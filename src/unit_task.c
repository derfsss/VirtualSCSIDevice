#include "unit_task.h"
#include "virtioscsi.h"
#include "virtioscsi_cmds.h"
#include "scsi_cdb_helpers.h"
#include "virtio/virtio_scsi_io.h"
#include <exec/exec.h>
#include <exec/exectags.h>
#include <exec/memory.h>

/* Forward declaration — dispatch is defined below.
 * Returns TRUE if at least one VirtIOSCSI_Submit() succeeded (kick pending). */
static BOOL UnitTask_Dispatch(struct VirtIOSCSIBase *libBase,
                               struct VirtIOUSCSIDevUnit *unit,
                               struct IOStdReq *ioreq);

/* -------------------------------------------------------------------------
 * preallocate_unit_dma / free_unit_dma
 *
 * Allocate MAX_INFLIGHT req/resp buffer pairs as MEMF_SHARED (DMA-capable)
 * memory and establish permanent DMA mappings. Slot 0 is also aliased via
 * unit->req_buf / unit->resp_buf for use by the synchronous VirtIOSCSI_DoIO
 * path (discovery, HD_SCSICMD). All pipeline slots are ready for Submit.
 * ---------------------------------------------------------------------- */
static BOOL alloc_one_slot(struct ExecIFace *IExec, struct VirtIOUSCSIDevUnit *unit, uint32 s)
{
    unit->req_bufs[s] = IExec->AllocVecTags(sizeof(struct virtio_scsi_req_cmd),
                                             AVT_ClearWithValue, 0,
                                             AVT_Type, MEMF_SHARED,
                                             TAG_DONE);
    if (!unit->req_bufs[s])
        return FALSE;

    unit->resp_bufs[s] = IExec->AllocVecTags(sizeof(struct virtio_scsi_resp_cmd),
                                              AVT_ClearWithValue, 0,
                                              AVT_Type, MEMF_SHARED,
                                              TAG_DONE);
    if (!unit->resp_bufs[s]) {
        IExec->FreeVec(unit->req_bufs[s]);
        unit->req_bufs[s] = NULL;
        return FALSE;
    }

    unit->dma_req_entries_arr[s] = IExec->StartDMA(unit->req_bufs[s],
                                                     sizeof(struct virtio_scsi_req_cmd),
                                                     DMA_ReadFromRAM);
    if (unit->dma_req_entries_arr[s] == 0) goto fail_resp;

    unit->dma_req_lists[s] = (struct DMAEntry *)IExec->AllocSysObjectTags(
        ASOT_DMAENTRY, ASODMAE_NumEntries, unit->dma_req_entries_arr[s], TAG_DONE);
    if (!unit->dma_req_lists[s]) {
        IExec->EndDMA(unit->req_bufs[s], sizeof(struct virtio_scsi_req_cmd),
                      DMA_ReadFromRAM | DMAF_NoModify);
        unit->dma_req_entries_arr[s] = 0;
        goto fail_resp;
    }
    IExec->GetDMAList(unit->req_bufs[s], sizeof(struct virtio_scsi_req_cmd),
                      DMA_ReadFromRAM, unit->dma_req_lists[s]);

    unit->dma_resp_entries_arr[s] = IExec->StartDMA(unit->resp_bufs[s],
                                                      sizeof(struct virtio_scsi_resp_cmd),
                                                      0 /* device writes = IN */);
    if (unit->dma_resp_entries_arr[s] == 0) {
        IExec->FreeSysObject(ASOT_DMAENTRY, unit->dma_req_lists[s]);
        unit->dma_req_lists[s] = NULL;
        IExec->EndDMA(unit->req_bufs[s], sizeof(struct virtio_scsi_req_cmd),
                      DMA_ReadFromRAM | DMAF_NoModify);
        unit->dma_req_entries_arr[s] = 0;
        goto fail_resp;
    }
    unit->dma_resp_lists[s] = (struct DMAEntry *)IExec->AllocSysObjectTags(
        ASOT_DMAENTRY, ASODMAE_NumEntries, unit->dma_resp_entries_arr[s], TAG_DONE);
    if (!unit->dma_resp_lists[s]) {
        IExec->EndDMA(unit->resp_bufs[s], sizeof(struct virtio_scsi_resp_cmd), DMAF_NoModify);
        unit->dma_resp_entries_arr[s] = 0;
        IExec->FreeSysObject(ASOT_DMAENTRY, unit->dma_req_lists[s]);
        unit->dma_req_lists[s] = NULL;
        IExec->EndDMA(unit->req_bufs[s], sizeof(struct virtio_scsi_req_cmd),
                      DMA_ReadFromRAM | DMAF_NoModify);
        unit->dma_req_entries_arr[s] = 0;
        goto fail_resp;
    }
    IExec->GetDMAList(unit->resp_bufs[s], sizeof(struct virtio_scsi_resp_cmd),
                      0, unit->dma_resp_lists[s]);
    return TRUE;

fail_resp:
    IExec->FreeVec(unit->resp_bufs[s]);
    unit->resp_bufs[s] = NULL;
    IExec->FreeVec(unit->req_bufs[s]);
    unit->req_bufs[s] = NULL;
    return FALSE;
}

static void free_one_slot(struct ExecIFace *IExec, struct VirtIOUSCSIDevUnit *unit, uint32 s)
{
    if (unit->dma_resp_lists[s]) {
        IExec->FreeSysObject(ASOT_DMAENTRY, unit->dma_resp_lists[s]);
        unit->dma_resp_lists[s] = NULL;
    }
    if (unit->dma_resp_entries_arr[s] > 0 && unit->resp_bufs[s]) {
        IExec->EndDMA(unit->resp_bufs[s], sizeof(struct virtio_scsi_resp_cmd), DMAF_NoModify);
        unit->dma_resp_entries_arr[s] = 0;
    }
    if (unit->dma_req_lists[s]) {
        IExec->FreeSysObject(ASOT_DMAENTRY, unit->dma_req_lists[s]);
        unit->dma_req_lists[s] = NULL;
    }
    if (unit->dma_req_entries_arr[s] > 0 && unit->req_bufs[s]) {
        IExec->EndDMA(unit->req_bufs[s], sizeof(struct virtio_scsi_req_cmd),
                      DMA_ReadFromRAM | DMAF_NoModify);
        unit->dma_req_entries_arr[s] = 0;
    }
    if (unit->resp_bufs[s]) { IExec->FreeVec(unit->resp_bufs[s]); unit->resp_bufs[s] = NULL; }
    if (unit->req_bufs[s])  { IExec->FreeVec(unit->req_bufs[s]);  unit->req_bufs[s]  = NULL; }
}

/* -------------------------------------------------------------------------
 * Bounce buffer helpers
 *
 * One BOUNCE_BUF_SIZE MEMF_SHARED buffer is pre-allocated per inflight slot
 * and kept permanently DMA-mapped.  Submit uses it for transfers that fit
 * (data_len <= BOUNCE_BUF_SIZE), avoiding per-call StartDMA/EndDMA entirely.
 *
 * The buffer is a single contiguous page-aligned allocation so StartDMA
 * always returns exactly one DMA entry.  We store the physical address and
 * entry count; the DMAEntry list itself is freed immediately after GetDMAList
 * since we only need the physical address for the SG entry.
 * ---------------------------------------------------------------------- */
static BOOL alloc_one_bounce(struct ExecIFace *IExec, struct VirtIOUSCSIDevUnit *unit, uint32 s)
{
    unit->bounce_bufs[s] = (uint8 *)IExec->AllocVecTags(BOUNCE_BUF_SIZE,
                                                          AVT_ClearWithValue, 0,
                                                          AVT_Type, MEMF_SHARED,
                                                          TAG_DONE);
    if (!unit->bounce_bufs[s])
        return FALSE;

    /*
     * DMA_ReadFromRAM | ~DMA_ReadFromRAM: we need the bounce buffer
     * accessible in both directions (write: OUT to device; read: IN from
     * device).  Use DMA_ReadFromRAM for the StartDMA — AmigaOS uses the
     * flags only to determine cache coherency direction; MEMF_SHARED is
     * already non-cacheable so both directions work with either flag.
     * Store the physical base; the DMAEntry list is freed immediately.
     */
    uint32 entries = IExec->StartDMA(unit->bounce_bufs[s], BOUNCE_BUF_SIZE, DMA_ReadFromRAM);
    if (entries == 0) {
        IExec->FreeVec(unit->bounce_bufs[s]);
        unit->bounce_bufs[s] = NULL;
        return FALSE;
    }
    unit->bounce_dma_entries[s] = entries;

    struct DMAEntry *tmp = (struct DMAEntry *)IExec->AllocSysObjectTags(
        ASOT_DMAENTRY, ASODMAE_NumEntries, entries, TAG_DONE);
    if (!tmp) {
        IExec->EndDMA(unit->bounce_bufs[s], BOUNCE_BUF_SIZE, DMA_ReadFromRAM | DMAF_NoModify);
        unit->bounce_dma_entries[s] = 0;
        IExec->FreeVec(unit->bounce_bufs[s]);
        unit->bounce_bufs[s] = NULL;
        return FALSE;
    }
    IExec->GetDMAList(unit->bounce_bufs[s], BOUNCE_BUF_SIZE, DMA_ReadFromRAM, tmp);
    unit->bounce_dma_phys[s] = (uint32)tmp[0].PhysicalAddress;
    IExec->FreeSysObject(ASOT_DMAENTRY, tmp);
    /* DMA mapping stays live — EndDMA called in free_one_bounce */
    return TRUE;
}

static void free_one_bounce(struct ExecIFace *IExec, struct VirtIOUSCSIDevUnit *unit, uint32 s)
{
    if (unit->bounce_dma_entries[s] > 0 && unit->bounce_bufs[s]) {
        IExec->EndDMA(unit->bounce_bufs[s], BOUNCE_BUF_SIZE,
                      DMA_ReadFromRAM | DMAF_NoModify);
        unit->bounce_dma_entries[s] = 0;
        unit->bounce_dma_phys[s]    = 0;
    }
    if (unit->bounce_bufs[s]) {
        IExec->FreeVec(unit->bounce_bufs[s]);
        unit->bounce_bufs[s] = NULL;
    }
}

static BOOL preallocate_unit_dma(struct VirtIOSCSIBase *libBase,
                                  struct VirtIOUSCSIDevUnit *unit)
{
    struct ExecIFace *IExec = libBase->IExec;
    uint32 s;

    for (s = 0; s < MAX_INFLIGHT; s++) {
        if (!alloc_one_slot(IExec, unit, s)) {
            DPRINTF(IExec, "[virtioscsi:unit_task.c] preallocate_unit_dma: slot %lu alloc failed\n", s);
            /* Free already-allocated slots */
            uint32 k;
            for (k = 0; k < s; k++) {
                free_one_bounce(IExec, unit, k);
                free_one_slot(IExec, unit, k);
            }
            return FALSE;
        }
        if (!alloc_one_bounce(IExec, unit, s)) {
            DPRINTF(IExec, "[virtioscsi:unit_task.c] preallocate_unit_dma: bounce slot %lu alloc failed\n", s);
            free_one_slot(IExec, unit, s);
            uint32 k;
            for (k = 0; k < s; k++) {
                free_one_bounce(IExec, unit, k);
                free_one_slot(IExec, unit, k);
            }
            return FALSE;
        }
    }

    /* Alias slot 0 for the synchronous DoIO path */
    unit->req_buf          = unit->req_bufs[0];
    unit->resp_buf         = unit->resp_bufs[0];
    unit->dma_req_list     = unit->dma_req_lists[0];
    unit->dma_req_entries  = unit->dma_req_entries_arr[0];
    unit->dma_resp_list    = unit->dma_resp_lists[0];
    unit->dma_resp_entries = unit->dma_resp_entries_arr[0];

    DPRINTF(IExec, "[virtioscsi:unit_task.c] preallocate_unit_dma: unit %lu %d slots ready\n",
            unit->unit_num, MAX_INFLIGHT);
    return TRUE;
}

static void free_unit_dma(struct VirtIOSCSIBase *libBase,
                           struct VirtIOUSCSIDevUnit *unit)
{
    struct ExecIFace *IExec = libBase->IExec;
    uint32 s;

    for (s = 0; s < MAX_INFLIGHT; s++) {
        free_one_bounce(IExec, unit, s);
        free_one_slot(IExec, unit, s);
    }

    /* Clear the slot-0 aliases */
    unit->req_buf          = NULL;
    unit->resp_buf         = NULL;
    unit->dma_req_list     = NULL;
    unit->dma_req_entries  = 0;
    unit->dma_resp_list    = NULL;
    unit->dma_resp_entries = 0;

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

    /*
     * Allocate a persistent signal bit for VirtIO completion notifications.
     * The ISR signals this whenever it detects a VirtIO interrupt for any
     * unit. We register ourselves permanently (not per-request) so the
     * ISR can signal without DoIO-style per-call setup.
     */
    int8 isr_bit = IExec->AllocSignal(-1);
    if (isr_bit < 0) {
        DPRINTF(IExec, "[virtioscsi:unit_task.c] UnitTask_Entry: AllocSignal for ISR failed\n");
        IExec->Signal(startMsg->parent_task, startMsg->ready_mask);
        IExec->FreeSysObject(ASOT_PORT, unit->io_port);
        unit->io_port      = NULL;
        unit->io_port_mask = 0;
        unit->task         = NULL;
        return;
    }
    unit->io_signal_mask = 1UL << isr_bit;
    unit->io_wait_task   = self; /* persistent — ISR signals us on every completion */
    unit->io_cookie      = (void *)1; /* non-NULL so ISR check fires; actual matching is in Harvest */

    /* Signal parent: port is ready, it's safe to PutMsg to us now */
    IExec->Signal(startMsg->parent_task, startMsg->ready_mask);
    /* startMsg is stack-allocated in UnitTask_Start — do NOT touch it after this signal */

    uint32 wait_mask = unit->io_port_mask | unit->io_signal_mask | SIGBREAKF_CTRL_C;

    /* ---- Main pipeline event loop ---- */
    while (!unit->task_shutdown) {
        uint32 sigs = IExec->Wait(wait_mask);

        if (sigs & SIGBREAKF_CTRL_C)
            break;

        /* Harvest completions first — frees inflight slots before we try to fill more */
        if (sigs & unit->io_signal_mask)
            VirtIOSCSI_Harvest(libBase, unit);

        /* Dispatch new requests from the port.
         *
         * Deferred-kick optimisation: drain the entire message queue before
         * notifying the device.  Each VirtIOSCSI_Submit() call does AddBuf
         * (updates avail->idx) but intentionally skips the PCI QUEUE_NOTIFY
         * write.  After all pending messages are dispatched, a single
         * VirtIOSCSI_Kick() flushes the whole batch with one PCI write.
         *
         * For a burst of N queued requests this reduces PCI writes from N to 1.
         * Synchronous commands (geometry, HD_SCSICMD) that call VirtIOSCSI_DoIO
         * issue their own unconditional QUEUE_NOTIFY internally, so they are
         * unaffected by whether we kick at the end of the loop.
         */
        if (sigs & unit->io_port_mask) {
            struct IOStdReq *ioreq;
            BOOL need_kick = FALSE;
            while ((ioreq = (struct IOStdReq *)IExec->GetMsg(unit->io_port)) != NULL) {
                if (UnitTask_Dispatch(libBase, unit, ioreq))
                    need_kick = TRUE;
            }
            /* One kick covers all Submit()s in this batch. */
            if (need_kick)
                VirtIOSCSI_Kick(libBase);
        }
    }

    /* Drain: abort any inflight pipeline requests */
    {
        uint32 s;
        for (s = 0; s < MAX_INFLIGHT; s++) {
            if (unit->inflight[s].ioreq) {
                unit->inflight[s].ioreq->io_Error = IOERR_ABORTED;
                IExec->ReplyMsg((struct Message *)unit->inflight[s].ioreq);
                unit->inflight[s].ioreq  = NULL;
                unit->inflight[s].cookie = NULL;
                if (unit->inflight[s].dma_list) {
                    IExec->FreeSysObject(ASOT_DMAENTRY, unit->inflight[s].dma_list);
                    IExec->EndDMA(unit->inflight[s].dma_addr, unit->inflight[s].dma_size,
                                  unit->inflight[s].dma_flags | DMAF_NoModify);
                    unit->inflight[s].dma_list        = NULL;
                    unit->inflight[s].dma_num_entries = 0;
                }
            }
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

    /* Remove ISR registration */
    unit->io_wait_task   = NULL;
    unit->io_cookie      = NULL;
    unit->io_signal_mask = 0;
    IExec->FreeSignal(isr_bit);

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
 * make_block_cdb
 *
 * Build a READ or WRITE CDB for a block I/O IORequest and call Submit.
 * Returns TRUE if the request was submitted (Harvest will ReplyMsg later).
 * Returns FALSE on hard failure — caller must set io_Error and ReplyMsg.
 *
 * The function handles 32-bit (CMD_READ/WRITE, TD_READ64/WRITE64) and
 * 64-bit NSCMD variants transparently.
 * ---------------------------------------------------------------------- */
static BOOL submit_block_io(struct VirtIOSCSIBase *libBase,
                             struct VirtIOUSCSIDevUnit *unit,
                             struct IOStdReq *ioreq,
                             uint64 offset64, BOOL is_write)
{
    uint32 blksz = (unit->geometry_valid && unit->block_size) ? unit->block_size : 512;

    uint64 lba    = offset64 / blksz;
    uint32 blocks = ioreq->io_Length / blksz;
    if (blocks == 0) blocks = 1;

    uint8  cdb[16];
    uint32 cdb_len;
    if (lba > 0xFFFFFFFFULL) {
        if (is_write) make_write16_cdb(cdb, lba, blocks);
        else          make_read16_cdb(cdb,  lba, blocks);
        cdb_len = 16;
    } else {
        if (is_write) make_write10_cdb(cdb, (uint32)lba, (uint16)blocks);
        else          make_read10_cdb(cdb,  (uint32)lba, (uint16)blocks);
        cdb_len = 10;
    }

    int32 rc = VirtIOSCSI_Submit(libBase, unit, ioreq, cdb, cdb_len, is_write);
    if (rc == 0)
        return TRUE; /* async — Harvest will ReplyMsg */

    if (rc == -1) {
        /* No inflight slot — fall back to synchronous DoIO */
        uint8  scsi_status = 0;
        uint32 residual    = 0;
        int32  sync_rc = VirtIOSCSI_DoIO(libBase, unit, unit->target_id, unit->lun_id,
                                          cdb, cdb_len,
                                          (uint8 *)ioreq->io_Data, ioreq->io_Length,
                                          is_write, &scsi_status, &residual);
        if (sync_rc != 0) {
            ioreq->io_Error  = (BYTE)sync_rc;
            ioreq->io_Actual = 0;
        } else {
            ioreq->io_Error  = 0;
            ioreq->io_Actual = ioreq->io_Length - residual;
        }
    } else {
        /* Hard failure from Submit */
        ioreq->io_Error  = (BYTE)rc;
        ioreq->io_Actual = 0;
    }
    return FALSE; /* caller must ReplyMsg */
}

/* -------------------------------------------------------------------------
 * UnitTask_Dispatch
 *
 * Called from the unit task's event loop for each IORequest dequeued from
 * the message port.
 *
 * Block I/O commands (CMD_READ, CMD_WRITE, TD_*64, NSCMD_TD_*64) are
 * submitted to VirtIO asynchronously via VirtIOSCSI_Submit(). On success
 * the request is HELD in an inflight slot — do NOT call ReplyMsg. Harvest
 * will reply when VirtIO signals completion.
 *
 * All other commands (geometry, SCSI pass-through, no-ops) complete
 * synchronously and are replied to immediately by this function.
 *
 * Returns TRUE if VirtIOSCSI_Submit() succeeded; caller must then kick.
 * ---------------------------------------------------------------------- */
static BOOL UnitTask_Dispatch(struct VirtIOSCSIBase *libBase,
                               struct VirtIOUSCSIDevUnit *unit,
                               struct IOStdReq *ioreq)
{
    struct ExecIFace *IExec = libBase->IExec;
    BOOL submitted = FALSE;

    switch (ioreq->io_Command) {

    /* ---- 32-bit block I/O ---- */
    case CMD_READ:
    case ETD_READ:
        submitted = submit_block_io(libBase, unit, ioreq,
                                    (uint64)ioreq->io_Offset, FALSE);
        break;

    case CMD_WRITE:
    case TD_FORMAT:
    case ETD_FORMAT:
    case ETD_WRITE:
        submitted = submit_block_io(libBase, unit, ioreq,
                                    (uint64)ioreq->io_Offset, TRUE);
        break;

    /* ---- Legacy 64-bit block I/O ---- */
    case TD_READ64:
        submitted = submit_block_io(libBase, unit, ioreq,
                                    ((uint64)ioreq->io_Actual << 32) | ioreq->io_Offset, FALSE);
        break;
    case TD_WRITE64:
    case TD_FORMAT64:
        submitted = submit_block_io(libBase, unit, ioreq,
                                    ((uint64)ioreq->io_Actual << 32) | ioreq->io_Offset, TRUE);
        break;

    /* ---- NSD 64-bit block I/O ---- */
    case NSCMD_TD_READ64:
    case NSCMD_ETD_READ64:
        submitted = submit_block_io(libBase, unit, ioreq,
                                    ((uint64)ioreq->io_Actual << 32) | ioreq->io_Offset, FALSE);
        break;
    case NSCMD_TD_WRITE64:
    case NSCMD_TD_FORMAT64:
    case NSCMD_ETD_WRITE64:
    case NSCMD_ETD_FORMAT64:
        submitted = submit_block_io(libBase, unit, ioreq,
                                    ((uint64)ioreq->io_Actual << 32) | ioreq->io_Offset, TRUE);
        break;

    /* ---- Synchronous commands — reply immediately ---- */
    case CMD_UPDATE:
    case CMD_FLUSH:
    case ETD_UPDATE:
        Handle_CMD_Update(libBase, ioreq);
        break;

    case TD_GETGEOMETRY:
        Handle_TD_GetGeometry(libBase, ioreq);
        break;

    case HD_SCSICMD:
        Parse_SCSI_Command(libBase, ioreq);
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

    /* For synchronous commands (not submitted asynchronously), reply now */
    if (!submitted)
        IExec->ReplyMsg((struct Message *)ioreq);

    return submitted; /* TRUE = AddBuf done, caller must VirtIOSCSI_Kick() */
}
