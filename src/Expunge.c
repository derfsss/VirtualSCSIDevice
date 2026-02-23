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

        /* Remove interrupt handler before resetting VirtIO hardware */
        RemoveVirtIOInterrupt(devBase);

        CleanupVirtIOSCSI(devBase);

        for (int i = 0; i < 8; i++) {
            if (devBase->units[i]) {
                /* Ensure unit task is stopped before freeing (handles forced expunge) */
                UnitTask_Shutdown(devBase, devBase->units[i]);
                IExec->FreeVec(devBase->units[i]);
                devBase->units[i] = NULL;
            }
        }

        if (devBase->pciDevice) {
            if (devBase->bar0)
                devBase->pciDevice->FreeResourceRange(devBase->bar0);
            if (devBase->bar4)
                devBase->pciDevice->FreeResourceRange(devBase->bar4);
            devBase->IPCI->FreeDevice(devBase->pciDevice);
            devBase->pciDevice = NULL;
        }

        if (devBase->IUtility) {
            IExec->DropInterface((struct Interface *)devBase->IUtility);
        }
        if (devBase->UtilityBase) {
            IExec->CloseLibrary(devBase->UtilityBase);
        }

        if (devBase->IPCI) {
            IExec->DropInterface((struct Interface *)devBase->IPCI);
        }
        if (devBase->ExpansionBase) {
            IExec->CloseLibrary(devBase->ExpansionBase);
        }

        // Delete the OS4 library and interfaces
        IExec->DeleteLibrary((struct Library *)devBase);
    } else {
        devBase->dev_Base.dd_Library.lib_Flags |= LIBF_DELEXP;
    }

    return seglist;
}
