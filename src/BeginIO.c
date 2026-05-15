#include "cmd_names.h"
#include "virtioscsi.h"
#include "virtioscsi_cmds.h"
#include <exec/errors.h>

void _manager_BeginIO(struct DeviceManagerInterface *Self, struct IOStdReq *ioreq)
{
    struct VirtIOSCSIBase *libBase = (struct VirtIOSCSIBase *)Self->Data.LibBase;
    struct ExecIFace *IExec = libBase->IExec;
    struct VirtIOUSCSIDevUnit *unit = (struct VirtIOUSCSIDevUnit *)ioreq->io_Unit;

    ioreq->io_Error = 0;

    struct Task *task = IExec->FindTask(NULL);
    const char *taskName = task ? task->tc_Node.ln_Name : "unknown";
    uint32 target = unit ? unit->target_id : 99;
    uint32 lun = unit ? unit->lun_id : 99;

    DPRINTF(IExec,
            "[virtioscsi:BeginIO.c] BeginIO [%s]: T%lu L%lu Cmd %lu (%s) Off 0x%08lx Data %p Len %lu "
            "Flags 0x%02X G:%d\n",
            taskName, target, lun, (uint32)ioreq->io_Command, GetCommandName(ioreq->io_Command),
            (uint32)ioreq->io_Offset, ioreq->io_Data, (uint32)ioreq->io_Length, (uint32)ioreq->io_Flags,
            unit ? (int)unit->geometry_valid : 0);

    switch (ioreq->io_Command) {

    /*
     * === Held Change Notification Commands ===
     *
     * TD_ADDCHANGEINT and TD_REMOVE register callbacks for disk change
     * notification. The IORequest must NOT be replied to — it stays pending
     * until TD_REMCHANGEINT is issued or the device is closed. Replying
     * immediately causes filesystem handlers (SFS, etc.) to corrupt their
     * linked lists, leading to DSI crashes.
     * These are handled inline and never queued.
     */
    case TD_ADDCHANGEINT:
        if (unit) {
            /* A second TD_ADDCHANGEINT without an intervening TD_REMCHANGEINT
             * is a caller bug, but tolerate it: reply the previous request
             * with IOERR_ABORTED so its caller wakes, then store the new one.
             * Silent overwrite would orphan the prior IORequest. */
            if (unit->changeint_req) {
                DPRINTF(IExec, "[virtioscsi:BeginIO.c] TD_ADDCHANGEINT: replacing held req on T%lu L%lu\n",
                        unit->target_id, unit->lun_id);
                unit->changeint_req->io_Error = IOERR_ABORTED;
                IExec->ReplyMsg((struct Message *)unit->changeint_req);
            }
            unit->changeint_req = ioreq;
            DPRINTF(IExec, "[virtioscsi:BeginIO.c] TD_ADDCHANGEINT: Holding request for T%lu L%lu\n",
                    unit->target_id, unit->lun_id);
        }
        ioreq->io_Flags &= ~IOF_QUICK;
        return; /* Do NOT reply */

    case TD_REMOVE:
        if (unit) {
            /* Same reject-or-replace semantics as TD_ADDCHANGEINT. */
            if (unit->remove_req) {
                DPRINTF(IExec, "[virtioscsi:BeginIO.c] TD_REMOVE: replacing held req on T%lu L%lu\n",
                        unit->target_id, unit->lun_id);
                unit->remove_req->io_Error = IOERR_ABORTED;
                IExec->ReplyMsg((struct Message *)unit->remove_req);
            }
            unit->remove_req = ioreq;
            DPRINTF(IExec, "[virtioscsi:BeginIO.c] TD_REMOVE: Holding request for T%lu L%lu\n",
                    unit->target_id, unit->lun_id);
        }
        ioreq->io_Flags &= ~IOF_QUICK;
        return; /* Do NOT reply */

    case TD_REMCHANGEINT:
        if (unit && unit->changeint_req) {
            DPRINTF(IExec, "[virtioscsi:BeginIO.c] TD_REMCHANGEINT: Replying to held ADDCHANGEINT on T%lu L%lu\n",
                    unit->target_id, unit->lun_id);
            unit->changeint_req->io_Error = 0;
            IExec->ReplyMsg((struct Message *)unit->changeint_req);
            unit->changeint_req = NULL;
        }
        ioreq->io_Error = 0;
        if (ioreq->io_Flags & IOF_QUICK)
            return; /* Quick: caller polls io_Error, no reply needed */
        IExec->ReplyMsg((struct Message *)ioreq);
        return;

    /*
     * === Simple Commands (handled inline — no unit task needed) ===
     *
     * VirtIO SCSI disks are fixed media: always present, never spinning down.
     * These commands require no hardware interaction and are replied to here.
     */

    /* Motor control: report motor was already on (io_Actual = 1) */
    case CMD_STOP:
    case CMD_START:
    case TD_MOTOR:
        ioreq->io_Actual = 1;
        ioreq->io_Error = 0;
        if (ioreq->io_Flags & IOF_QUICK)
            return;
        IExec->ReplyMsg((struct Message *)ioreq);
        return;

    /* Position / flush no-ops for fixed media */
    case TD_SEEK:
    case TD_EJECT:
    case TD_SEEK64:
    case ETD_MOTOR:
    case ETD_SEEK:
    case CMD_CLEAR:
    case ETD_CLEAR:
        ioreq->io_Error = 0;
        if (ioreq->io_Flags & IOF_QUICK)
            return;
        IExec->ReplyMsg((struct Message *)ioreq);
        return;

    case TD_CHANGENUM:
        /* Monotonic change counter.  Incremented by the event-queue
         * handler on every PARAM_CHANGE / TRANSPORT_RESET concerning this
         * unit.  Filesystems read this to detect missed changes. */
        ioreq->io_Actual = unit ? unit->change_count : 0;
        ioreq->io_Error = 0;
        DPRINTF(IExec, "[virtioscsi:BeginIO.c] TD_CHANGENUM inline reply: ioreq=%p actual=%lu port=%p\n",
                ioreq, (uint32)ioreq->io_Actual, ioreq->io_Message.mn_ReplyPort);
        if (ioreq->io_Flags & IOF_QUICK)
            return;
        IExec->ReplyMsg((struct Message *)ioreq);
        return;

    case TD_CHANGESTATE:
        /* 0 = disk present, 1 = no disk.  Updated by CD insert/eject
         * events from the VirtIO event queue (VIRTIO_SCSI_T_PARAM_CHANGE
         * with ASC 0x28/0x3A, or TRANSPORT_RESET RESCAN/REMOVED on an
         * existing LUN). */
        ioreq->io_Actual = (unit && unit->media_present) ? 0 : 1;
        ioreq->io_Error = 0;
        DPRINTF(IExec, "[virtioscsi:BeginIO.c] TD_CHANGESTATE inline reply: ioreq=%p actual=%lu port=%p\n",
                ioreq, (uint32)ioreq->io_Actual, ioreq->io_Message.mn_ReplyPort);
        if (ioreq->io_Flags & IOF_QUICK)
            return;
        IExec->ReplyMsg((struct Message *)ioreq);
        return;

    case TD_PROTSTATUS:
        ioreq->io_Actual = 0; /* 0 = not write-protected */
        ioreq->io_Error = 0;
        DPRINTF(IExec, "[virtioscsi:BeginIO.c] TD_PROTSTATUS inline reply: ioreq=%p actual=%lu port=%p\n",
                ioreq, (uint32)ioreq->io_Actual, ioreq->io_Message.mn_ReplyPort);
        if (ioreq->io_Flags & IOF_QUICK)
            return;
        IExec->ReplyMsg((struct Message *)ioreq);
        return;

    case TD_GETDRIVETYPE:
        /* DEBUG: a1ide.device (DH0 driver, works fine with SFS) does NOT
         * return DRIVE_NEWSTYLE.  Inspecting its binary shows it returns
         * a classic value (likely DRIVE3_5 = 1).  Returning DRIVE_NEWSTYLE
         * may put DOS / diskboot.kmod / SFS into a "modern device" code
         * path that doesn't correctly construct the DOSNode for an RDB
         * partition on a virtio-scsi device — observed crash: kernel
         * deref of a corrupt BPTR (0x22C58000) inside DOS struct walking
         * code, after our TD_GETGEOMETRY reply, before SFS issues any
         * CMD_READ.  Fall back to DRIVE3_5 to match a1ide.device. */
        ioreq->io_Actual = DRIVE3_5;
        ioreq->io_Error = 0;
        DPRINTF(IExec, "[virtioscsi:BeginIO.c] TD_GETDRIVETYPE inline reply (DRIVE3_5=1, was DRIVE_NEWSTYLE): ioreq=%p actual=0x%lx flags=0x%02x port=%p\n",
                ioreq, (uint32)ioreq->io_Actual, (uint32)ioreq->io_Flags, ioreq->io_Message.mn_ReplyPort);
        if (ioreq->io_Flags & IOF_QUICK)
            return;
        IExec->ReplyMsg((struct Message *)ioreq);
        return;

    case TD_GETNUMTRACKS:
        ioreq->io_Actual = 0;
        ioreq->io_Error = 0;
        if (ioreq->io_Flags & IOF_QUICK)
            return;
        IExec->ReplyMsg((struct Message *)ioreq);
        return;

    case NSCMD_DEVICEQUERY:
        Parse_NS_Command(libBase, ioreq);
        if (ioreq->io_Flags & IOF_QUICK)
            return;
        IExec->ReplyMsg((struct Message *)ioreq);
        return;

    /*
     * === Slow I/O Commands — queued to the unit task ===
     *
     * These commands involve actual hardware I/O and must not block the
     * calling task. We clear IOF_QUICK, hand the request to the unit task
     * via PutMsg(), and return immediately. The unit task calls ReplyMsg()
     * when done.
     *
     * If the unit task is not running (no open), fall back to IOERR_OPENFAIL.
     */
    case CMD_READ:
    case ETD_READ:
    case CMD_WRITE:
    case TD_FORMAT:
    case ETD_FORMAT:
    case ETD_WRITE:
    case CMD_UPDATE:
    case CMD_FLUSH:
    case ETD_UPDATE:
    case HD_SCSICMD:
    case TD_GETGEOMETRY:
    case TD_READ64:
    case TD_WRITE64:
    case TD_FORMAT64:
    case NSCMD_TD_READ64:
    case NSCMD_TD_WRITE64:
    case NSCMD_TD_SEEK64:
    case NSCMD_TD_FORMAT64:
    case NSCMD_TD_GETGEOMETRY64:
    case NSCMD_TD_CHANGEUNIT:
    case NSCMD_TD_ADDSTATCALLBACK:
    case NSCMD_TD_REMSTATCALLBACK:
    case NSCMD_ETD_READ64:
    case NSCMD_ETD_WRITE64:
    case NSCMD_ETD_SEEK64:
    case NSCMD_ETD_FORMAT64:
        if (!unit || !unit->io_port) {
            ioreq->io_Error = IOERR_OPENFAIL;
            if (ioreq->io_Flags & IOF_QUICK)
                return;
            IExec->ReplyMsg((struct Message *)ioreq);
            return;
        }
        /* DEBUG: For CMD_WRITE / TD_WRITE64 / ETD_WRITE / NSCMD_TD_WRITE64,
         * dump the first 128 bytes of the buffer the caller wants to write.
         * This is what diskboot.kmod / MediaToolbox / SFS-format will write
         * during partition setup so we can verify exactly what the disk
         * receives — and tell apart RDB / PartitionBlock / FSHD / SFS root
         * blocks by their 4-byte signatures. */
        if ((ioreq->io_Command == CMD_WRITE || ioreq->io_Command == TD_WRITE64 ||
             ioreq->io_Command == NSCMD_TD_WRITE64 || ioreq->io_Command == ETD_WRITE) &&
            ioreq->io_Data && ioreq->io_Length > 0) {
            uint8 *b = (uint8 *)ioreq->io_Data;
            uint32 dump_len = ioreq->io_Length < 128 ? ioreq->io_Length : 128;
            char sig[5] = {0};
            sig[0] = b[0]; sig[1] = b[1]; sig[2] = b[2]; sig[3] = b[3];
            const char *sigtype = "unknown";
            if (sig[0] == 'R' && sig[1] == 'D' && sig[2] == 'S' && sig[3] == 'K') sigtype = "RDSK (RDB header)";
            else if (sig[0] == 'P' && sig[1] == 'A' && sig[2] == 'R' && sig[3] == 'T') sigtype = "PART (PartitionBlock)";
            else if (sig[0] == 'F' && sig[1] == 'S' && sig[2] == 'H' && sig[3] == 'D') sigtype = "FSHD (FileSysHeader)";
            else if (sig[0] == 'L' && sig[1] == 'S' && sig[2] == 'E' && sig[3] == 'G') sigtype = "LSEG (LoadSeg block)";
            else if (sig[0] == 'B' && sig[1] == 'A' && sig[2] == 'D' && sig[3] == 'B') sigtype = "BADB (BadBlock list)";
            else if (sig[0] == 'S' && sig[1] == 'F' && sig[2] == 'S' && sig[3] == 0) sigtype = "SFS root/superblock";
            DPRINTF(IExec,
                    "[virtioscsi:BeginIO.c] WRITE T%lu L%lu off=0x%llx len=%lu first4=%02x%02x%02x%02x (%s)\n",
                    unit->target_id, unit->lun_id,
                    (unsigned long long)ioreq->io_Offset, (uint32)ioreq->io_Length,
                    b[0], b[1], b[2], b[3], sigtype);
            for (uint32 r = 0; r < dump_len; r += 16) {
                DPRINTF(IExec, "  +0x%03lx: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
                        r,
                        b[r+ 0], b[r+ 1], b[r+ 2], b[r+ 3],
                        b[r+ 4], b[r+ 5], b[r+ 6], b[r+ 7],
                        b[r+ 8], b[r+ 9], b[r+10], b[r+11],
                        b[r+12], b[r+13], b[r+14], b[r+15]);
            }
        }
        /* Clear IOF_QUICK: caller must not touch ioreq until ReplyMsg */
        ioreq->io_Flags &= ~IOF_QUICK;
        DPRINTF(IExec, "[virtioscsi:BeginIO.c] Queuing cmd %lu to unit task port\n", (uint32)ioreq->io_Command);
        IExec->PutMsg(unit->io_port, (struct Message *)ioreq);
        return; /* Unit task calls ReplyMsg when done */

    default:
        DPRINTF(IExec, "[virtioscsi:BeginIO.c] UNKNOWN COMMAND: %d. Returning IOERR_NOCMD.\n", ioreq->io_Command);
        ioreq->io_Error = IOERR_NOCMD;
        if (ioreq->io_Flags & IOF_QUICK)
            return;
        IExec->ReplyMsg((struct Message *)ioreq);
        return;
    }
}

LONG _manager_AbortIO(struct DeviceManagerInterface *Self, struct IOStdReq *ioreq)
{
    struct VirtIOSCSIBase *libBase = (struct VirtIOSCSIBase *)Self->Data.LibBase;
    struct ExecIFace *IExec = libBase->IExec;
    struct VirtIOUSCSIDevUnit *unit = (struct VirtIOUSCSIDevUnit *)ioreq->io_Unit;

    struct Task *caller = IExec->FindTask(NULL);
    const char *callerName = (caller && caller->tc_Node.ln_Name) ? caller->tc_Node.ln_Name : "?";
    DPRINTF(IExec, "[virtioscsi:BeginIO.c] AbortIO by '%s' ioreq=%p cmd=%u port=%p\n",
            callerName, ioreq, (uint32)ioreq->io_Command, ioreq->io_Message.mn_ReplyPort);

    if (!unit || !unit->io_port || !unit->port_mutex)
        return IOERR_NOCMD;

    /*
     * Attempt to remove the request from the unit's message port queue.
     * The port_mutex serializes this traversal against the unit task's
     * GetMsg loop.  Replaces deprecated Forbid/Permit per OS4 guidelines.
     */
    IExec->MutexObtain(unit->port_mutex);

    struct Message *msg = (struct Message *)unit->io_port->mp_MsgList.lh_Head;
    struct Message *found = NULL;

    while (msg->mn_Node.ln_Succ) {
        if (msg == (struct Message *)ioreq) {
            found = msg;
            break;
        }
        msg = (struct Message *)msg->mn_Node.ln_Succ;
    }

    if (found) {
        IExec->Remove((struct Node *)found);
    }

    IExec->MutexRelease(unit->port_mutex);

    if (found) {
        ioreq->io_Error = IOERR_ABORTED;
        IExec->ReplyMsg((struct Message *)ioreq);
        return 0; /* Success: request aborted */
    }

    return IOERR_NOCMD; /* Request not found in queue (already executing) */
}

/*
 * Reply and clear any held async TD_ADDCHANGEINT / TD_REMOVE on a unit.
 * Used by:
 *  - UnitTask_Entry shutdown drain (error_code = IOERR_ABORTED)
 *  - Expunge per-unit cleanup    (error_code = IOERR_ABORTED)
 *  - RESET_REMOVED for the remove_req (error_code = 0, the handshake)
 *
 * Both fields are NULLed after replying so a second call is a no-op.
 */
void Reply_Held_Async_Reqs(struct ExecIFace *IExec,
                           struct VirtIOUSCSIDevUnit *unit,
                           int8 error_code)
{
    if (!unit) return;

    if (unit->changeint_req) {
        unit->changeint_req->io_Error = error_code;
        IExec->ReplyMsg((struct Message *)unit->changeint_req);
        unit->changeint_req = NULL;
    }

    if (unit->remove_req) {
        unit->remove_req->io_Error = error_code;
        IExec->ReplyMsg((struct Message *)unit->remove_req);
        unit->remove_req = NULL;
    }
}
