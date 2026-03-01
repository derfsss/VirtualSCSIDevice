#include "pci/pci_modern_detect.h"
#include "virtio/virtio_pci_modern.h"
#include "virtioscsi.h"

/*
 * DetectModernVirtIO: walk the PCI capability list for VirtIO vendor-specific
 * capabilities (type 0x09).  Each such cap describes one configuration region
 * (COMMON, NOTIFY, ISR, DEVICE) located within a specific BAR at a given offset.
 *
 * If a COMMON_CFG cap is found, libBase->modern_mode is set TRUE and all
 * cfg_base fields are populated.  Otherwise modern_mode stays FALSE and the
 * caller will use the legacy init path.
 *
 * The notify_off_mult field defaults to 0 (shared notify address).  If the
 * NOTIFY_CFG cap is present, the actual multiplier is read from cap offset +16.
 */
BOOL DetectModernVirtIO(struct VirtIOSCSIBase *libBase)
{
    struct ExecIFace *IExec = libBase->IExec;
    struct PCIDevice *pciDev = libBase->pciDevice;

    libBase->modern_mode    = FALSE;
    libBase->common_cfg_base = 0;
    libBase->notify_cfg_base = 0;
    libBase->notify_off_mult = 0;
    libBase->isr_cfg_base    = 0;
    libBase->device_cfg_base = 0;

    if (!pciDev) {
        DPRINTF(IExec, "[virtioscsi:pci_modern_detect.c] No PCI device handle.\n");
        return FALSE;
    }

    struct PCICapability *cap = pciDev->GetFirstCapability();
    int guard = 0;

    while (cap && guard < 32) {
        guard++;

        if (cap->Type != PCI_CAPABILITYID_VENDOR) {
            cap = pciDev->GetNextCapability(cap);
            continue;
        }

        /* VirtIO vendor-specific capability */
        uint8 cfg_type = pciDev->ReadConfigByte(cap->CapOffset + VIRTIO_CAP_OFF_CFG_TYPE);
        uint8 bar_num  = pciDev->ReadConfigByte(cap->CapOffset + VIRTIO_CAP_OFF_BAR);
        uint32 offset  = pciDev->ReadConfigLong(cap->CapOffset + VIRTIO_CAP_OFF_OFFSET);

        struct PCIResourceRange *bar = pciDev->GetResourceRange(bar_num);
        if (!bar) {
            DPRINTF(IExec,
                    "[virtioscsi:pci_modern_detect.c] cfg_type=%u: BAR%u not available.\n",
                    (uint32)cfg_type, (uint32)bar_num);
            cap = pciDev->GetNextCapability(cap);
            continue;
        }

        /*
         * Use BaseAddress (CPU-visible mapped address), NOT Physical (PCI bus address).
         * On Pegasos2 (MV64361) these differ: Physical is the PCI-side address while
         * BaseAddress is where the CPU can actually reach the MMIO region.  Using
         * Physical causes all MMIO reads to return 0 and writes to be silently lost.
         */
        uint32 addr = bar->BaseAddress + offset;
        uint32 phys = bar->Physical + offset;

        switch (cfg_type) {
        case VIRTIO_PCI_CAP_COMMON_CFG:
            libBase->common_cfg_base = addr;
            libBase->modern_mode = TRUE;
            DPRINTF(IExec,
                    "[virtioscsi:pci_modern_detect.c] COMMON_CFG  BAR%u+0x%lX -> base=0x%08lX phys=0x%08lX\n",
                    (uint32)bar_num, offset, addr, phys);
            break;

        case VIRTIO_PCI_CAP_NOTIFY_CFG:
            libBase->notify_cfg_base = addr;
            /* Read the notify_off_multiplier (4 bytes at cap+16) */
            libBase->notify_off_mult = pciDev->ReadConfigLong(
                cap->CapOffset + VIRTIO_CAP_OFF_NOTIFY_MULT);
            DPRINTF(IExec,
                    "[virtioscsi:pci_modern_detect.c] NOTIFY_CFG  BAR%u+0x%lX -> base=0x%08lX phys=0x%08lX mult=%lu\n",
                    (uint32)bar_num, offset, addr, phys, libBase->notify_off_mult);
            break;

        case VIRTIO_PCI_CAP_ISR_CFG:
            libBase->isr_cfg_base = addr;
            DPRINTF(IExec,
                    "[virtioscsi:pci_modern_detect.c] ISR_CFG     BAR%u+0x%lX -> base=0x%08lX phys=0x%08lX\n",
                    (uint32)bar_num, offset, addr, phys);
            break;

        case VIRTIO_PCI_CAP_DEVICE_CFG:
            libBase->device_cfg_base = addr;
            DPRINTF(IExec,
                    "[virtioscsi:pci_modern_detect.c] DEVICE_CFG  BAR%u+0x%lX -> base=0x%08lX phys=0x%08lX\n",
                    (uint32)bar_num, offset, addr, phys);
            break;

        default:
            DPRINTF(IExec,
                    "[virtioscsi:pci_modern_detect.c] Unknown VirtIO cfg_type=%u (ignored)\n",
                    (uint32)cfg_type);
            break;
        }

        cap = pciDev->GetNextCapability(cap);
    }

    DPRINTF(IExec, "[virtioscsi:pci_modern_detect.c] VirtIO mode: %s\n",
            libBase->modern_mode ? "MODERN" : "LEGACY");

    return libBase->modern_mode;
}
