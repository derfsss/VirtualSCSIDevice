#include "virtio/virtio_events.h"
#include "virtio/virtio_mounter.h"
#include "virtio/virtio_scsi.h"
#include "virtio/virtio_scsi_io.h"
#include "virtio/virtqueue.h"
#include "unit_discovery.h"
#include "virtioscsi.h"
#include <exec/exectags.h>
#include <exec/memory.h>
#include <exec/tasks.h>

/*
 * Phase 9: VirtIO SCSI event queue handling.
 *
 * The device writes a 16-byte virtio_scsi_event into each device-writable
 * buffer it dequeues from VQ1 (eventq).  We keep a pool of VIRTIO_SCSI_EVENT_BUFS
 * buffers permanently posted; whenever one returns with an event, the event
 * consumer task processes it and re-posts it.
 *
 * A dedicated worker task serves VQ1 rather than piggybacking on the unit
 * tasks, because:
 *   - The eventq has no associated unit (events can concern any T/L).
 *   - The handler may need to call VirtIOSCSI_DoIO() to probe a new LUN,
 *     which itself waits on a per-task signal — unsafe from a unit task
 *     that is itself the one servicing completions.
 *   - Keeps the unit-task hot path free of event-handling branches.
 */

/*
 * Byte-swap helper for reading LE event fields into native uint32.
 * Events are always LE in modern mode (the only mode that negotiates HOTPLUG).
 */
static inline uint32 ev_r32(uint32 v)
{
    return __builtin_bswap32(v);
}

/*
 * Post one event buffer to VQ1 as a device-writable entry.
 * slot_phys is the pre-computed physical address of event_pool + slot * sizeof(event).
 */
static BOOL post_event_buf(struct VirtIOSCSIBase *libBase, uint32 slot)
{
    struct ExecIFace *IExec = libBase->IExec;
    struct virtqueue *evq = libBase->vqs[1];
    if (!evq)
        return FALSE;

    uint32 slot_phys = libBase->event_pool_phys + slot * sizeof(struct virtio_scsi_event);
    uint8 *slot_virt = libBase->event_pool + slot * sizeof(struct virtio_scsi_event);

    /* Reset slot memory so we can detect whether the device actually wrote */
    for (uint32 i = 0; i < sizeof(struct virtio_scsi_event); i++)
        slot_virt[i] = 0;

    struct vring_sg sg;
    sg.addr = slot_phys;
    sg.len  = sizeof(struct virtio_scsi_event);

    /* out_num=0, in_num=1 — device writes into our buffer.  Cookie encodes
     * slot+1 (so 0 is never used as a cookie — VirtQueue_GetBuf treats NULL
     * returns as "ring empty"). */
    int32 rc = VirtQueue_AddBuf(IExec, evq, &sg, 0, 1, (void *)(uint32)(slot + 1));
    if (rc != 0) {
        DPRINTF(IExec, "[virtioscsi:virtio_events.c] post_event_buf slot %lu AddBuf failed\n", slot);
        return FALSE;
    }
    return TRUE;
}

/*
 * Resolve a SAM LUN to our internal target/lun pair.
 * Per virtio-scsi spec: lun[0]=1, lun[1]=target_id, lun[2..3]=2-byte LUN (top 2 bits = addressing).
 * Returns TRUE if the LUN encoding looks valid.
 */
static BOOL parse_sam_lun(const uint8 lun[8], uint32 *target_out, uint32 *lun_out)
{
    if (lun[0] != 1)
        return FALSE;
    *target_out = lun[1];
    /* Single-level LUN addressing: bits 13:8 and 7:0 of lun[2..3].  Top 2 bits
     * of lun[2] encode the addressing method; we only accept 00 (peripheral). */
    uint32 addr_method = (lun[2] >> 6) & 0x3;
    if (addr_method != 0)
        return FALSE;
    *lun_out = ((uint32)(lun[2] & 0x3F) << 8) | lun[3];
    return TRUE;
}

/*
 * Find the units[] slot whose target/lun matches.
 * Returns the slot index [0..7] or -1 if not found.
 */
static int32 find_unit_slot(struct VirtIOSCSIBase *libBase, uint32 target, uint32 lun)
{
    for (int32 i = 0; i < 8; i++) {
        struct VirtIOUSCSIDevUnit *u = libBase->units[i];
        if (u && u->target_id == target && u->lun_id == lun)
            return i;
    }
    return -1;
}

/*
 * Probe a single target/LUN via INQUIRY.  On success, if it's a disk or CD
 * and there is a free slot in units[], allocate a unit struct and install it.
 * Returns TRUE if a new unit was added.
 *
 * Does NOT start a unit task for the new unit, does NOT call mounter —
 * those integrations are deferred (see ROADMAP Phase 10).  The unit is
 * visible to subsequent OpenDevice() calls; boot-time diskboot.kmod has
 * already completed so DOSNode creation would require mounter.library.
 */
static BOOL probe_and_add(struct VirtIOSCSIBase *libBase, uint32 target, uint32 lun)
{
    struct ExecIFace *IExec = libBase->IExec;

    if (find_unit_slot(libBase, target, lun) >= 0) {
        DPRINTF(IExec, "[virtioscsi:virtio_events.c] probe T%lu L%lu: already registered\n", target, lun);
        return FALSE;
    }

    int32 free_slot = -1;
    for (int32 i = 0; i < 8; i++) {
        if (!libBase->units[i]) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0) {
        DPRINTF(IExec, "[virtioscsi:virtio_events.c] probe T%lu L%lu: no free unit slot\n", target, lun);
        return FALSE;
    }

    uint8 *inq = IExec->AllocVecTags(36, AVT_Type, MEMF_SHARED, AVT_ClearWithValue, 0, TAG_END);
    if (!inq)
        return FALSE;

    uint8 cdb[6] = {0x12, 0, 0, 0, 36, 0}; /* INQUIRY */
    uint8 scsi_status = 0;
    uint32 residual = 0;
    int32 rc = VirtIOSCSI_DoIO(libBase, NULL, target, lun,
                               cdb, 6, inq, 36, FALSE, &scsi_status, &residual);
    BOOL added = FALSE;
    if (rc == 0 && scsi_status == 0) {
        uint8 qual    = inq[0] >> 5;
        uint8 devType = inq[0] & 0x1F;
        if (qual != 0x03 && (devType == 0x00 || devType == 0x05)) {
            struct VirtIOUSCSIDevUnit *unit = (struct VirtIOUSCSIDevUnit *)IExec->AllocVecTags(
                sizeof(struct VirtIOUSCSIDevUnit),
                AVT_Type, MEMF_SHARED, AVT_ClearWithValue, 0, TAG_END);
            if (unit) {
                unit->unit_num     = (uint32)free_slot;
                unit->target_id    = target;
                unit->lun_id       = lun;
                unit->media_present = TRUE;
                unit->announced    = FALSE;
                init_dev_unit(IExec, unit);
                libBase->units[free_slot] = unit;
                DPRINTF(IExec,
                        "[virtioscsi:virtio_events.c] HOTPLUG: added unit %ld (T%lu L%lu) type=0x%02X\n",
                        free_slot, target, lun, (uint32)devType);
                added = TRUE;

                /* Phase 10: hand the unit over to mounter.library so a
                 * DOSNode is created and the disk appears on the desktop.
                 * Non-fatal if mounter is unavailable — the unit remains
                 * accessible via OpenDevice(). */
                AnnounceIfHotAdded(libBase, unit);
            }
        } else {
            DPRINTF(IExec, "[virtioscsi:virtio_events.c] probe T%lu L%lu: not a disk/CD (type=0x%02X qual=0x%02X)\n",
                    target, lun, (uint32)devType, (uint32)qual);
        }
    } else {
        DPRINTF(IExec, "[virtioscsi:virtio_events.c] probe T%lu L%lu: INQUIRY failed (rc=%ld status=0x%02X)\n",
                target, lun, rc, (uint32)scsi_status);
    }
    IExec->FreeVec(inq);
    return added;
}

/*
 * Record a media state change on an existing unit and wake any filesystem
 * waiting on TD_ADDCHANGEINT.
 *
 * present:   TRUE  = medium inserted, FALSE = medium ejected
 * invalidate_geom: TRUE drops cached READ CAPACITY info so the next access
 *                  re-reads geometry.  Always TRUE when the medium changes.
 *
 * AmigaOS filesystem convention (CDFileSystem, SFS):
 *   - TD_ADDCHANGEINT is "held" by the driver (not replied to).
 *   - On any media change, the driver ReplyMsg()s the held request.
 *   - The filesystem wakes, re-reads, and re-queues TD_ADDCHANGEINT.
 *   - TD_REMCHANGEINT is the orderly teardown path.
 */
static void notify_media_change(struct VirtIOSCSIBase *libBase,
                                struct VirtIOUSCSIDevUnit *unit,
                                BOOL present)
{
    struct ExecIFace *IExec = libBase->IExec;

    unit->media_present  = present;
    unit->change_count++;
    unit->geometry_valid = FALSE;

    if (unit->changeint_req) {
        DPRINTF(IExec, "[virtioscsi:virtio_events.c] waking changeint on T%lu L%lu (%s, count=%lu)\n",
                unit->target_id, unit->lun_id,
                present ? "inserted" : "ejected", unit->change_count);
        unit->changeint_req->io_Error = 0;
        IExec->ReplyMsg((struct Message *)unit->changeint_req);
        unit->changeint_req = NULL;
    }

    /* TD_REMOVE follows the same held-request pattern but fires on device
     * removal rather than media change.  For media change alone we don't
     * touch it — it fires on the REMOVED transport event, below. */
}

/*
 * Handle one decoded event.
 */
static void handle_event(struct VirtIOSCSIBase *libBase, const struct virtio_scsi_event *ev)
{
    struct ExecIFace *IExec = libBase->IExec;
    uint32 event  = ev_r32(ev->event);
    uint32 reason = ev_r32(ev->reason);

    if (event & VIRTIO_SCSI_T_EVENTS_MISSED) {
        DPRINTF(IExec, "[virtioscsi:virtio_events.c] EVENTS_MISSED flag set — one or more events dropped\n");
        event &= ~VIRTIO_SCSI_T_EVENTS_MISSED;
    }

    uint32 target = 0, lun = 0;
    BOOL lun_ok = parse_sam_lun(ev->lun, &target, &lun);

    switch (event) {
    case VIRTIO_SCSI_T_NO_EVENT:
        /* Buffer returned without a real event (e.g. after device reset). */
        return;

    case VIRTIO_SCSI_T_TRANSPORT_RESET:
        if (!lun_ok) {
            DPRINTF(IExec, "[virtioscsi:virtio_events.c] TRANSPORT_RESET reason=%lu with invalid LUN bytes\n", reason);
            return;
        }
        switch (reason) {
        case VIRTIO_SCSI_EVT_RESET_RESCAN: {
            /* Two sub-cases:
             *   1. The T/L already has a unit → treat as "medium inserted"
             *      (CD change path).  Wake changeint, re-read geometry.
             *   2. The T/L has no unit yet → new device hotplug, probe.
             */
            int32 slot = find_unit_slot(libBase, target, lun);
            if (slot >= 0) {
                DPRINTF(IExec,
                        "[virtioscsi:virtio_events.c] RESET_RESCAN T%lu L%lu — existing unit %ld, medium inserted\n",
                        target, lun, slot);
                notify_media_change(libBase, libBase->units[slot], TRUE);
            } else {
                DPRINTF(IExec, "[virtioscsi:virtio_events.c] RESET_RESCAN T%lu L%lu — new device, probing\n", target, lun);
                probe_and_add(libBase, target, lun);
            }
            break;
        }
        case VIRTIO_SCSI_EVT_RESET_REMOVED: {
            int32 slot = find_unit_slot(libBase, target, lun);
            if (slot >= 0) {
                DPRINTF(IExec, "[virtioscsi:virtio_events.c] RESET_REMOVED T%lu L%lu — unit %ld, medium ejected\n",
                        target, lun, slot);
                struct VirtIOUSCSIDevUnit *unit = libBase->units[slot];
                notify_media_change(libBase, unit, FALSE);
                /* Keep the unit slot allocated so filesystems can still issue
                 * TD_CHANGESTATE and get the "no disk" answer.  A fresh
                 * RESET_RESCAN on the same T/L will flip media_present back.
                 *
                 * If the unit was auto-mounted (hot-added path), denounce it
                 * from mounter.library so the DOSNode is torn down properly.
                 * Boot-time units were never announced by us; the flag
                 * guards against a spurious DenounceDevice on those. */
                DenounceIfAnnounced(libBase, unit);
                libBase->active_units_mask &= ~(1U << slot);
            } else {
                DPRINTF(IExec, "[virtioscsi:virtio_events.c] RESET_REMOVED T%lu L%lu — no matching unit\n",
                        target, lun);
            }
            break;
        }
        case VIRTIO_SCSI_EVT_RESET_HARD:
            DPRINTF(IExec, "[virtioscsi:virtio_events.c] RESET_HARD — device-wide reset reported\n");
            /* Every unit sees a media change; wake all pending changeints. */
            for (int32 i = 0; i < 8; i++) {
                struct VirtIOUSCSIDevUnit *u = libBase->units[i];
                if (u)
                    notify_media_change(libBase, u, u->media_present);
            }
            break;
        default:
            DPRINTF(IExec, "[virtioscsi:virtio_events.c] TRANSPORT_RESET T%lu L%lu unknown reason=%lu\n",
                    target, lun, reason);
            break;
        }
        return;

    case VIRTIO_SCSI_T_PARAM_CHANGE: {
        if (!lun_ok) {
            DPRINTF(IExec, "[virtioscsi:virtio_events.c] PARAM_CHANGE reason=0x%08lX invalid LUN\n", reason);
            return;
        }
        int32 slot = find_unit_slot(libBase, target, lun);
        if (slot < 0)
            return;
        struct VirtIOUSCSIDevUnit *unit = libBase->units[slot];

        /* reason packs ASCQ:ASC in the low two bytes (ASC = byte 0, ASCQ = byte 1). */
        uint8 asc  = (uint8)(reason & 0xFF);
        uint8 ascq = (uint8)((reason >> 8) & 0xFF);

        /* Well-known ASC codes we care about.  Anything else counts as a
         * generic change — bump counter, invalidate geometry, wake listener. */
        switch (asc) {
        case 0x28:
            /* NOT READY TO READY CHANGE, MEDIUM MAY HAVE CHANGED (ascq 0x00).
             * This is the CD/DVD "just inserted" notification. */
            DPRINTF(IExec, "[virtioscsi:virtio_events.c] PARAM_CHANGE T%lu L%lu: medium inserted (asc=%02X ascq=%02X)\n",
                    target, lun, (uint32)asc, (uint32)ascq);
            notify_media_change(libBase, unit, TRUE);
            break;
        case 0x3A:
            /* MEDIUM NOT PRESENT — ejected. */
            DPRINTF(IExec, "[virtioscsi:virtio_events.c] PARAM_CHANGE T%lu L%lu: medium ejected (asc=%02X ascq=%02X)\n",
                    target, lun, (uint32)asc, (uint32)ascq);
            notify_media_change(libBase, unit, FALSE);
            break;
        case 0x2A: /* MODE PARAMETERS CHANGED / CAPACITY DATA HAS CHANGED */
        case 0x3F: /* REPORTED LUNS DATA HAS CHANGED */
        default:
            DPRINTF(IExec,
                    "[virtioscsi:virtio_events.c] PARAM_CHANGE T%lu L%lu: asc=%02X ascq=%02X — generic change\n",
                    target, lun, (uint32)asc, (uint32)ascq);
            /* Keep current media_present state; only bump counter and drop
             * cached geometry so the next access re-reads capacity. */
            notify_media_change(libBase, unit, unit->media_present);
            break;
        }
        return;
    }

    case VIRTIO_SCSI_T_ASYNC_NOTIFY:
        DPRINTF(IExec, "[virtioscsi:virtio_events.c] ASYNC_NOTIFY T%lu L%lu reason=0x%08lX\n",
                (uint32)(lun_ok ? target : 0), (uint32)(lun_ok ? lun : 0), reason);
        return;

    default:
        DPRINTF(IExec, "[virtioscsi:virtio_events.c] Unknown event type %lu\n", event);
        return;
    }
}

/*
 * Event consumer task body.  Runs until event_task_shutdown is set.
 * libBase is passed via AT_Param1 on CreateTaskTags.
 */
static void event_task_entry(struct VirtIOSCSIBase *libBase)
{
    struct ExecIFace *IExec = libBase->IExec;

    /* Allocate our signal bit; ISR will use event_signal_mask to wake us */
    BYTE bit = IExec->AllocSignal(-1);
    if (bit < 0) {
        DPRINTF(IExec, "[virtioscsi:virtio_events.c] event_task: AllocSignal failed\n");
        libBase->event_task = NULL;
        return;
    }
    libBase->event_signal_bit  = (uint8)bit;
    libBase->event_signal_mask = 1UL << bit;

    DPRINTF(IExec, "[virtioscsi:virtio_events.c] event_task started (signal bit %ld)\n", (int32)bit);

    while (!libBase->event_task_shutdown) {
        /* Drain VQ1 until empty */
        struct virtqueue *evq = libBase->vqs[1];
        BOOL reposted = FALSE;
        if (evq) {
            uint32 written;
            void *cookie;
            while ((cookie = VirtQueue_GetBuf(IExec, evq, &written)) != NULL) {
                uint32 slot = ((uint32)cookie) - 1;
                if (slot >= VIRTIO_SCSI_EVENT_BUFS)
                    continue;
                struct virtio_scsi_event *ev =
                    (struct virtio_scsi_event *)(libBase->event_pool + slot * sizeof(struct virtio_scsi_event));
                /* Ensure cache coherency — the device wrote via DMA */
                IExec->CacheClearE(ev, sizeof(*ev), CACRF_InvalidateD);
                handle_event(libBase, ev);

                /* Re-post the buffer */
                if (post_event_buf(libBase, slot))
                    reposted = TRUE;
            }
        }

        /* If we re-posted anything, kick the device so it can write new events */
        if (reposted && evq && libBase->pciDevice)
            VirtQueue_Kick(IExec, evq, libBase->pciDevice,
                           libBase->bar0 ? (uint32)libBase->bar0->Physical : 0);

        if (libBase->event_task_shutdown)
            break;

        /* Wait for the ISR to signal us */
        IExec->Wait(libBase->event_signal_mask);
    }

    IExec->FreeSignal(bit);
    DPRINTF(IExec, "[virtioscsi:virtio_events.c] event_task exiting\n");

    /* Signal the shutdown caller (may be NULL if shutdown never called).
     * Capture pointers/mask locally so we can clear event_task last — once
     * event_task is NULL, ShutdownEventQueue may begin tearing down state. */
    struct Task *exit_task = libBase->event_exit_task;
    uint32       exit_mask = libBase->event_exit_mask;
    libBase->event_task = NULL;
    if (exit_task)
        IExec->Signal(exit_task, exit_mask);
}

BOOL InitEventQueue(struct VirtIOSCSIBase *libBase)
{
    struct ExecIFace *IExec = libBase->IExec;

    if (!libBase->events_enabled) {
        DPRINTF(IExec, "[virtioscsi:virtio_events.c] HOTPLUG/CHANGE not negotiated — event queue idle\n");
        return TRUE;
    }

    if (!libBase->vqs[1]) {
        DPRINTF(IExec, "[virtioscsi:virtio_events.c] VQ1 not configured — cannot start event queue\n");
        libBase->events_enabled = FALSE;
        return FALSE;
    }

    /* Allocate the event buffer pool */
    libBase->event_pool_size = VIRTIO_SCSI_EVENT_BUFS * sizeof(struct virtio_scsi_event);
    libBase->event_pool = IExec->AllocVecTags(libBase->event_pool_size,
                                              AVT_Type, MEMF_SHARED,
                                              AVT_ClearWithValue, 0,
                                              TAG_END);
    if (!libBase->event_pool) {
        DPRINTF(IExec, "[virtioscsi:virtio_events.c] event pool alloc failed\n");
        libBase->events_enabled = FALSE;
        return FALSE;
    }

    /* DMA-map the pool and cache the physical base */
    uint32 entries = IExec->StartDMA(libBase->event_pool, libBase->event_pool_size, DMA_ReadFromRAM);
    if (entries == 0) {
        IExec->FreeVec(libBase->event_pool);
        libBase->event_pool = NULL;
        libBase->events_enabled = FALSE;
        return FALSE;
    }
    struct DMAEntry *dma_list = (struct DMAEntry *)IExec->AllocSysObjectTags(
        ASOT_DMAENTRY, ASODMAE_NumEntries, entries, TAG_DONE);
    if (!dma_list) {
        IExec->EndDMA(libBase->event_pool, libBase->event_pool_size, DMA_ReadFromRAM | DMAF_NoModify);
        IExec->FreeVec(libBase->event_pool);
        libBase->event_pool = NULL;
        libBase->events_enabled = FALSE;
        return FALSE;
    }
    IExec->GetDMAList(libBase->event_pool, libBase->event_pool_size, DMA_ReadFromRAM, dma_list);
    libBase->event_pool_phys = (uint32)dma_list[0].PhysicalAddress;
    libBase->event_pool_dma_list = dma_list;
    libBase->event_pool_dma_entries = entries;

    /* Post all buffers */
    uint32 posted = 0;
    for (uint32 s = 0; s < VIRTIO_SCSI_EVENT_BUFS; s++) {
        if (post_event_buf(libBase, s))
            posted++;
    }

    if (posted == 0) {
        DPRINTF(IExec, "[virtioscsi:virtio_events.c] no event buffers posted — disabling eventq\n");
        ShutdownEventQueue(libBase);
        return FALSE;
    }

    /* Kick the eventq once so the device knows we have buffers available */
    VirtQueue_Kick(IExec, libBase->vqs[1], libBase->pciDevice,
                   libBase->bar0 ? (uint32)libBase->bar0->Physical : 0);

    /* Start the consumer task.  libBase is passed directly as arg1 via
     * AT_Param1 — no Forbid/Permit dance, no tc_UserData shuffling. */
    libBase->event_task_shutdown = FALSE;
    libBase->event_exit_task = NULL;
    libBase->event_exit_mask = 0;
    libBase->event_task = IExec->CreateTaskTags(
        "virtioscsi.event",
        20, /* priority: above default (0), below a1ide IO tasks */
        event_task_entry,
        8192,
        AT_Param1, (uint32)libBase,
        TAG_DONE);

    if (!libBase->event_task) {
        DPRINTF(IExec, "[virtioscsi:virtio_events.c] event task creation failed — eventq disabled\n");
        ShutdownEventQueue(libBase);
        return FALSE;
    }

    DPRINTF(IExec, "[virtioscsi:virtio_events.c] event queue active: %lu buffers posted\n", posted);
    return TRUE;
}

void ShutdownEventQueue(struct VirtIOSCSIBase *libBase)
{
    struct ExecIFace *IExec = libBase->IExec;

    if (libBase->event_task) {
        /* Allocate a signal bit in THIS task's context for the worker to
         * signal on exit.  Can't use event_signal_mask — that's allocated
         * by the worker and only valid in its own context. */
        int8 bit = IExec->AllocSignal(-1);
        uint32 mask = (bit >= 0) ? (1UL << bit) : 0;

        libBase->event_exit_task = IExec->FindTask(NULL);
        libBase->event_exit_mask = mask;
        libBase->event_task_shutdown = TRUE;

        /* Wake the worker from its Wait.  SIGBREAKF_CTRL_C is always
         * deliverable even if AllocSignal failed in the worker. */
        IExec->Signal(libBase->event_task,
                      libBase->event_signal_mask | SIGBREAKF_CTRL_C);

        if (mask) {
            IExec->Wait(mask);
            IExec->FreeSignal(bit);
        } else {
            /* No signal bit available — fall back to yielding until worker
             * clears its pointer.  Rare path; losing this race leaks the
             * event pool, which is acceptable at device teardown. */
            while (libBase->event_task)
                IExec->Wait(SIGBREAKF_CTRL_C);
        }
        libBase->event_exit_task = NULL;
        libBase->event_exit_mask = 0;
    }

    if (libBase->event_pool_dma_list) {
        IExec->EndDMA(libBase->event_pool, libBase->event_pool_size, DMA_ReadFromRAM | DMAF_NoModify);
        IExec->FreeSysObject(ASOT_DMAENTRY, libBase->event_pool_dma_list);
        libBase->event_pool_dma_list = NULL;
    }
    if (libBase->event_pool) {
        IExec->FreeVec(libBase->event_pool);
        libBase->event_pool = NULL;
    }
    libBase->events_enabled = FALSE;
}
