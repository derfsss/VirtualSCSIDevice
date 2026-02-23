#include "virtioscsi.h"
#include "unit_task.h"
#include <exec/errors.h>
#include <exec/exec.h>

struct VirtIOSCSIBase *_manager_Open(struct DeviceManagerInterface *Self, struct IOStdReq *ioreq, ULONG unitNum,
                                     ULONG flags)
{
    struct VirtIOSCSIBase *devBase = (struct VirtIOSCSIBase *)Self->Data.LibBase;
    struct VirtIOUSCSIDevUnit *unit;

    devBase->dev_Base.dd_Library.lib_OpenCnt++;

    DPRINTF(devBase->IExec, "[virtioscsi:Open.c] Open unit %lu requested (flags %lu)\n", unitNum, flags);

    if (unitNum > 7) {
        ioreq->io_Error = IOERR_OPENFAIL;
        goto bailout;
    }

    unit = devBase->units[unitNum];
    if (unit == NULL) {
        ioreq->io_Error = IOERR_OPENFAIL;
        goto bailout;
    }

    /* Start unit task on first open */
    if (unit->open_count == 0) {
        if (!UnitTask_Start(devBase, unit)) {
            DPRINTF(devBase->IExec, "[virtioscsi:Open.c] UnitTask_Start failed for unit %lu\n", unitNum);
            ioreq->io_Error = IOERR_OPENFAIL;
            goto bailout;
        }
    }

    unit->open_count++;
    ioreq->io_Unit = (struct Unit *)unit;
    ioreq->io_Error = 0;
    ioreq->io_Message.mn_Node.ln_Type = NT_REPLYMSG;

    devBase->dev_Base.dd_Library.lib_Flags &= ~LIBF_DELEXP;

bailout:
    if (ioreq->io_Error != 0) {
        ioreq->io_Unit = (struct Unit *)-1;
        ioreq->io_Device = (struct Device *)-1;
        devBase->dev_Base.dd_Library.lib_OpenCnt--;
        return NULL;
    }

    return devBase;
}
