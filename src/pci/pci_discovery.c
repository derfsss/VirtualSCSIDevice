#include "pci/pci_discovery.h"
#include "virtioscsi.h"

BOOL DiscoverVirtIOSCSI(struct VirtIOSCSIBase *libBase)
{
    struct ExecIFace *IExec = libBase->IExec;
    struct PCIIFace *IPCI = libBase->IPCI;
    struct PCIDevice *device = NULL;
    BOOL found = FALSE;

    if (!IPCI) {
        DPRINTF(IExec, "[virtioscsi] PCI_Discovery: IPCI interface not available.\n");
        return FALSE;
    }

    DPRINTF(IExec, "[virtioscsi] PCI_Discovery: Scanning for VirtIO SCSI controller (0x1AF4/0x1004)...\n");

    /* Directly search for the VirtIO SCSI controller utilizing the TagList */
    device = IPCI->FindDeviceTags(FDT_VendorID, 0x1AF4, FDT_DeviceID, 0x1004, TAG_DONE);

    if (!device) {
        DPRINTF(IExec, "[virtioscsi] PCI_Discovery: No VirtIO SCSI controller found.\n");
        return FALSE;
    }

    /* Verify Vendor and Device IDs (optional since FindDeviceTags filtered it, but good for logs) */
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

    found = TRUE;
    /* We intentionally leave the device handle active in libBase->pciDevice.
       We do NOT call IPCI->FreeDevice(device) here because we need the BARs to remain
       active during the device driver's lifetime. It is freed in _manager_Expunge. */

    return found;
}
