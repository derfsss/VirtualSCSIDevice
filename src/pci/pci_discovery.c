#include "pci/pci_discovery.h"
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

    DPRINTF(IExec, "[virtioscsi] PCI_Discovery: Scanning for VirtIO SCSI controller (0x1AF4/0x1004)...\n");

    /* Search for the VirtIO SCSI controller by PCI vendor:device ID */
    device = IPCI->FindDeviceTags(FDT_VendorID, 0x1AF4, FDT_DeviceID, 0x1004, TAG_DONE);

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
        DPRINTF(IExec, "[virtioscsi:pci_discovery.c] PCI_Discovery: BAR0 (I/O) mapped at Physical 0x%08lX, Size: %lu\n",
                libBase->bar0->Physical, libBase->bar0->Size);
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
     */
    return TRUE;
}
