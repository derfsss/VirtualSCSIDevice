#include "unit_task.h"
#include "cmd_names.h"
#include "virtioscsi.h"
#include "virtioscsi_cmds.h"
#include "scsi_cdb_helpers.h"
#include "virtio/virtio_scsi_io.h"
#include "virtio/virtqueue.h"
#include <exec/exec.h>
#include <exec/exectags.h>
#include <exec/memory.h>

/* SandboxVM-private AllocVecTags tag -- see comment in virtqueue.c.
 * Routes DMA buffers through the SandboxVM host's real allocator so
 * StartDMA/GetDMAList accept them. Unknown tag value on native AOS4
 * (silently ignored by the utility.library tag walker). Must stay
 * in sync with SandboxVM/VM-OS4/include/sbvm_tags.h. */
#ifndef SBV_AVT_HostDMA
#define SBV_AVT_HostDMA        (0x80535601u)
#endif

/* Forward declaration -- dispatch is defined below.
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
                                             SBV_AVT_HostDMA, 0,
                                             TAG_DONE);
    if (!unit->req_bufs[s])
        return FALSE;

    unit->resp_bufs[s] = IExec->AllocVecTags(sizeof(struct virtio_scsi_resp_cmd),
                                              AVT_ClearWithValue, 0,
                                              AVT_Type, MEMF_SHARED,
                                              SBV_AVT_HostDMA, 0,
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
                                                          SBV_AVT_HostDMA, 0,
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

    /*
     * Release the DMA mapping immediately — the physical address is cached
     * in bounce_dma_phys[s].  EndDMA with DMAF_NoModify restores the buffer
     * to normal cacheable state, allowing CopyMem to operate at L1/L2 speed.
     * Cache coherency is handled explicitly in Submit (CacheClearE CACRF_ClearD
     * flushes dirty lines to RAM before device reads) and Harvest (CacheClearE
     * CACRF_InvalidateD invalidates stale lines before CPU reads device data).
     */
    IExec->EndDMA(unit->bounce_bufs[s], BOUNCE_BUF_SIZE, DMA_ReadFromRAM | DMAF_NoModify);
    unit->bounce_dma_entries[s] = 0;

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

    /* Initialise inflight free list: all slots free, linked 0→1→...→15→-1 */
    for (s = 0; s < MAX_INFLIGHT - 1; s++)
        unit->inflight_next[s] = (int32)(s + 1);
    unit->inflight_next[MAX_INFLIGHT - 1] = -1;
    unit->free_head = 0;

    for (s = 0; s < MAX_INFLIGHT; s++) {
        if (!alloc_one_slot(IExec, unit, s)) {
            DPRINTF(IExec, "[virtioscsi:unit_task.c] preallocate_unit_dma: slot %lu alloc failed\n", s);
            /* Free already-allocated slots */
            uint32 k;
            for (k = 0; k < s; k++) {
                if (unit->data_dma_pool[k]) {
                    IExec->FreeSysObject(ASOT_DMAENTRY, unit->data_dma_pool[k]);
                    unit->data_dma_pool[k] = NULL;
                }
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
                if (unit->data_dma_pool[k]) {
                    IExec->FreeSysObject(ASOT_DMAENTRY, unit->data_dma_pool[k]);
                    unit->data_dma_pool[k] = NULL;
                }
                free_one_bounce(IExec, unit, k);
                free_one_slot(IExec, unit, k);
            }
            return FALSE;
        }
        /* Allocate DMA entry pool for direct-DMA path (>BOUNCE_BUF_SIZE transfers) */
        unit->data_dma_pool[s] = (struct DMAEntry *)IExec->AllocSysObjectTags(
            ASOT_DMAENTRY, ASODMAE_NumEntries, MAX_SG_ENTRIES, TAG_DONE);
        if (!unit->data_dma_pool[s]) {
            DPRINTF(IExec, "[virtioscsi:unit_task.c] preallocate_unit_dma: DMA pool slot %lu alloc failed\n", s);
            free_one_bounce(IExec, unit, s);
            free_one_slot(IExec, unit, s);
            uint32 k;
            for (k = 0; k < s; k++) {
                if (unit->data_dma_pool[k]) {
                    IExec->FreeSysObject(ASOT_DMAENTRY, unit->data_dma_pool[k]);
                    unit->data_dma_pool[k] = NULL;
                }
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
        if (unit->data_dma_pool[s]) {
            IExec->FreeSysObject(ASOT_DMAENTRY, unit->data_dma_pool[s]);
            unit->data_dma_pool[s] = NULL;
        }
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

    /* Reset free list */
    unit->free_head = -1;

    DPRINTF(IExec, "[virtioscsi:unit_task.c] free_unit_dma: unit %lu DMA freed\n", unit->unit_num);
}

/* -------------------------------------------------------------------------
 * UnitTask_Entry
 *
 * Entry point for each unit's device task. Exec calls this function in the
 * new task's context. startMsg arrives via AT_Param1 from CreateTaskTags.
 * ---------------------------------------------------------------------- */
static void UnitTask_Entry(struct UnitTaskStartMsg *startMsg)
{
    struct VirtIOSCSIBase    *libBase = startMsg->libBase;
    struct VirtIOUSCSIDevUnit *unit   = startMsg->unit;
    struct ExecIFace *IExec = libBase->IExec;
    struct Task *self = IExec->FindTask(NULL);

    DPRINTF(IExec, "[virtioscsi:unit_task.c] UnitTask_Entry: unit %lu started\n", unit->unit_num);

    /* Allocate our message port */
    unit->io_port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    if (!unit->io_port) {
        DPRINTF(IExec, "[virtioscsi:unit_task.c] UnitTask_Entry: AllocSysObject(PORT) failed\n");
        IExec->Signal(startMsg->parent_task, startMsg->ready_mask);
        return;
    }

    /* Mutex protecting io_port queue traversal (AbortIO vs GetMsg).
     * Replaces deprecated Forbid/Permit per AmigaOS 4.x guidelines. */
    unit->port_mutex = IExec->AllocSysObjectTags(ASOT_MUTEX, TAG_DONE);
    if (!unit->port_mutex) {
        DPRINTF(IExec, "[virtioscsi:unit_task.c] UnitTask_Entry: AllocSysObject(MUTEX) failed\n");
        IExec->FreeSysObject(ASOT_PORT, unit->io_port);
        unit->io_port = NULL;
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
        DPRINTF(IExec, "[virtioscsi:unit_task.c] unit %lu: Wait(mask=0x%08lx) open_count=%lu\n",
                unit->unit_num, wait_mask, unit->open_count);
        uint32 sigs = IExec->Wait(wait_mask);
        DPRINTF(IExec, "[virtioscsi:unit_task.c] unit %lu: woke sigs=0x%08lx (io=%s irq=%s brk=%s)\n",
                unit->unit_num, sigs,
                (sigs & unit->io_port_mask)    ? "Y" : "-",
                (sigs & unit->io_signal_mask)  ? "Y" : "-",
                (sigs & SIGBREAKF_CTRL_C)      ? "Y" : "-");

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
            for (;;) {
                IExec->MutexObtain(unit->port_mutex);
                ioreq = (struct IOStdReq *)IExec->GetMsg(unit->io_port);
                IExec->MutexRelease(unit->port_mutex);
                if (!ioreq)
                    break;
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
                DPRINTF(IExec, "[virtioscsi:unit_task.c] Shutdown: aborting inflight slot %lu cmd %lu\n",
                        s, (uint32)unit->inflight[s].ioreq->io_Command);
                unit->inflight[s].ioreq->io_Error = IOERR_ABORTED;
                IExec->ReplyMsg((struct Message *)unit->inflight[s].ioreq);
                unit->inflight[s].ioreq  = NULL;
                unit->inflight[s].cookie = NULL;
                if (unit->inflight[s].dma_list) {
                    /* dma_list points into data_dma_pool — do NOT FreeSysObject */
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
        for (;;) {
            IExec->MutexObtain(unit->port_mutex);
            ioreq = (struct IOStdReq *)IExec->GetMsg(unit->io_port);
            IExec->MutexRelease(unit->port_mutex);
            if (!ioreq)
                break;
            ioreq->io_Error = IOERR_ABORTED;
            IExec->ReplyMsg((struct Message *)ioreq);
        }
    }

    /* Remove ISR registration */
    unit->io_wait_task   = NULL;
    unit->io_cookie      = NULL;
    unit->io_signal_mask = 0;
    IExec->FreeSignal(isr_bit);

    /* Null out io_port FIRST to prevent new AbortIO callers from entering,
     * then free the mutex and port. */
    {
        struct MsgPort *port = unit->io_port;
        unit->io_port      = NULL;
        unit->io_port_mask = 0;

        if (unit->port_mutex) {
            IExec->FreeSysObject(ASOT_MUTEX, unit->port_mutex);
            unit->port_mutex = NULL;
        }
        IExec->FreeSysObject(ASOT_PORT, port);
    }

    DPRINTF(IExec, "[virtioscsi:unit_task.c] UnitTask_Entry: unit %lu exiting\n", unit->unit_num);

    /*
     * Shutdown handshake: capture the exit signal target, clear unit->task,
     * then signal the shutdown caller.  If shutdown_exit_mask is 0 (rare:
     * AllocSignal failed in the shutdown caller), fall back to CTRL_C so
     * the caller's Wait(SIGBREAKF_CTRL_C) loop wakes.
     */
    {
        struct Task *exit_task = unit->shutdown_exit_task;
        uint32       exit_mask = unit->shutdown_exit_mask;
        unit->task = NULL;
        if (exit_task)
            IExec->Signal(exit_task, exit_mask ? exit_mask : SIGBREAKF_CTRL_C);
    }
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

    /* Write the task name into the unit struct so it outlives this stack
     * frame — CreateTaskTags stores the pointer (ln_Name), not a copy. */
    libBase->IUtility->SNPrintf(unit->task_name, sizeof(unit->task_name),
                                "virtioscsi unit %lu", unit->unit_num);

    /* Pass the start message via AT_Param1 — no Forbid/Permit needed.
     * This is the same pattern the event task uses (virtio_events.c). */
    struct Task *task = IExec->CreateTaskTags(unit->task_name, 5,
                                              UnitTask_Entry, 16384,
                                              AT_Param1, (uint32)&startMsg,
                                              TAG_DONE);

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

    /* Signal-based exit handshake (same pattern as ShutdownEventQueue).
     * Allocate a signal bit in THIS task's context; the unit task signals
     * us on exit.  Avoids deprecated Forbid/Permit busy-wait. */
    int8 bit = IExec->AllocSignal(-1);
    uint32 mask = (bit >= 0) ? (1UL << bit) : 0;

    unit->shutdown_exit_task = IExec->FindTask(NULL);
    unit->shutdown_exit_mask = mask;
    unit->task_shutdown = TRUE;

    struct Task *t = unit->task; /* capture before it clears itself */
    IExec->Signal(t, SIGBREAKF_CTRL_C);

    if (mask) {
        IExec->Wait(mask);
        IExec->FreeSignal(bit);
    } else {
        /* Rare fallback: AllocSignal failed.  The exiting task sends
         * SIGBREAKF_CTRL_C back when shutdown_exit_mask is 0. */
        while (unit->task)
            IExec->Wait(SIGBREAKF_CTRL_C);
    }

    unit->shutdown_exit_task = NULL;
    unit->shutdown_exit_mask = 0;

    /* Free DMA buffers AFTER the task has fully exited.  Previously this
     * ran before the exit signal, leaving dangling DMA pointers that the
     * task's drain loop could dereference. */
    free_unit_dma(libBase, unit);

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
        DPRINTF(libBase->IExec, "[virtioscsi:unit_task.c] submit_block_io: no free slot, falling back to DoIO\n");
        uint8  scsi_status = 0;
        uint32 residual    = 0;
        int32  sync_rc = VirtIOSCSI_DoIO(libBase, unit, unit->target_id, unit->lun_id,
                                          cdb, cdb_len,
                                          (uint8 *)ioreq->io_Data, ioreq->io_Length,
                                          is_write, &scsi_status, &residual);
        if (sync_rc != 0) {
            DPRINTF(libBase->IExec, "[virtioscsi:unit_task.c] submit_block_io: DoIO fallback failed rc=%ld\n",
                    (long)sync_rc);
            ioreq->io_Error  = (BYTE)sync_rc;
            ioreq->io_Actual = 0;
        } else {
            ioreq->io_Error  = 0;
            ioreq->io_Actual = ioreq->io_Length - residual;
        }
    } else {
        /* Hard failure from Submit */
        DPRINTF(libBase->IExec, "[virtioscsi:unit_task.c] submit_block_io: Submit HARD FAIL rc=%ld cmd=0x%02X len=%lu\n",
                (long)rc, (uint32)cdb[0], (uint32)ioreq->io_Length);
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
        DPRINTF(IExec, "[virtioscsi:unit_task.c] UnitTask_Dispatch: UNKNOWN cmd %lu (%s) — IOERR_NOCMD\n",
                (uint32)ioreq->io_Command, GetCommandName(ioreq->io_Command));
        ioreq->io_Error = IOERR_NOCMD;
        break;
    }

    /* For synchronous commands (not submitted asynchronously), reply now */
    if (!submitted) {
        struct MsgPort *rp = ioreq->io_Message.mn_ReplyPort;
        DPRINTF(IExec,
                "[virtioscsi:unit_task.c] ReplyMsg: ioreq=%p cmd=%lu err=%ld actual=%lu port=%p\n"
                "  port pre-reply: mp_Flags=0x%02x mp_SigBit=%d mp_SigTask=%p lh_Head=%p lh_TailPred=%p\n",
                ioreq, (uint32)ioreq->io_Command, (long)(int8)ioreq->io_Error,
                (uint32)ioreq->io_Actual, rp,
                rp ? (uint32)rp->mp_Flags : 0,
                rp ? (int)rp->mp_SigBit : -1,
                rp ? (void *)rp->mp_SigTask : NULL,
                rp ? (void *)rp->mp_MsgList.lh_Head : NULL,
                rp ? (void *)rp->mp_MsgList.lh_TailPred : NULL);
        IExec->ReplyMsg((struct Message *)ioreq);
        /* Re-read after: if the reply landed, lh_Head should point to our
         * ioreq (as the first queued reply), OR if the receiver was very
         * fast, the port is empty and lh_Head == &lh_Tail (self-ref sentinel). */
        DPRINTF(IExec,
                "  port post-reply: lh_Head=%p lh_TailPred=%p  (ioreq in list? Succ=%p Pred=%p)\n",
                rp ? (void *)rp->mp_MsgList.lh_Head : NULL,
                rp ? (void *)rp->mp_MsgList.lh_TailPred : NULL,
                ioreq->io_Message.mn_Node.ln_Succ,
                ioreq->io_Message.mn_Node.ln_Pred);
        /* If the target task is identifiable, dump its run state.
         * tc_State: 0=INVALID, 1=ADDED, 2=RUN, 3=READY, 4=WAIT, 5=EXCEPT,
         * 6=REMOVED.  Bit we signaled is stored in tc_SigAlloc masks.
         * This tells us whether the receiving task is actually blocked on
         * our signal or stuck elsewhere. */
        struct Task *tgt = rp ? rp->mp_SigTask : NULL;
        if (tgt) {
            DPRINTF(IExec,
                    "  target task %p '%s': tc_State=%d tc_SigRecvd=0x%08lx tc_SigWait=0x%08lx tc_SigAlloc=0x%08lx\n",
                    tgt,
                    tgt->tc_Node.ln_Name ? tgt->tc_Node.ln_Name : "?",
                    (int)tgt->tc_State,
                    (uint32)tgt->tc_SigRecvd,
                    (uint32)tgt->tc_SigWait,
                    (uint32)tgt->tc_SigAlloc);
        }
    }

    return submitted; /* TRUE = AddBuf done, caller must VirtIOSCSI_Kick() */
}
