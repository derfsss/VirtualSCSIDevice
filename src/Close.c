#include "virtioscsi.h"
#include "unit_task.h"
#include <exec/exec.h>

BPTR _manager_Expunge(struct DeviceManagerInterface *Self);

BPTR _manager_Close(struct DeviceManagerInterface *Self, struct IOStdReq *ioreq)
{
    struct VirtIOSCSIBase *devBase = (struct VirtIOSCSIBase *)Self->Data.LibBase;
    struct VirtIOUSCSIDevUnit *unit = (struct VirtIOUSCSIDevUnit *)ioreq->io_Unit;
    BPTR seglist = (BPTR)NULL;

    struct Task *caller = devBase->IExec->FindTask(NULL);
    const char *callerName = (caller && caller->tc_Node.ln_Name) ? caller->tc_Node.ln_Name : "?";
    uint8 *ib = (uint8 *)ioreq;
    DPRINTF(devBase->IExec,
            "[virtioscsi:Close.c] Close by '%s' ioreq=%p unit=%p open_count=%ld\n"
            "  raw: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n"
            "       %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n"
            "       %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
            callerName, ioreq, unit,
            (long)(unit ? (long)unit->open_count : -1L),
            ib[ 0], ib[ 1], ib[ 2], ib[ 3], ib[ 4], ib[ 5], ib[ 6], ib[ 7],
            ib[ 8], ib[ 9], ib[10], ib[11], ib[12], ib[13], ib[14], ib[15],
            ib[16], ib[17], ib[18], ib[19], ib[20], ib[21], ib[22], ib[23],
            ib[24], ib[25], ib[26], ib[27], ib[28], ib[29], ib[30], ib[31],
            ib[32], ib[33], ib[34], ib[35], ib[36], ib[37], ib[38], ib[39],
            ib[40], ib[41], ib[42], ib[43], ib[44], ib[45], ib[46], ib[47]);

    if (unit && unit != (struct VirtIOUSCSIDevUnit *)-1) {
        if (unit->open_count > 0) {
            unit->open_count--;
            if (unit->dev_Unit.unit_OpenCnt > 0)
                unit->dev_Unit.unit_OpenCnt--;
            /* Shutdown unit task when last opener closes */
            if (unit->open_count == 0) {
                UnitTask_Shutdown(devBase, unit);
            }
        }
    }

    ioreq->io_Unit = (struct Unit *)-1;
    ioreq->io_Device = (struct Device *)-1;

    devBase->dev_Base.dd_Library.lib_OpenCnt--;

    if (devBase->dev_Base.dd_Library.lib_OpenCnt == 0) {
        if (devBase->dev_Base.dd_Library.lib_Flags & LIBF_DELEXP) {
            seglist = _manager_Expunge(Self);
        }
    }

    return seglist;
}
