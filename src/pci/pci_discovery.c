#include "pci/pci_discovery.h"
#include "pci/pci_modern_detect.h"
#include "virtioscsi.h"

BOOL DiscoverVirtIOSCSI(struct VirtIOSCSIBase *libBase)
{
    struct ExecIFace *IExec = libBase->IExec;
    struct PCIIFace *IPCI = libBase->IPCI;
    struct PCIDevice *device = NULL;

    if (!IPCI) {
        DPRINTF(IExec, "[virtioscsi] PCI_Discovery: IPCI interface not available.\n");
        return FALSE;
    }

    DPRINTF(IExec, "[virtioscsi] PCI_Discovery: Scanning for VirtIO SCSI controller...\n");

    /* Try modern VirtIO 1.0 SCSI first (0x1048), then legacy transitional (0x1004) */
    device = IPCI->FindDeviceTags(FDT_VendorID, 0x1AF4, FDT_DeviceID, 0x1048, TAG_DONE);

    if (!device) {
        device = IPCI->FindDeviceTags(FDT_VendorID, 0x1AF4, FDT_DeviceID, 0x1004, TAG_DONE);
    }

    if (!device) {
        DPRINTF(IExec, "[virtioscsi] PCI_Discovery: No VirtIO SCSI controller found.\n");
        return FALSE;
    }

    /* Read back IDs for the log (FindDeviceTags already matched them) */
    uint16 vendor = device->ReadConfigWord(PCI_VENDOR_ID);
    uint16 devid = device->ReadConfigWord(PCI_DEVICE_ID);

    uint8 bus, dev, fn;
    device->GetAddress(&bus, &dev, &fn);

    DPRINTF(IExec, "[virtioscsi:pci_discovery.c] PCI_Discovery: Found VirtIO SCSI (%04x:%04x) at %02x:%02x.%u\n",
            vendor, devid, (unsigned int)bus, (unsigned int)dev, (unsigned int)fn);

    /* Fetch BAR 0 (I/O) and BAR 4 (MMIO) */
    libBase->pciDevice = device;
    libBase->bar0 = device->GetResourceRange(0);
    libBase->bar4 = device->GetResourceRange(4);

    if (libBase->bar0) {
        const char *bar0_type = (libBase->bar0->Flags & PCI_RANGE_IO) ? "I/O" : "MEM";
        DPRINTF(IExec, "[virtioscsi:pci_discovery.c] PCI_Discovery: BAR0 (%s) mapped at Physical 0x%08lX, Size: %lu\n",
                bar0_type, libBase->bar0->Physical, libBase->bar0->Size);
    }

    if (libBase->bar4) {
        DPRINTF(IExec,
                "[virtioscsi:pci_discovery.c] PCI_Discovery: BAR4 (MMIO) mapped at Physical 0x%08lX, Size: %lu\n",
                libBase->bar4->Physical, libBase->bar4->Size);
    }

    /*
     * Keep the device handle live in libBase->pciDevice — do NOT call
     * IPCI->FreeDevice() here. BAR mappings must remain valid for the
     * driver's lifetime; they are released in _manager_Expunge().
     *
     * Only run modern VirtIO detection for non-transitional devices (0x1048).
     * Transitional 0x1004 devices expose vendor-specific PCI capabilities but
     * on AmigaOne the MMIO BAR reads all return 0 (Articia S hardware limit),
     * so modern_mode must stay FALSE to use the working legacy I/O path.
     */
    if (devid == 0x1048) {
        DetectModernVirtIO(libBase);
    } else {
        DPRINTF(IExec, "[virtioscsi:pci_discovery.c] Legacy device 0x%04X — skipping modern detection.\n",
                (unsigned int)devid);
    }

    return TRUE;
}
