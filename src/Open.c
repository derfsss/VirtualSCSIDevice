#include "virtioscsi.h"
#include "unit_task.h"
#include <exec/errors.h>
#include <exec/exec.h>

struct VirtIOSCSIBase *_manager_Open(struct DeviceManagerInterface *Self, struct IOStdReq *ioreq, ULONG unitNum,
                                     ULONG flags)
{
    struct VirtIOSCSIBase *devBase = (struct VirtIOSCSIBase *)Self->Data.LibBase;
    struct VirtIOUSCSIDevUnit *unit;

    (void)flags; /* only consumed by the DEBUG dump block below */

    devBase->dev_Base.dd_Library.lib_OpenCnt++;

#ifdef DEBUG
    {
        struct Task *caller = devBase->IExec->FindTask(NULL);
        const char *callerName = (caller && caller->tc_Node.ln_Name) ? caller->tc_Node.ln_Name : "?";
        struct MsgPort *rp = ioreq->io_Message.mn_ReplyPort;
        /* Dump all 48 bytes of the ioreq so we see every byte SFS (or any caller)
         * has set, including fields we haven't interpreted. */
        const uint8 *ib = (const uint8 *)ioreq;
        devBase->IExec->DebugPrintF(
                "[virtioscsi:Open.c] Open unit %lu flags=%lu by '%s' ioreq=%p rp=%p (sigBit=%d sigTask=%p)\n"
                "  raw: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n"
                "       %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n"
                "       %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
                unitNum, flags, callerName, ioreq,
                rp, rp ? (int)rp->mp_SigBit : -1, rp ? (void *)rp->mp_SigTask : NULL,
                ib[ 0], ib[ 1], ib[ 2], ib[ 3], ib[ 4], ib[ 5], ib[ 6], ib[ 7],
                ib[ 8], ib[ 9], ib[10], ib[11], ib[12], ib[13], ib[14], ib[15],
                ib[16], ib[17], ib[18], ib[19], ib[20], ib[21], ib[22], ib[23],
                ib[24], ib[25], ib[26], ib[27], ib[28], ib[29], ib[30], ib[31],
                ib[32], ib[33], ib[34], ib[35], ib[36], ib[37], ib[38], ib[39],
                ib[40], ib[41], ib[42], ib[43], ib[44], ib[45], ib[46], ib[47]);
    }
#endif /* DEBUG */

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
    unit->dev_Unit.unit_OpenCnt++;
    ioreq->io_Unit = (struct Unit *)unit;
    ioreq->io_Error = 0;
    /* NOTE: do NOT set mn_Node.ln_Type here.  AllocIORequest() already leaves
     * ln_Type in the correct "ready" state; overriding it to NT_REPLYMSG
     * makes the ioreq look already-replied, which confuses WaitIO() if the
     * caller uses the same ioreq for a later BeginIO/WaitIO cycle. */

    devBase->dev_Base.dd_Library.lib_Flags &= ~LIBF_DELEXP;

#ifdef DEBUG
    /* Dump the first 64 bytes of our libBase and of the unit struct as SFS
     * (or any caller) would see them by dereferencing io_Device / io_Unit.
     * If SFS is rejecting us based on some field, it has to be one of
     * these bytes. */
    {
        const uint8 *lb = (const uint8 *)devBase;
        const uint8 *ub = (const uint8 *)unit;
        const struct Library *lib = &devBase->dev_Base.dd_Library;
        devBase->IExec->DebugPrintF(
                "  libBase=%p lib_Version=%u lib_Revision=%u lib_OpenCnt=%u lib_Flags=0x%02x lib_Sum=0x%08lx lib_NegSize=%u lib_PosSize=%u\n"
                "    ln_Type=%d ln_Pri=%d ln_Name='%s'\n"
                "    raw[0..63]:\n"
                "    %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n"
                "    %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n"
                "    %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n"
                "    %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
                devBase, (uint32)lib->lib_Version, (uint32)lib->lib_Revision, (uint32)lib->lib_OpenCnt,
                (uint32)lib->lib_Flags, (uint32)lib->lib_Sum, (uint32)lib->lib_NegSize, (uint32)lib->lib_PosSize,
                (int)lib->lib_Node.ln_Type, (int)lib->lib_Node.ln_Pri,
                lib->lib_Node.ln_Name ? lib->lib_Node.ln_Name : "(null)",
                lb[ 0], lb[ 1], lb[ 2], lb[ 3], lb[ 4], lb[ 5], lb[ 6], lb[ 7],
                lb[ 8], lb[ 9], lb[10], lb[11], lb[12], lb[13], lb[14], lb[15],
                lb[16], lb[17], lb[18], lb[19], lb[20], lb[21], lb[22], lb[23],
                lb[24], lb[25], lb[26], lb[27], lb[28], lb[29], lb[30], lb[31],
                lb[32], lb[33], lb[34], lb[35], lb[36], lb[37], lb[38], lb[39],
                lb[40], lb[41], lb[42], lb[43], lb[44], lb[45], lb[46], lb[47],
                lb[48], lb[49], lb[50], lb[51], lb[52], lb[53], lb[54], lb[55],
                lb[56], lb[57], lb[58], lb[59], lb[60], lb[61], lb[62], lb[63]);
        devBase->IExec->DebugPrintF(
                "  unit=%p unit_flags=0x%02x unit_OpenCnt=%u\n"
                "    raw[0..63]:\n"
                "    %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n"
                "    %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n"
                "    %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n"
                "    %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
                unit, (uint32)unit->dev_Unit.unit_flags, (uint32)unit->dev_Unit.unit_OpenCnt,
                ub[ 0], ub[ 1], ub[ 2], ub[ 3], ub[ 4], ub[ 5], ub[ 6], ub[ 7],
                ub[ 8], ub[ 9], ub[10], ub[11], ub[12], ub[13], ub[14], ub[15],
                ub[16], ub[17], ub[18], ub[19], ub[20], ub[21], ub[22], ub[23],
                ub[24], ub[25], ub[26], ub[27], ub[28], ub[29], ub[30], ub[31],
                ub[32], ub[33], ub[34], ub[35], ub[36], ub[37], ub[38], ub[39],
                ub[40], ub[41], ub[42], ub[43], ub[44], ub[45], ub[46], ub[47],
                ub[48], ub[49], ub[50], ub[51], ub[52], ub[53], ub[54], ub[55],
                ub[56], ub[57], ub[58], ub[59], ub[60], ub[61], ub[62], ub[63]);
    }
#endif /* DEBUG */

bailout:
    if (ioreq->io_Error != 0) {
        ioreq->io_Unit = (struct Unit *)-1;
        ioreq->io_Device = (struct Device *)-1;
        devBase->dev_Base.dd_Library.lib_OpenCnt--;
        return NULL;
    }

    return devBase;
}
