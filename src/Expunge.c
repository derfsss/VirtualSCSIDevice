#include "virtio/virtio_events.h"
#include "virtio/virtio_init.h"
#include "virtio/virtio_irq.h"
#include "virtio/virtio_mounter.h"
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

        /* Stop the event consumer task before tearing down queues.
         * ShutdownEventQueue is a no-op if events were never started. */
        DPRINTF(IExec, "[virtioscsi:Expunge.c] Expunge: Shutting down event queue.\n");
        ShutdownEventQueue(devBase);

        /* Phase 10: tear down mounter.library integration.  MUST follow
         * ShutdownEventQueue so no event-task code is racing with the
         * Denounce/CloseLibrary calls.  No-op if mounter was never opened. */
        DPRINTF(IExec, "[virtioscsi:Expunge.c] Expunge: Cleaning up mounter integration.\n");
        CleanupMounter(devBase);

        /* Remove interrupt handler before resetting VirtIO hardware */
        DPRINTF(IExec, "[virtioscsi:Expunge.c] Expunge: Removing VirtIO interrupt handler.\n");
        RemoveVirtIOInterrupt(devBase);

        DPRINTF(IExec, "[virtioscsi:Expunge.c] Expunge: Cleaning up VirtIO queues and hardware.\n");
        CleanupVirtIOSCSI(devBase);

        for (int i = 0; i < MAX_UNITS; i++) {
            if (devBase->units[i]) {
                DPRINTF(IExec, "[virtioscsi:Expunge.c] Expunge: Shutting down unit %d task.\n", i);
                /* Ensure unit task is stopped before freeing (handles forced expunge) */
                UnitTask_Shutdown(devBase, devBase->units[i]);
                /* Defense in depth: UnitTask_Entry's drain handles held async
                 * requests in the normal shutdown path, but for hot-added units
                 * that never had a task start (no opener arrived) UnitTask_Shutdown
                 * is a no-op. A held req on such a unit is impossible by
                 * construction (BeginIO needs io_port which the task creates),
                 * but the call is a safe no-op when both fields are NULL. */
                Reply_Held_Async_Reqs(IExec, devBase->units[i], IOERR_ABORTED);
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
        DPRINTF(IExec, "[virtioscsi:Expunge.c] Expunge: Still in use -- setting LIBF_DELEXP.\n");
        devBase->dev_Base.dd_Library.lib_Flags |= LIBF_DELEXP;
    }

    return seglist;
}
