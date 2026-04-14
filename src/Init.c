#include "pci/pci_discovery.h"
#include "unit_discovery.h"
#include "virtio/virtio_events.h"
#include "virtio/virtio_init.h"
#include "virtio/virtio_irq.h"
#include "virtioscsi.h"
#include <exec/exec.h>

extern const APTR devInterfaces[];

#include "version.h"

struct Library *_manager_Init(struct Library *library, BPTR seglist, struct Interface *exec)
{
    struct VirtIOSCSIBase *devBase = (struct VirtIOSCSIBase *)library;
    struct ExecIFace *iexec = (struct ExecIFace *)exec;

    iexec->DebugPrintF("[virtioscsi] Loading Version: %s\n", DEVVERSIONSTRING_FULL);

    devBase->IExec = iexec;
    devBase->dev_SegList = seglist;

    iexec->InitSemaphore(&devBase->io_lock);

    for (int i = 0; i < 8; i++) {
        devBase->units[i] = NULL;
    }

    /* Open expansion.library and get PCI interface */
    devBase->ExpansionBase = iexec->OpenLibrary("expansion.library", 54);
    if (!devBase->ExpansionBase) {
        DPRINTF(iexec, "[virtioscsi:Init.c] Init: Failed to open expansion.library v54.\n");
        return NULL;
    }
    devBase->IPCI = (struct PCIIFace *)iexec->GetInterface(devBase->ExpansionBase, "pci", 1, NULL);
    if (!devBase->IPCI) {
        DPRINTF(iexec, "[virtioscsi:Init.c] Init: Failed to get IPCI interface.\n");
        iexec->CloseLibrary(devBase->ExpansionBase);
        return NULL;
    }

    /* Discover VirtIO SCSI PCI device */
    if (!DiscoverVirtIOSCSI(devBase)) {
        iexec->DropInterface((struct Interface *)devBase->IPCI);
        iexec->CloseLibrary(devBase->ExpansionBase);
        return NULL;
    }

    /* Open utility.library */
    devBase->UtilityBase = iexec->OpenLibrary("utility.library", 50);
    if (!devBase->UtilityBase) {
        DPRINTF(iexec, "[virtioscsi:Init.c] Init: Failed to open utility.library v50.\n");
        iexec->DropInterface((struct Interface *)devBase->IPCI);
        iexec->CloseLibrary(devBase->ExpansionBase);
        return NULL;
    }
    devBase->IUtility = (struct UtilityIFace *)iexec->GetInterface(devBase->UtilityBase, "main", 1, NULL);
    if (!devBase->IUtility) {
        DPRINTF(iexec, "[virtioscsi:Init.c] Init: Failed to get IUtility interface.\n");
        iexec->CloseLibrary(devBase->UtilityBase);
        iexec->DropInterface((struct Interface *)devBase->IPCI);
        iexec->CloseLibrary(devBase->ExpansionBase);
        return NULL;
    }

    /* Initialize VirtIO queues */
    if (!InitVirtIOSCSI(devBase)) {
        CleanupVirtIOSCSI(devBase);
        iexec->DropInterface((struct Interface *)devBase->IUtility);
        iexec->CloseLibrary(devBase->UtilityBase);
        iexec->DropInterface((struct Interface *)devBase->IPCI);
        iexec->CloseLibrary(devBase->ExpansionBase);
        return NULL;
    }

    /* Install PCI interrupt handler (non-fatal — falls back to polling on failure) */
    if (!InstallVirtIOInterrupt(devBase)) {
        DPRINTF(iexec, "[virtioscsi:Init.c] Init: Interrupt install failed, using polling fallback.\n");
    }

    /*
     * Make device public. Under RTF_AUTOINIT | RTF_NATIVE, the kernel
     * handles registration. Manual AddDevice would cause double registration.
     */
    devBase->dev_Base.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    devBase->dev_Base.dd_Library.lib_Node.ln_Pri = 0;
    devBase->dev_Base.dd_Library.lib_Node.ln_Name = DEVNAME;
    devBase->dev_Base.dd_Library.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    devBase->dev_Base.dd_Library.lib_Version = DEVVER;
    devBase->dev_Base.dd_Library.lib_Revision = DEVREV;
    devBase->dev_Base.dd_Library.lib_IdString = DEVVERSIONSTRING;

    /* Scan SCSI targets and announce to mounter.library */
    DiscoverUnits(devBase);

    /* Start VirtIO event queue (HOTPLUG + PARAM_CHANGE).  Non-fatal: if it
     * fails to start, static discovery still provides the boot-time disks. */
    InitEventQueue(devBase);

    DPRINTF(iexec, "[virtioscsi:Init.c] Init: Device library initialized and public.\n");

    return (struct Library *)devBase;
}
