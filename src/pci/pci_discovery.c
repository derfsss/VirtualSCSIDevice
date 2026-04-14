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

    /* Try transitional VirtIO SCSI first (0x1004), then modern-only (0x1048).
     * Transitional devices work on ALL QEMU machines: the driver auto-detects
     * whether to use legacy I/O or modern MMIO based on hardware capability.
     * Modern-only 0x1048 is kept as fallback for existing Pegasos2 setups
     * using -device virtio-scsi-pci-non-transitional. */
    device = IPCI->FindDeviceTags(FDT_VendorID, 0x1AF4, FDT_DeviceID, 0x1004, TAG_DONE);

    if (!device) {
        device = IPCI->FindDeviceTags(FDT_VendorID, 0x1AF4, FDT_DeviceID, 0x1048, TAG_DONE);
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

    /* AmigaOne firmware-chain workaround for 64-bit BAR high DWORD.
     *
     * On AmigaOne with QEMU 10.2.2, the VirtIO modern MMIO BAR (BAR4) is
     * a 64-bit prefetchable memory BAR.  BBoot does not write to the high
     * DWORD, and AmigaOS's later PCI enumerator performs a classic sizing
     * probe (write 0xffffffff, read size, write address back) but fails to
     * write 0 back to the high DWORD.  Result: BAR5 (config offset 0x24)
     * sits at 0xffffffff, placing BAR4 at 0xffffffff84204000 — outside
     * Articia's decoded PCI memory window, so MMIO reads return 0xff and
     * writes are dropped.
     *
     * Fix it at the source: read BAR5; if it's 0xffffffff, write 0 back.
     * This must happen BEFORE GetResourceRange(4) so the AmigaOS PCI
     * library reads a sane high DWORD when computing the BAR's CPU-visible
     * address.  Pegasos2 / SAM460ex are unaffected (VOF programs BAR5=0).
     */
    uint32 bar5 = device->ReadConfigLong(0x24);
    if (bar5 == 0xFFFFFFFFUL) {
        DPRINTF(IExec,
                "[virtioscsi:pci_discovery.c] BAR5 high DWORD is 0xffffffff (AmigaOne PCI probe bug), zeroing.\n");
        device->WriteConfigLong(0x24, 0);
        uint32 bar5_after = device->ReadConfigLong(0x24);
        DPRINTF(IExec,
                "[virtioscsi:pci_discovery.c] BAR5 after fix: 0x%08lX\n", (unsigned long)bar5_after);
    }

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
     * Attempt modern VirtIO detection for ALL device types.
     *
     * Transitional devices (0x1004) expose vendor-specific PCI capabilities
     * for modern mode alongside their legacy I/O interface.  DetectModernVirtIO
     * walks the capability chain and probes MMIO to verify it actually works
     * on this platform's PCI bridge:
     *
     *   Pegasos2 (MV64361 transparent bridge): MMIO probe passes → modern mode
     *   AmigaOne (Articia S floating buffer):  MMIO probe fails  → legacy I/O
     *
     * Non-transitional devices (0x1048) also go through the probe; if MMIO
     * works the driver uses modern mode, otherwise init will fail gracefully.
     */
    DetectModernVirtIO(libBase);

    return TRUE;
}
