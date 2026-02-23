#include "unit_task.h"
#include "virtioscsi.h"
#include "virtioscsi_cmds.h"
#include "scsi_cdb_helpers.h"
#include "virtio/virtio_scsi_io.h"
#include <exec/exec.h>
#include <exec/exectags.h>
#include <exec/memory.h>

/* Forward declaration — dispatch is defined below */
static void UnitTask_Dispatch(struct VirtIOSCSIBase *libBase,
                               struct VirtIOUSCSIDevUnit *unit,
                               struct IOStdReq *ioreq);

/* -------------------------------------------------------------------------
 * preallocate_unit_dma / free_unit_dma
 *
 * Allocate req_buf and resp_buf as MEMF_SHARED (DMA-capable) memory and
 * establish permanent DMA mappings. Called once at unit open, freed once at
 * unit close. Eliminates per-request AllocVecTags + StartDMA overhead.
 * ---------------------------------------------------------------------- */
static BOOL preallocate_unit_dma(struct VirtIOSCSIBase *libBase,
                                  struct VirtIOUSCSIDevUnit *unit)
{
    struct ExecIFace *IExec = libBase->IExec;

    /* Allocate req_buf */
    unit->req_buf = IExec->AllocVecTags(sizeof(struct virtio_scsi_req_cmd),
                                        AVT_ClearWithValue, 0,
                                        AVT_Type, MEMF_SHARED,
                                        TAG_DONE);
    if (!unit->req_buf) {
        DPRINTF(IExec, "[virtioscsi:unit_task.c] preallocate_unit_dma: req_buf alloc failed\n");
        return FALSE;
    }

    /* Allocate resp_buf */
    unit->resp_buf = IExec->AllocVecTags(sizeof(struct virtio_scsi_resp_cmd),
                                         AVT_ClearWithValue, 0,
                                         AVT_Type, MEMF_SHARED,
                                         TAG_DONE);
    if (!unit->resp_buf) {
        DPRINTF(IExec, "[virtioscsi:unit_task.c] preallocate_unit_dma: resp_buf alloc failed\n");
        IExec->FreeVec(unit->req_buf);
        unit->req_buf = NULL;
        return FALSE;
    }

    /* DMA-map req_buf */
    unit->dma_req_entries = IExec->StartDMA(unit->req_buf,
                                             sizeof(struct virtio_scsi_req_cmd),
                                             DMA_ReadFromRAM);
    if (unit->dma_req_entries == 0) {
        DPRINTF(IExec, "[virtioscsi:unit_task.c] preallocate_unit_dma: StartDMA(req) failed\n");
        IExec->FreeVec(unit->resp_buf);
        IExec->FreeVec(unit->req_buf);
        unit->resp_buf = NULL;
        unit->req_buf  = NULL;
        return FALSE;
    }
    unit->dma_req_list = IExec->AllocSysObjectTags(ASOT_DMAENTRY,
                                                    ASODMAE_NumEntries, unit->dma_req_entries,
                                                    TAG_DONE);
    if (!unit->dma_req_list) {
        IExec->EndDMA(unit->req_buf, sizeof(struct virtio_scsi_req_cmd),
                      DMA_ReadFromRAM | DMAF_NoModify);
        unit->dma_req_entries = 0;
        IExec->FreeVec(unit->resp_buf);
        IExec->FreeVec(unit->req_buf);
        unit->resp_buf = NULL;
        unit->req_buf  = NULL;
        return FALSE;
    }
    IExec->GetDMAList(unit->req_buf, sizeof(struct virtio_scsi_req_cmd),
                      DMA_ReadFromRAM, unit->dma_req_list);

    /* DMA-map resp_buf */
    unit->dma_resp_entries = IExec->StartDMA(unit->resp_buf,
                                              sizeof(struct virtio_scsi_resp_cmd),
                                              0 /* device writes = IN */);
    if (unit->dma_resp_entries == 0) {
        DPRINTF(IExec, "[virtioscsi:unit_task.c] preallocate_unit_dma: StartDMA(resp) failed\n");
        IExec->FreeSysObject(ASOT_DMAENTRY, unit->dma_req_list);
        IExec->EndDMA(unit->req_buf, sizeof(struct virtio_scsi_req_cmd),
                      DMA_ReadFromRAM | DMAF_NoModify);
        unit->dma_req_entries = 0;
        unit->dma_req_list    = NULL;
        IExec->FreeVec(unit->resp_buf);
        IExec->FreeVec(unit->req_buf);
        unit->resp_buf = NULL;
        unit->req_buf  = NULL;
        return FALSE;
    }
    unit->dma_resp_list = IExec->AllocSysObjectTags(ASOT_DMAENTRY,
                                                     ASODMAE_NumEntries, unit->dma_resp_entries,
                                                     TAG_DONE);
    if (!unit->dma_resp_list) {
        IExec->EndDMA(unit->resp_buf, sizeof(struct virtio_scsi_resp_cmd),
                      DMAF_NoModify);
        unit->dma_resp_entries = 0;
        IExec->FreeSysObject(ASOT_DMAENTRY, unit->dma_req_list);
        IExec->EndDMA(unit->req_buf, sizeof(struct virtio_scsi_req_cmd),
                      DMA_ReadFromRAM | DMAF_NoModify);
        unit->dma_req_entries = 0;
        unit->dma_req_list    = NULL;
        IExec->FreeVec(unit->resp_buf);
        IExec->FreeVec(unit->req_buf);
        unit->resp_buf = NULL;
        unit->req_buf  = NULL;
        return FALSE;
    }
    IExec->GetDMAList(unit->resp_buf, sizeof(struct virtio_scsi_resp_cmd),
                      0, unit->dma_resp_list);

    DPRINTF(IExec, "[virtioscsi:unit_task.c] preallocate_unit_dma: unit %lu DMA ready "
            "(req_entries=%lu resp_entries=%lu)\n",
            unit->unit_num, unit->dma_req_entries, unit->dma_resp_entries);
    return TRUE;
}

static void free_unit_dma(struct VirtIOSCSIBase *libBase,
                           struct VirtIOUSCSIDevUnit *unit)
{
    struct ExecIFace *IExec = libBase->IExec;

    if (unit->dma_resp_list) {
        IExec->FreeSysObject(ASOT_DMAENTRY, unit->dma_resp_list);
        unit->dma_resp_list = NULL;
    }
    if (unit->dma_resp_entries > 0 && unit->resp_buf) {
        IExec->EndDMA(unit->resp_buf, sizeof(struct virtio_scsi_resp_cmd), DMAF_NoModify);
        unit->dma_resp_entries = 0;
    }

    if (unit->dma_req_list) {
        IExec->FreeSysObject(ASOT_DMAENTRY, unit->dma_req_list);
        unit->dma_req_list = NULL;
    }
    if (unit->dma_req_entries > 0 && unit->req_buf) {
        IExec->EndDMA(unit->req_buf, sizeof(struct virtio_scsi_req_cmd),
                      DMA_ReadFromRAM | DMAF_NoModify);
        unit->dma_req_entries = 0;
    }

    if (unit->resp_buf) {
        IExec->FreeVec(unit->resp_buf);
        unit->resp_buf = NULL;
    }
    if (unit->req_buf) {
        IExec->FreeVec(unit->req_buf);
        unit->req_buf = NULL;
    }

    DPRINTF(IExec, "[virtioscsi:unit_task.c] free_unit_dma: unit %lu DMA freed\n", unit->unit_num);
}

/* -------------------------------------------------------------------------
 * UnitTask_Entry
 *
 * Entry point for each unit's device task. Exec calls this function in the
 * new task's context. We retrieve our parameters from tc_UserData, open our
 * message port, signal the parent, then run the event loop.
 * ---------------------------------------------------------------------- */
void UnitTask_Entry(void)
{
    struct ExecIFace *IExec;

    /* tc_UserData carries the start message set by UnitTask_Start() */
    struct Task *self = ((struct ExecIFace *)((*(struct ExecBase **)4)->MainInterface))->FindTask(NULL);
    struct UnitTaskStartMsg *startMsg = (struct UnitTaskStartMsg *)self->tc_UserData;

    struct VirtIOSCSIBase    *libBase = startMsg->libBase;
    struct VirtIOUSCSIDevUnit *unit   = startMsg->unit;
    IExec = libBase->IExec;

    DPRINTF(IExec, "[virtioscsi:unit_task.c] UnitTask_Entry: unit %lu started\n", unit->unit_num);

    /* Allocate our message port */
    unit->io_port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    if (!unit->io_port) {
        DPRINTF(IExec, "[virtioscsi:unit_task.c] UnitTask_Entry: AllocSysObject(PORT) failed\n");
        /* Signal parent of failure */
        IExec->Signal(startMsg->parent_task, startMsg->ready_mask);
        return;
    }

    unit->io_port_mask = 1UL << unit->io_port->mp_SigBit;
    unit->task         = self;
    unit->task_shutdown = FALSE;

    /* Signal parent: port is ready, it's safe to PutMsg to us now */
    IExec->Signal(startMsg->parent_task, startMsg->ready_mask);
    /* startMsg is stack-allocated in UnitTask_Start — do NOT touch it after this signal */

    /* ---- Main event loop ---- */
    while (!unit->task_shutdown) {
        uint32 sigs = IExec->Wait(unit->io_port_mask | SIGBREAKF_CTRL_C);

        if (sigs & SIGBREAKF_CTRL_C)
            break;

        struct IOStdReq *ioreq;
        while ((ioreq = (struct IOStdReq *)IExec->GetMsg(unit->io_port)) != NULL) {
            UnitTask_Dispatch(libBase, unit, ioreq);
            IExec->ReplyMsg((struct Message *)ioreq);
        }
    }

    /* Drain any requests that arrived between the break signal and here */
    {
        struct IOStdReq *ioreq;
        while ((ioreq = (struct IOStdReq *)IExec->GetMsg(unit->io_port)) != NULL) {
            ioreq->io_Error = IOERR_ABORTED;
            IExec->ReplyMsg((struct Message *)ioreq);
        }
    }

    IExec->FreeSysObject(ASOT_PORT, unit->io_port);
    unit->io_port      = NULL;
    unit->io_port_mask = 0;
    unit->task         = NULL;

    DPRINTF(IExec, "[virtioscsi:unit_task.c] UnitTask_Entry: unit %lu exiting\n", unit->unit_num);

    /* Signal parent that we have fully shut down */
    IExec->Signal(startMsg->parent_task, startMsg->ready_mask);
}

/* -------------------------------------------------------------------------
 * UnitTask_Start
 *
 * Called from _manager_Open() (under the device library open lock).
 * Creates a new exec task for the unit and blocks until it signals ready.
 * ---------------------------------------------------------------------- */
BOOL UnitTask_Start(struct VirtIOSCSIBase *libBase, struct VirtIOUSCSIDevUnit *unit)
{
    struct ExecIFace *IExec = libBase->IExec;

    /* Allocate a signal bit for synchronisation */
    int8 ready_bit = IExec->AllocSignal(-1);
    if (ready_bit < 0) {
        DPRINTF(IExec, "[virtioscsi:unit_task.c] UnitTask_Start: AllocSignal failed\n");
        return FALSE;
    }

    uint32 ready_mask = 1UL << ready_bit;

    /* Stack-allocate the start message — it lives until the task signals us */
    struct UnitTaskStartMsg startMsg;
    startMsg.libBase     = libBase;
    startMsg.unit        = unit;
    startMsg.parent_task = IExec->FindTask(NULL);
    startMsg.ready_mask  = ready_mask;
    startMsg.ready_bit   = ready_bit;

    /* Create the task — name it after the unit number */
    char taskName[32];
    libBase->IUtility->SNPrintf(taskName, sizeof(taskName), "virtioscsi unit %lu", unit->unit_num);

    /* Create the task under Forbid so we can safely set tc_UserData before
     * the scheduler picks it up. Forbid() prevents task switching but not
     * interrupts; it's the standard pattern for this. */
    IExec->Forbid();
    struct Task *task = IExec->CreateTaskTags(taskName, 5, UnitTask_Entry, 16384,
                                              TAG_DONE);
    if (task) {
        task->tc_UserData = (APTR)&startMsg;
    }
    IExec->Permit();

    if (!task) {
        DPRINTF(IExec, "[virtioscsi:unit_task.c] UnitTask_Start: CreateTaskTags failed\n");
        IExec->FreeSignal(ready_bit);
        return FALSE;
    }

    /* Wait for the task to open its port (or fail) */
    IExec->Wait(ready_mask);
    IExec->FreeSignal(ready_bit);

    if (!unit->io_port) {
        /* Task signalled us but port is NULL — it failed to allocate */
        DPRINTF(IExec, "[virtioscsi:unit_task.c] UnitTask_Start: task failed to init port\n");
        return FALSE;
    }

    /* Pre-allocate DMA buffers now that the task is running */
    if (!preallocate_unit_dma(libBase, unit)) {
        DPRINTF(IExec, "[virtioscsi:unit_task.c] UnitTask_Start: DMA pre-alloc failed\n");
        UnitTask_Shutdown(libBase, unit);
        return FALSE;
    }

    DPRINTF(IExec, "[virtioscsi:unit_task.c] UnitTask_Start: unit %lu task ready\n", unit->unit_num);
    return TRUE;
}

/* -------------------------------------------------------------------------
 * UnitTask_Shutdown
 *
 * Signal the unit task to exit and wait for it to confirm.
 * ---------------------------------------------------------------------- */
void UnitTask_Shutdown(struct VirtIOSCSIBase *libBase, struct VirtIOUSCSIDevUnit *unit)
{
    struct ExecIFace *IExec = libBase->IExec;

    if (!unit->task)
        return;

    DPRINTF(IExec, "[virtioscsi:unit_task.c] UnitTask_Shutdown: stopping unit %lu task\n", unit->unit_num);

    /* Free pre-allocated DMA buffers before stopping the task.
     * The task must not be mid-I/O at this point (caller ensures last close). */
    free_unit_dma(libBase, unit);

    /* No extra signal needed — simply signal CTRL_C and busy-wait.
     * See comments below for rationale. */

    unit->task_shutdown = TRUE;
    struct Task *t = unit->task; /* capture before it clears itself */
    IExec->Signal(t, SIGBREAKF_CTRL_C);

    /*
     * Busy-wait for the task to clear unit->task.
     * This is acceptable because shutdown is rare (device close/expunge)
     * and the unit task will exit promptly.
     * A proper solution would require a dedicated shutdown signal bit stored
     * in the unit struct, but that adds 8 bytes for a rarely-used path.
     */
    uint32 patience = 100000;
    while (unit->task && patience-- > 0) {
        /* yield by calling Forbid/Permit to give the other task a chance */
        IExec->Forbid();
        IExec->Permit();
    }

    DPRINTF(IExec, "[virtioscsi:unit_task.c] UnitTask_Shutdown: unit %lu task stopped\n", unit->unit_num);
}

/* -------------------------------------------------------------------------
 * UnitTask_Dispatch
 *
 * Called from the unit task's event loop for each IORequest dequeued from
 * the message port. Sets io_Error / io_Actual; caller calls ReplyMsg.
 * ---------------------------------------------------------------------- */
static void UnitTask_Dispatch(struct VirtIOSCSIBase *libBase,
                               struct VirtIOUSCSIDevUnit *unit,
                               struct IOStdReq *ioreq)
{
    switch (ioreq->io_Command) {

    case CMD_READ:
    case ETD_READ:
        Handle_CMD_Read(libBase, ioreq);
        break;

    case CMD_WRITE:
    case TD_FORMAT:
    case ETD_FORMAT:
    case ETD_WRITE:
        Handle_CMD_Write(libBase, ioreq);
        break;

    case CMD_UPDATE:
    case CMD_FLUSH:
    case ETD_UPDATE:
        Handle_CMD_Update(libBase, ioreq);
        break;

    case TD_READ64:
    case TD_WRITE64:
    case TD_FORMAT64:
        Handle_TD_IO64(libBase, ioreq);
        break;

    case TD_GETGEOMETRY:
        Handle_TD_GetGeometry(libBase, ioreq);
        break;

    case HD_SCSICMD:
        Parse_SCSI_Command(libBase, ioreq);
        break;

    case NSCMD_TD_READ64:
    case NSCMD_TD_WRITE64:
    case NSCMD_TD_FORMAT64:
    case NSCMD_ETD_READ64:
    case NSCMD_ETD_WRITE64:
    case NSCMD_ETD_FORMAT64:
        Handle_NS_TD_IO64(libBase, ioreq);
        break;

    case NSCMD_TD_GETGEOMETRY64:
        Handle_NS_TD_GetGeometry64(libBase, ioreq);
        break;

    case NSCMD_TD_SEEK64:
    case NSCMD_TD_CHANGEUNIT:
    case NSCMD_TD_ADDSTATCALLBACK:
    case NSCMD_TD_REMSTATCALLBACK:
    case NSCMD_ETD_SEEK64:
        /* NSD no-ops for fixed media */
        ioreq->io_Error = 0;
        break;

    default:
        ioreq->io_Error = IOERR_NOCMD;
        break;
    }
}
