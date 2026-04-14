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
            unit->changeint_req = ioreq;
            DPRINTF(IExec, "[virtioscsi:BeginIO.c] TD_ADDCHANGEINT: Holding request for T%lu L%lu\n",
                    unit->target_id, unit->lun_id);
        }
        ioreq->io_Flags &= ~IOF_QUICK;
        return; /* Do NOT reply */

    case TD_REMOVE:
        if (unit) {
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
        if (ioreq->io_Flags & IOF_QUICK)
            return;
        IExec->ReplyMsg((struct Message *)ioreq);
        return;

    case TD_PROTSTATUS:
        ioreq->io_Actual = 0; /* 0 = not write-protected */
        ioreq->io_Error = 0;
        if (ioreq->io_Flags & IOF_QUICK)
            return;
        IExec->ReplyMsg((struct Message *)ioreq);
        return;

    case TD_GETDRIVETYPE:
        ioreq->io_Actual = DRIVE_NEWSTYLE; /* 0x44: 64-bit addressing + NSD support */
        ioreq->io_Error = 0;
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

    if (!unit || !unit->io_port)
        return IOERR_NOCMD;

    /*
     * Attempt to remove the request from the unit's message port queue.
     * Forbid() prevents task switching so we can safely traverse the list.
     * If found, we reply with IOERR_ABORTED so the caller isn't left waiting.
     * If not found, the request is already being processed — we can't abort it.
     */
    IExec->Forbid();

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

    IExec->Permit();

    if (found) {
        ioreq->io_Error = IOERR_ABORTED;
        IExec->ReplyMsg((struct Message *)ioreq);
        return 0; /* Success — request aborted */
    }

    return IOERR_NOCMD; /* Request not found in queue (already executing) */
}
