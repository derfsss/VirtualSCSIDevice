#include "virtio/virtio_init.h"
#include "virtio/virtio_scsi.h"
#include "virtio/virtqueue.h"
#include "virtioscsi.h"

/*
 * VirtIO Legacy PCI I/O Port Access
 *
 * BAR0 on a legacy VirtIO PCI device is an I/O BAR, NOT memory-mapped.
 * Direct pointer dereference causes a DSI exception (page fault).
 * We MUST use the PCIDevice's InByte/OutByte/InWord/OutWord/InLong/OutLong
 * methods which perform proper PCI I/O cycles.
 *
 * The 'port' parameter is the physical I/O port address (BAR0 base + offset).
 *
 * Endianness: The Legacy PCI configuration registers are Little Endian.
 * The PCIDevice I/O methods handle byte-swapping transparently on
 * Big Endian PowerPC, so we pass native values directly.
 */

BOOL InitVirtIOSCSI(struct VirtIOSCSIBase *libBase)
{
    struct ExecIFace *IExec = libBase->IExec;
    struct PCIDevice *pciDev = libBase->pciDevice;

    if (!pciDev) {
        DPRINTF(IExec, "[virtioscsi] InitVirtIO: No PCI device handle.\n");
        return FALSE;
    }

    if (!libBase->bar0) {
        DPRINTF(IExec, "[virtioscsi] InitVirtIO: BAR0 not available.\n");
        return FALSE;
    }

    /* BAR0 base is the I/O port base address */
    uint32 iobase = (uint32)libBase->bar0->Physical;

    DPRINTF(IExec, "[virtioscsi] InitVirtIO: Starting Legacy PCI sequence at I/O port 0x%08lX\n", iobase);

    /*
     * VirtIO Legacy Initialization Sequence (Section 3.1.2)
     * Legacy devices MUST omit steps 5 (FEATURES_OK) and 6 (verify FEATURES_OK).
     *
     * 1. Reset device (write 0 to status)
     * 2. Set ACKNOWLEDGE status bit
     * 3. Set DRIVER status bit
     * 4. Read/write feature bits
     * 5. (OMITTED in Legacy) Set FEATURES_OK
     * 6. (OMITTED in Legacy) Verify FEATURES_OK
     * 7. Set up virtqueues
     * 8. Set DRIVER_OK status bit
     */

    /* Step 1: Reset Device */
    pciDev->OutByte(iobase + VIRTIO_PCI_STATUS, 0x00);

    /* Clear any pending interrupts */
    pciDev->InByte(iobase + VIRTIO_PCI_ISR);

    /* Step 2: Set ACKNOWLEDGE (Section 2.1.1: must not clear previously set bits) */
    pciDev->OutByte(iobase + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);

    /* Step 3: Set DRIVER */
    pciDev->OutByte(iobase + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Step 4: Feature Negotiation (Legacy: only bits 0-31 accessible) */
    uint32 host_features = pciDev->InLong(iobase + VIRTIO_PCI_HOST_FEATURES);
    DPRINTF(IExec, "[virtioscsi] InitVirtIO: Host features: 0x%08lX\n", host_features);

    /* Decode and log each feature bit */
    static const struct
    {
        uint32 bit;
        const char *name;
    } feature_names[] = {
        {0, "VIRTIO_SCSI_F_INOUT (bidirectional)"},
        {1, "VIRTIO_SCSI_F_HOTPLUG (hotplug events)"},
        {2, "VIRTIO_SCSI_F_CHANGE (LUN param changes)"},
        {3, "VIRTIO_SCSI_F_T10_PI (T10 protection info)"},
        {24, "VIRTIO_F_NOTIFY_ON_EMPTY"},
        {27, "VIRTIO_F_ANY_LAYOUT"},
        {28, "VIRTIO_F_INDIRECT_DESC"},
        {29, "VIRTIO_F_EVENT_IDX"},
        {30, "VIRTIO_F_UNUSED (reserved)"},
    };

    uint32 i;
    for (i = 0; i < sizeof(feature_names) / sizeof(feature_names[0]); i++) {
        if (host_features & (1UL << feature_names[i].bit)) {
            DPRINTF(IExec, "[virtioscsi]   bit %2lu: %s\n", feature_names[i].bit, feature_names[i].name);
        }
    }

    /*
     * Feature negotiation: accept VIRTIO_F_EVENT_IDX (bit 29) if offered.
     *
     * Previous bug: avail_event in the used ring is 0 at init time, and
     * last_kick_avail_idx was also 0, so the suppression condition
     * (new_idx - avail_event - 1) < (new_idx - old_idx) evaluated FALSE
     * for the second kick — silently dropping all I/O after T0 L0.
     *
     * Fix: initialise last_kick_avail_idx = 0xFFFF so that
     * (new_idx - old_idx) wraps to a large uint16, making the first
     * comparison always TRUE until the device writes a real avail_event.
     */
    /* Accept EVENT_IDX (bit 29) and INDIRECT_DESC (bit 28) if offered */
    uint32 guest_features = host_features & ((1UL << 29) | (1UL << 28));
    BOOL use_event_idx  = (guest_features & (1UL << 29)) != 0;
    BOOL use_indirect   = (guest_features & (1UL << 28)) != 0;
    DPRINTF(IExec, "[virtioscsi] InitVirtIO: Guest features: 0x%08lX%s%s\n", guest_features,
            use_event_idx ? " EVENT_IDX" : "",
            use_indirect  ? " INDIRECT_DESC" : "");
    pciDev->OutLong(iobase + VIRTIO_PCI_GUEST_FEATURES, guest_features);

    /* Step 7: VirtQueue Setup */
    /* VirtIO SCSI has 3 queues: 0=controlq, 1=eventq, 2=requestq */
    for (uint16 q = 0; q <= 2; q++) {
        /* Select the queue */
        pciDev->OutWord(iobase + VIRTIO_PCI_QUEUE_SEL, q);

        /* Read queue size (max number of entries) */
        uint16 q_max = pciDev->InWord(iobase + VIRTIO_PCI_QUEUE_NUM);

        if (q_max == 0) {
            DPRINTF(IExec, "[virtioscsi:virtio_init.c] InitVirtIO: Queue %u unavailable.\n", q);
            continue;
        }

        DPRINTF(IExec, "[virtioscsi:virtio_init.c] InitVirtIO: Queue %u max size: %u\n", q, q_max);

        struct virtqueue *vq = VirtQueue_Allocate(IExec, q, q_max);
        if (!vq) {
            DPRINTF(IExec, "[virtioscsi:virtio_init.c] InitVirtIO: Failed to allocate Queue %u.\n", q);
            return FALSE;
        }

        /*
         * Legacy PCI: Write the Page Frame Number (physical address / 4096)
         * to VIRTIO_PCI_QUEUE_PFN. The device uses the legacy vring layout
         * formula (Section 2.7.2) to locate desc/avail/used within.
         *
         * Use StartDMA + GetDMAList (modern replacements for deprecated
         * CachePreDMA/CachePostDMA) to obtain the physical address.
         * The vring is a single contiguous MEMF_SHARED allocation so
         * entry [0] holds the full physical base address.
         */
        uint32 vring_entries = IExec->StartDMA(vq->desc, vq->mem_size, DMA_ReadFromRAM);
        if (vring_entries == 0) {
            DPRINTF(IExec, "[virtioscsi:virtio_init.c] InitVirtIO: StartDMA failed for queue %u\n", q);
            return FALSE;
        }
        struct DMAEntry *vring_dma = (struct DMAEntry *)IExec->AllocSysObjectTags(
            ASOT_DMAENTRY, ASODMAE_NumEntries, vring_entries, TAG_DONE);
        if (!vring_dma) {
            IExec->EndDMA(vq->desc, vq->mem_size, DMA_ReadFromRAM | DMAF_NoModify);
            return FALSE;
        }
        IExec->GetDMAList(vq->desc, vq->mem_size, DMA_ReadFromRAM, vring_dma);
        uint32 phys_addr = (uint32)vring_dma[0].PhysicalAddress;
        IExec->FreeSysObject(ASOT_DMAENTRY, vring_dma);
        /* Keep the DMA mapping live for the device's lifetime — EndDMA on cleanup */
        vq->dma_phys = phys_addr;
        vq->dma_entries = vring_entries;
        uint32 pfn = phys_addr / 4096;
        pciDev->OutLong(iobase + VIRTIO_PCI_QUEUE_PFN, pfn);

        vq->use_event_idx = use_event_idx;
        /* 0xFFFF ensures the first kick always fires regardless of avail_event */
        vq->last_kick_avail_idx = 0xFFFF;
        vq->use_indirect = use_indirect;
        libBase->vqs[q] = vq;
        DPRINTF(IExec,
                "[virtioscsi:virtio_init.c] InitVirtIO: Queue %u configured, virt=0x%08lX phys=0x%08lX PFN=0x%08lX\n",
                q, (uint32)vq->desc, phys_addr, pfn);
    }

    /* Step 8: Set DRIVER_OK (no FEATURES_OK in Legacy mode) */
    pciDev->OutByte(iobase + VIRTIO_PCI_STATUS,
                    VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    /* Verify final status */
    uint8 final_status = pciDev->InByte(iobase + VIRTIO_PCI_STATUS);
    DPRINTF(IExec, "[virtioscsi:virtio_init.c] InitVirtIO: Complete. Status=0x%02X\n", final_status);

    return TRUE;
}

void CleanupVirtIOSCSI(struct VirtIOSCSIBase *libBase)
{
    struct ExecIFace *IExec = libBase->IExec;
    struct PCIDevice *pciDev = libBase->pciDevice;

    if (pciDev && libBase->bar0) {
        uint32 iobase = (uint32)libBase->bar0->Physical;
        /* Reset the device to stop all DMA */
        pciDev->OutByte(iobase + VIRTIO_PCI_STATUS, 0x00);
        DPRINTF(IExec, "[virtioscsi:virtio_init.c] VirtIO: Hardware Reset Issued.\n");
    }

    for (int i = 0; i <= 2; i++) {
        if (libBase->vqs[i]) {
            VirtQueue_Free(IExec, libBase->vqs[i]);
            libBase->vqs[i] = NULL;
        }
    }
}
