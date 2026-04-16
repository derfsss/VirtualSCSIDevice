#include "virtio/virtio_init.h"
#include "virtio/virtio_pci_modern.h"
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

/* Forward declaration — defined below */
static BOOL InitVirtIOSCSI_Modern(struct VirtIOSCSIBase *libBase);

BOOL InitVirtIOSCSI(struct VirtIOSCSIBase *libBase)
{
    struct ExecIFace *IExec = libBase->IExec;
    struct PCIDevice *pciDev = libBase->pciDevice;

    if (!pciDev) {
        DPRINTF(IExec, "[virtioscsi] InitVirtIO: No PCI device handle.\n");
        return FALSE;
    }

    /* Dispatch to the appropriate init path.
     * Modern devices (0x1048) may have no BAR0 — they use MMIO via BAR4.
     * Check modern_mode before requiring BAR0. */
    if (libBase->modern_mode)
        return InitVirtIOSCSI_Modern(libBase);

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
     * Feature negotiation.
     *
     * VIRTIO_F_EVENT_IDX (bit 29): accepted. Enables interrupt coalescing
     * via used_event (driver writes target used->idx into avail->ring[num]).
     * The kick-suppression half (avail_event) is NOT used — QEMU legacy mode
     * never writes avail_event so it stays 0, which would suppress all kicks
     * after the first. VirtIOSCSI_Kick always sends an unconditional notify.
     *
     * VIRTIO_F_INDIRECT_DESC (bit 28): NOT accepted. QEMU legacy VirtIO reads
     * indirect descriptor entries as little-endian; our PPC guest fills them
     * in native big-endian order, producing garbage lengths ("wrong size for
     * virtio-scsi headers"). The direct chained-descriptor path is used
     * instead and supports up to MAX_SG_ENTRIES=64 per request.
     */
    uint32 guest_features = host_features & (1UL << 29); /* EVENT_IDX only */
    BOOL use_event_idx  = (guest_features & (1UL << 29)) != 0;
    BOOL use_indirect   = FALSE; /* disabled — endianness incompatibility */
    DPRINTF(IExec, "[virtioscsi] InitVirtIO: Guest features: 0x%08lX%s\n", guest_features,
            use_event_idx ? " EVENT_IDX" : "");
    (void)use_indirect; /* suppress unused-variable warning */
    pciDev->OutLong(iobase + VIRTIO_PCI_GUEST_FEATURES, guest_features);

    /* Step 7: VirtQueue Setup — 3 queues: 0=controlq, 1=eventq, 2=requestq */
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

/*
 * InitVirtIOSCSI_Modern: Modern VirtIO 1.0 initialization sequence.
 *
 * Uses the common config region mapped via the vendor-specific PCI capability
 * chain.  Registers are little-endian; pciDev->SetEndian(PCI_MODE_LITTLE_ENDIAN)
 * is called once at entry and persists for the life of this PCIDevice handle.
 *
 * Queue address registration uses three 32-bit register pairs (DESC/AVAIL/USED
 * low+high) instead of a single legacy PFN.  Physical addresses are 32-bit only
 * (high halves written as 0) since AmigaOS 4.1 operates in a 32-bit address space.
 */
static BOOL InitVirtIOSCSI_Modern(struct VirtIOSCSIBase *libBase)
{
    struct ExecIFace *IExec = libBase->IExec;
    struct PCIDevice *pciDev = libBase->pciDevice;
    uint32 base = libBase->common_cfg_base;

    DPRINTF(IExec, "[virtioscsi:virtio_init.c] InitVirtIO_Modern: common_cfg=0x%08lX\n", base);

    /*
     * Enable PCI Memory Space access and Bus Master before any MMIO.
     * Without this, all MMIO reads return 0 and writes are silently lost.
     * test_modern.c does this and works; the driver was missing it.
     */
    {
        uint16 pci_cmd = pciDev->ReadConfigWord(PCI_COMMAND);
        if (!(pci_cmd & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER))) {
            pciDev->WriteConfigWord(PCI_COMMAND, pci_cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
            DPRINTF(IExec, "[virtioscsi:virtio_init.c] InitVirtIO_Modern: Enabled PCI Memory+BusMaster (was 0x%04lX)\n",
                    (uint32)pci_cmd);
        }
        uint32 caps = pciDev->GetCapabilities();
        if (!(caps & PCI_CAP_BUSMASTER)) {
            pciDev->SetCapabilities(PCI_CAP_BUSMASTER | PCI_CAP_SETCLR);
            DPRINTF(IExec, "[virtioscsi:virtio_init.c] InitVirtIO_Modern: Set AmigaOS BusMaster capability\n");
        }
    }

    /*
     * All MMIO access uses stwbrx/lwbrx/stb/lbz inline asm macros
     * (mmio_r8/w8/r16/w16/r32/w32).  PCI accessor methods (InByte etc.)
     * do NOT work for MMIO BAR addresses on Pegasos2.
     */

    /* Step 1: Reset — write 0 then poll until device acknowledges */
    mmio_w8(pciDev, base + VIRTIO_PCI_COMMON_STATUS, 0x00);
    {
        uint32 tries = 0;
        uint8 rst;
        do {
            rst = mmio_r8(pciDev, base + VIRTIO_PCI_COMMON_STATUS);
            tries++;
        } while (rst != 0 && tries < 1000);

        if (rst != 0) {
            DPRINTF(IExec, "[virtioscsi:virtio_init.c] InitVirtIO_Modern: Reset did not complete (status=0x%02X)\n",
                    (uint32)rst);
        }
    }

    /* Step 2: ACKNOWLEDGE */
    mmio_w8(pciDev, base + VIRTIO_PCI_COMMON_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);

    /* Step 3: DRIVER */
    mmio_w8(pciDev, base + VIRTIO_PCI_COMMON_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Step 4: Feature negotiation. */
    mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_DFSELECT, 0);
    uint32 dev_feat_lo = mmio_r32(pciDev, base + VIRTIO_PCI_COMMON_DF);

    mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_DFSELECT, 1);
    uint32 dev_feat_hi = mmio_r32(pciDev, base + VIRTIO_PCI_COMMON_DF);

    DPRINTF(IExec, "[virtioscsi:virtio_init.c] InitVirtIO_Modern: Device features hi=0x%08lX lo=0x%08lX\n",
            dev_feat_hi, dev_feat_lo);

    /*
     * Accept on modern MMIO path:
     *   - VIRTIO_SCSI_F_HOTPLUG (bit 1): async hotplug events on eventq.
     *   - VIRTIO_SCSI_F_CHANGE  (bit 2): async param-change events on eventq.
     *   - VIRTIO_F_INDIRECT_DESC (bit 28): one vring slot per SG chain.
     *   - VIRTIO_F_EVENT_IDX    (bit 29): interrupt coalescing.
     *   - VIRTIO_F_VERSION_1    (bit 0 high = bit 32): mandatory for modern.
     *
     * INDIRECT_DESC was previously rejected due to a legacy-mode endianness
     * asymmetry (QEMU read indirect tables LE while PPC wrote BE).  On the
     * modern MMIO path both sides agree on LE, and virtqueue.c wraps all
     * indirect-table writes with vr64/vr32/vr16, so it's safe to enable.
     */
    /* DEBUG: HOTPLUG (bit 1) + CHANGE (bit 2) deliberately NOT negotiated
     * to isolate the SFS 1.290 mount-hang.  If SFS still crashes with these
     * off, the bug is unrelated to the v1.9 event-queue feature set. */
    uint32 drv_feat_lo = dev_feat_lo & ((1UL << 28)  /* INDIRECT_DESC */
                                       | (1UL << 29)  /* EVENT_IDX */);
    uint32 drv_feat_hi = dev_feat_hi & 1UL;          /* VIRTIO_F_VERSION_1 */

    mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_DFSELECTG, 0);
    mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_DFG, drv_feat_lo);

    mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_DFSELECTG, 1);
    mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_DFG, drv_feat_hi);

    DPRINTF(IExec, "[virtioscsi:virtio_init.c] InitVirtIO_Modern: Driver features hi=0x%08lX lo=0x%08lX\n",
            drv_feat_hi, drv_feat_lo);
    BOOL use_event_idx = (drv_feat_lo & (1UL << 29)) != 0;
    BOOL use_indirect  = (drv_feat_lo & (1UL << 28)) != 0;
    libBase->events_enabled = (drv_feat_lo & ((1UL << 1) | (1UL << 2))) != 0;

    /* Step 5: Set FEATURES_OK */
    mmio_w8(pciDev, base + VIRTIO_PCI_COMMON_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);

    /* Step 6: Verify FEATURES_OK is still set — device may reject features */
    uint8 status_check = mmio_r8(pciDev, base + VIRTIO_PCI_COMMON_STATUS);
    if (!(status_check & VIRTIO_STATUS_FEATURES_OK)) {
        DPRINTF(IExec, "[virtioscsi:virtio_init.c] InitVirtIO_Modern: FEATURES_OK rejected! Status=0x%02X\n",
                (uint32)status_check);
        mmio_w8(pciDev, base + VIRTIO_PCI_COMMON_STATUS, VIRTIO_STATUS_FAILED);
        return FALSE;
    }
    DPRINTF(IExec, "[virtioscsi:virtio_init.c] InitVirtIO_Modern: FEATURES_OK accepted. Status=0x%02X\n",
            (uint32)status_check);

    /* Step 7: VirtQueue Setup — queues 0 (controlq), 1 (eventq), 2 (requestq) */
    uint16 q;
    for (q = 0; q <= 2; q++) {
        mmio_w16(pciDev, base + VIRTIO_PCI_COMMON_Q_SELECT, q);

        uint16 q_max = mmio_r16(pciDev, base + VIRTIO_PCI_COMMON_Q_SIZE);
        if (q_max == 0) {
            DPRINTF(IExec, "[virtioscsi:virtio_init.c] InitVirtIO_Modern: Queue %u unavailable.\n", (uint32)q);
            continue;
        }

        DPRINTF(IExec, "[virtioscsi:virtio_init.c] InitVirtIO_Modern: Queue %u max size: %u\n",
                (uint32)q, (uint32)q_max);

        struct virtqueue *vq = VirtQueue_Allocate(IExec, q, q_max);
        if (!vq) {
            DPRINTF(IExec, "[virtioscsi:virtio_init.c] InitVirtIO_Modern: Failed to allocate Queue %u.\n", (uint32)q);
            return FALSE;
        }

        /* DMA-map the vring to obtain physical addresses */
        uint32 vring_entries = IExec->StartDMA(vq->desc, vq->mem_size, DMA_ReadFromRAM);
        if (vring_entries == 0) {
            DPRINTF(IExec, "[virtioscsi:virtio_init.c] InitVirtIO_Modern: StartDMA failed for queue %u\n", (uint32)q);
            VirtQueue_Free(IExec, vq);
            return FALSE;
        }

        struct DMAEntry *vring_dma = (struct DMAEntry *)IExec->AllocSysObjectTags(
            ASOT_DMAENTRY, ASODMAE_NumEntries, vring_entries, TAG_DONE);
        if (!vring_dma) {
            IExec->EndDMA(vq->desc, vq->mem_size, DMA_ReadFromRAM | DMAF_NoModify);
            VirtQueue_Free(IExec, vq);
            return FALSE;
        }

        IExec->GetDMAList(vq->desc, vq->mem_size, DMA_ReadFromRAM, vring_dma);
        uint32 desc_phys = (uint32)vring_dma[0].PhysicalAddress;
        IExec->FreeSysObject(ASOT_DMAENTRY, vring_dma);

        /* Keep the DMA mapping live — EndDMA called in VirtQueue_Free */
        vq->dma_phys    = desc_phys;
        vq->dma_entries = vring_entries;

        /*
         * Compute physical addresses of avail and used rings (contiguous layout,
         * Section 2.7.2): desc table at base, avail immediately after,
         * used ring page-aligned after avail.
         */
        uint32 desc_size   = sizeof(struct vring_desc) * q_max;
        uint32 avail_size  = sizeof(uint16) * (3 + q_max);
        uint32 used_offset = (desc_size + avail_size + 4095U) & ~4095U;

        vq->avail_phys = desc_phys + desc_size;
        vq->used_phys  = desc_phys + used_offset;

        /* Register the three ring addresses in the common config */
        mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_Q_DESCLO,  desc_phys);
        mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_Q_DESCHI,  0);
        mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_Q_AVAILLO, vq->avail_phys);
        mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_Q_AVAILHI, 0);
        mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_Q_USEDLO,  vq->used_phys);
        mmio_w32(pciDev, base + VIRTIO_PCI_COMMON_Q_USEDHI,  0);

        /* Enable the queue */
        mmio_w16(pciDev, base + VIRTIO_PCI_COMMON_Q_ENABLE, 1);

        /*
         * Read per-queue notify offset and compute the notification address.
         * notify addr = notify_cfg_base + Q_NOFF * notify_off_mult.
         * If notify_off_mult == 0, all queues share the base address.
         */
        uint16 q_noff = mmio_r16(pciDev, base + VIRTIO_PCI_COMMON_Q_NOFF);
        uint32 notify_mult = libBase->notify_off_mult;
        vq->notify_addr = libBase->notify_cfg_base +
                          (notify_mult ? (uint32)q_noff * notify_mult : 0);

        vq->modern = TRUE;
        vq->use_event_idx = use_event_idx;
        vq->last_kick_avail_idx = 0xFFFF; /* ensures first kick always fires */
        /* VQ1 (eventq) always uses direct descriptors: each event buffer is a
         * single writable region, no SG chain, no indirect benefit.  For VQ0
         * (controlq) and VQ2 (requestq) use indirect when negotiated. */
        vq->use_indirect = (q == 1) ? FALSE : use_indirect;

        libBase->vqs[q] = vq;

        DPRINTF(IExec,
                "[virtioscsi:virtio_init.c] InitVirtIO_Modern: Q%u enabled  desc=0x%08lX avail=0x%08lX used=0x%08lX notify=0x%08lX\n",
                (uint32)q, desc_phys, vq->avail_phys, vq->used_phys, vq->notify_addr);
    }

    /* Step 8: Set DRIVER_OK */
    mmio_w8(pciDev, base + VIRTIO_PCI_COMMON_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
            VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    uint8 final_status = mmio_r8(pciDev, base + VIRTIO_PCI_COMMON_STATUS);
    DPRINTF(IExec, "[virtioscsi:virtio_init.c] InitVirtIO_Modern: Complete. Status=0x%02X\n",
            (uint32)final_status);

    return TRUE;
}

void CleanupVirtIOSCSI(struct VirtIOSCSIBase *libBase)
{
    struct ExecIFace *IExec = libBase->IExec;
    struct PCIDevice *pciDev = libBase->pciDevice;

    if (pciDev) {
        /* Reset the device to stop all DMA */
        if (libBase->modern_mode && libBase->common_cfg_base) {
            mmio_w8(pciDev, libBase->common_cfg_base + VIRTIO_PCI_COMMON_STATUS, 0x00);
        } else if (libBase->bar0) {
            uint32 iobase = (uint32)libBase->bar0->Physical;
            pciDev->OutByte(iobase + VIRTIO_PCI_STATUS, 0x00);
        }
        DPRINTF(IExec, "[virtioscsi:virtio_init.c] VirtIO: Hardware Reset Issued.\n");
    }

    for (int i = 0; i <= 2; i++) {
        if (libBase->vqs[i]) {
            VirtQueue_Free(IExec, libBase->vqs[i]);
            libBase->vqs[i] = NULL;
        }
    }
}
