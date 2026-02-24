#include "virtio/virtio_init.h"
#include "virtio/virtio_irq.h"
#include "virtioscsi.h"
#include "unit_task.h"
#include <exec/exec.h>

BPTR _manager_Expunge(struct DeviceManagerInterface *Self)
{
    struct VirtIOSCSIBase *devBase = (struct VirtIOSCSIBase *)Self->Data.LibBase;
    BPTR seglist = (BPTR)NULL;
    struct ExecIFace *IExec = devBase->IExec;

    DPRINTF(IExec, "[virtioscsi:Expunge.c] Expunge: Entering with OpenCnt = %u\n",
            devBase->dev_Base.dd_Library.lib_OpenCnt);

    if (devBase->dev_Base.dd_Library.lib_OpenCnt == 0) {
        seglist = devBase->dev_SegList;

        IExec->Remove((struct Node *)devBase);
        DPRINTF(IExec, "[virtioscsi:Expunge.c] Expunge: Removed from device list.\n");

        /* Remove interrupt handler before resetting VirtIO hardware */
        DPRINTF(IExec, "[virtioscsi:Expunge.c] Expunge: Removing VirtIO interrupt handler.\n");
        RemoveVirtIOInterrupt(devBase);

        DPRINTF(IExec, "[virtioscsi:Expunge.c] Expunge: Cleaning up VirtIO queues and hardware.\n");
        CleanupVirtIOSCSI(devBase);

        for (int i = 0; i < 8; i++) {
            if (devBase->units[i]) {
                DPRINTF(IExec, "[virtioscsi:Expunge.c] Expunge: Shutting down unit %d task.\n", i);
                /* Ensure unit task is stopped before freeing (handles forced expunge) */
                UnitTask_Shutdown(devBase, devBase->units[i]);
                IExec->FreeVec(devBase->units[i]);
                devBase->units[i] = NULL;
                DPRINTF(IExec, "[virtioscsi:Expunge.c] Expunge: Unit %d freed.\n", i);
            }
        }

        if (devBase->pciDevice) {
            DPRINTF(IExec, "[virtioscsi:Expunge.c] Expunge: Freeing PCI BARs and device.\n");
            if (devBase->bar0)
                devBase->pciDevice->FreeResourceRange(devBase->bar0);
            if (devBase->bar4)
                devBase->pciDevice->FreeResourceRange(devBase->bar4);
            devBase->IPCI->FreeDevice(devBase->pciDevice);
            devBase->pciDevice = NULL;
        }

        if (devBase->IUtility) {
            DPRINTF(IExec, "[virtioscsi:Expunge.c] Expunge: Dropping IUtility interface.\n");
            IExec->DropInterface((struct Interface *)devBase->IUtility);
        }
        if (devBase->UtilityBase) {
            DPRINTF(IExec, "[virtioscsi:Expunge.c] Expunge: Closing utility.library.\n");
            IExec->CloseLibrary(devBase->UtilityBase);
        }

        if (devBase->IPCI) {
            DPRINTF(IExec, "[virtioscsi:Expunge.c] Expunge: Dropping IPCI interface.\n");
            IExec->DropInterface((struct Interface *)devBase->IPCI);
        }
        if (devBase->ExpansionBase) {
            DPRINTF(IExec, "[virtioscsi:Expunge.c] Expunge: Closing expansion.library.\n");
            IExec->CloseLibrary(devBase->ExpansionBase);
        }

        DPRINTF(IExec, "[virtioscsi:Expunge.c] Expunge: Deleting library base. Goodbye.\n");
        // Delete the OS4 library and interfaces
        IExec->DeleteLibrary((struct Library *)devBase);
    } else {
        DPRINTF(IExec, "[virtioscsi:Expunge.c] Expunge: Still in use — setting LIBF_DELEXP.\n");
        devBase->dev_Base.dd_Library.lib_Flags |= LIBF_DELEXP;
    }

    return seglist;
}
