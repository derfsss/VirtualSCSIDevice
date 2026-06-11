#include "virtio/virtio_scsi_io.h"
#include "virtio/virtio_scsi.h"
#include "virtio/virtio_scsi_cmd.h"
#include "virtio/virtqueue.h"
#include "virtioscsi.h"
#include "sandboxvm_tags.h"
#include <exec/exectags.h>
#include <exec/memory.h>

/*
 * map_scsi_error: translate VirtIO response + SCSI status/sense to AmigaOS io_Error.
 *
 * Sense key lives at sense[2] & 0x0F (SPC-4 fixed-format sense, byte 2 bits 3:0).
 * The sense buffer is only valid when virtio_resp == VIRTIO_SCSI_S_OK and
 * scsi_status == 2 (CHECK CONDITION).  For other non-zero statuses we return
 * HFERR_BadStatus directly.
 */
static int32 map_scsi_error(uint8 virtio_resp, uint8 scsi_status, uint8 sense_key)
{
    if (virtio_resp != VIRTIO_SCSI_S_OK)
        return TDERR_NotSpecified;
    if (scsi_status == 0)
        return 0; /* GOOD */
    if (scsi_status != 2)
        return HFERR_BadStatus; /* non-CHECK CONDITION non-GOOD status */

    /* CHECK CONDITION -- decode sense key */
    switch (sense_key & 0x0F) {
    case 0x00: return 0;                  /* NO SENSE -- treat as success */
    case 0x01: return 0;                  /* RECOVERED ERROR -- data valid */
    case 0x02: return TDERR_BadDriveType; /* NOT READY */
    case 0x03: return TDERR_BadSecHdr;    /* MEDIUM ERROR */
    case 0x04: return TDERR_BadDriveType; /* HARDWARE ERROR */
    case 0x05: return IOERR_NOCMD;        /* ILLEGAL REQUEST */
    case 0x06: return TDERR_DiskChanged;  /* UNIT ATTENTION */
    case 0x07: return TDERR_WriteProt;    /* DATA PROTECT */
    case 0x0B: return TDERR_NotSpecified; /* ABORTED COMMAND */
    default:   return HFERR_BadStatus;
    }
}

/*
 * Bounce buffer cache coherency constants.
 *
 * Bounce buffers are normal cacheable MEMF_SHARED memory (DMA mapping
 * released at alloc time after caching the physical address).  Cache
 * coherency is managed explicitly at I/O boundaries:
 *
 *   Write path (Submit): CopyMem user→bounce, then CacheClearE CACRF_ClearD
 *   to flush dirty cache lines to RAM before the device reads at the
 *   physical address in the SG list.
 *
 *   Read path (Harvest): CacheClearE CACRF_InvalidateD to invalidate stale
 *   cache lines, then CopyMem bounce→user to read fresh device-written data.
 */
#ifndef CACRF_ClearD
#define CACRF_ClearD       0x0800  /* Push dirty data cache lines to RAM */
#endif
#ifndef CACRF_InvalidateD
#define CACRF_InvalidateD  0x8000  /* Invalidate data cache lines */
#endif

/* Helper structure to track DMA resources for the user data buffer */
struct DMABuffer
{
    APTR addr;
    uint32 size;
    uint32 flags;
    struct DMAEntry *list;
    uint32 num_entries;
};

static void cleanup_dma(struct ExecIFace *IExec, struct DMABuffer *db)
{
    if (db->num_entries > 0) {
        if (db->list) {
            IExec->FreeSysObject(ASOT_DMAENTRY, db->list);
            db->list = NULL;
        }
        IExec->EndDMA(db->addr, db->size, db->flags);
        db->num_entries = 0;
    }
}

static BOOL prepare_dma(struct ExecIFace *IExec, struct DMABuffer *db, APTR addr, uint32 size, uint32 flags)
{
    db->addr = addr;
    db->size = size;
    db->flags = flags;
    db->list = NULL;
    db->num_entries = 0;

    if (size == 0)
        return TRUE;

    uint32 entries = IExec->StartDMA(addr, size, flags);
    if (entries == 0)
        return FALSE;

    db->num_entries = entries;
    db->list = (struct DMAEntry *)IExec->AllocSysObjectTags(ASOT_DMAENTRY, ASODMAE_NumEntries, entries, TAG_DONE);

    if (!db->list) {
        IExec->EndDMA(addr, size, flags | DMAF_NoModify);
        db->num_entries = 0;
        return FALSE;
    }

    IExec->GetDMAList(addr, size, flags, db->list);
    return TRUE;
}

/*
 * complete_inflight_slot: shared completion path for one pipelined slot.
 *
 * Called from three sites that previously duplicated this sequence:
 *   1. VirtIOSCSI_DoIO inline-harvest of cross-unit cookies during its drain loop
 *   2. VirtIOSCSI_Harvest cross-unit branch (owner != NULL)
 *   3. VirtIOSCSI_Harvest this-unit branch
 *
 * The caller has already identified the owning unit and slot. This helper:
 *   - maps the VirtIO response + SCSI status to io_Error
 *   - computes io_Actual from length and residual
 *   - performs bounce read-back (CacheClearE + CopyMem) for read completions
 *     that took the bounce path
 *   - releases any per-slot StartDMA mapping (direct path only)
 *   - returns the slot to the unit's free list
 *   - decrements libBase->occupied_count and target->inflight_count, and
 *     clears the unit's bit in libBase->active_units_mask when the unit
 *     drains
 *   - ReplyMsg's the ioreq
 *
 * io_lock contract: caller must NOT hold io_lock (ReplyMsg can
 * reschedule).  The helper takes io_lock itself around the free-list
 * and counter updates: a cross-unit completion runs in a DIFFERENT
 * task from the owning unit's Submit, so both the free list and the
 * occupancy counters need the same lock Submit uses.
 */
static void complete_inflight_slot(struct ExecIFace *IExec,
                                   struct VirtIOSCSIBase *libBase,
                                   struct VirtIOUSCSIDevUnit *target,
                                   int32 slot)
{
    struct IOStdReq *ioreq = target->inflight[slot].ioreq;
    struct virtio_scsi_resp_cmd *resp_cmd = target->resp_bufs[slot];
    struct virtqueue *vq = libBase->vqs[2];

    ioreq->io_Error = map_scsi_error(resp_cmd->response, resp_cmd->status,
                                     resp_cmd->sense[2] & 0x0F);
    if (ioreq->io_Error != 0) {
        DPRINTF(IExec,
                "[virtioscsi:virtio_scsi_io.c] complete: T%lu slot=%ld resp=0x%02X scsi=0x%02X err=%ld\n",
                (uint32)target->target_id, (long)slot,
                (uint32)resp_cmd->response, (uint32)resp_cmd->status, (long)ioreq->io_Error);
        ioreq->io_Actual = 0;
    } else {
        /* residual is device-written and little-endian in modern mode;
         * clamp defensively so a bogus value can't yield a huge io_Actual. */
        uint32 residual = virtio_scsi_resp_residual(vq->modern, resp_cmd);
        ioreq->io_Actual = (residual > ioreq->io_Length)
                           ? 0 : ioreq->io_Length - residual;
    }

    /* Bounce read-back: invalidate stale cache lines and copy device data
     * into the user buffer. Only relevant for read completions that took
     * the bounce path. */
    if (target->inflight[slot].using_bounce && !target->inflight[slot].is_write
            && ioreq->io_Error == 0 && ioreq->io_Actual > 0) {
        IExec->CacheClearE(target->bounce_bufs[slot], ioreq->io_Actual, CACRF_InvalidateD);
        IExec->CopyMem(target->bounce_bufs[slot], target->inflight[slot].dma_addr, ioreq->io_Actual);
    }

    /* Release the per-slot user-data StartDMA mapping. dma_list points into
     * data_dma_pool[] so we do NOT FreeSysObject -- just EndDMA. The bounce
     * path leaves dma_list NULL and skips this entirely. */
    if (target->inflight[slot].dma_list) {
        IExec->EndDMA(target->inflight[slot].dma_addr, target->inflight[slot].dma_size,
                      target->inflight[slot].dma_flags);
        target->inflight[slot].dma_list        = NULL;
        target->inflight[slot].dma_num_entries = 0;
    }

    /* Return the slot to the free list and decrement the occupancy
     * counters under io_lock.  Submit increments these counters under
     * io_lock and Harvest's coalescing logic READS occupied_count under
     * io_lock -- but this helper runs after the caller released the lock
     * (ReplyMsg can reschedule), and two unit tasks can complete slots
     * concurrently.  An unlocked read-modify-write here loses updates,
     * leaving occupied_count permanently inflated -- which makes the
     * EVENT_IDX coalescing in Harvest program a used_event for
     * completions that never come (missed interrupts, stalled I/O). */
    IExec->ObtainSemaphore(&libBase->io_lock);
    target->inflight[slot].ioreq  = NULL;
    target->inflight[slot].cookie = NULL;
    target->inflight_next[slot] = target->free_head;
    target->free_head = slot;

    if (libBase->occupied_count > 0)
        libBase->occupied_count--;
    if (target->inflight_count > 0) {
        target->inflight_count--;
        if (target->inflight_count == 0)
            libBase->active_units_mask &= ~(uint8)(1U << target->unit_num);
    }
    IExec->ReleaseSemaphore(&libBase->io_lock);

    IExec->ReplyMsg((struct Message *)ioreq);
}

/*
 * VirtIOSCSI_DoIO: Execute a SCSI command through VirtIO Queue 2 (requestq).
 *
 * When unit != NULL and interrupts are installed, the calling task sleeps
 * via Wait/Signal instead of busy-polling. Falls back to polling otherwise
 * (unit == NULL during discovery, before unit tasks exist).
 *
 * req_buf and resp_buf on unit are pre-allocated MEMF_SHARED buffers with
 * live DMA mappings -- no per-call allocation or DMA setup for these.
 * Only the user data buffer (data/data_len) is DMA-mapped per call.
 *
 * Returns 0 on success, non-zero on failure.
 * On success, scsi_status_out receives the SCSI status byte.
 */
int32 VirtIOSCSI_DoIO(struct VirtIOSCSIBase *libBase, struct VirtIOUSCSIDevUnit *unit,
                      uint32 target, uint32 lun, uint8 *cdb, uint32 cdb_len,
                      uint8 *data, uint32 data_len, BOOL is_write,
                      uint8 *scsi_status_out, uint32 *residual_out)
{
    struct ExecIFace *IExec = libBase->IExec;
    struct PCIDevice *pciDev = libBase->pciDevice;
    struct virtqueue *vq = libBase->vqs[2]; /* requestq */
    uint32 iobase = libBase->bar0 ? (uint32)libBase->bar0->Physical : 0;

    if (!vq || !pciDev) {
        DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: No requestq or PCI device\n");
        return TDERR_NotSpecified;
    }

    /*
     * Discovery path (unit == NULL): allocate temporary req/resp on the heap.
     * This happens only during Init before any unit task exists.
     * The normal I/O path (unit != NULL) uses pre-allocated unit->req_buf /
     * unit->resp_buf with permanent DMA mappings.
     */
    struct virtio_scsi_req_cmd  *req_cmd;
    struct virtio_scsi_resp_cmd *resp_cmd;
    BOOL temp_alloc = FALSE;

    if (unit == NULL) {
        req_cmd = IExec->AllocVecTags(sizeof(struct virtio_scsi_req_cmd),
                                      AVT_ClearWithValue, 0, AVT_Type, MEMF_SHARED,
                                      SBV_AVT_HostDMA, 0, TAG_DONE);
        resp_cmd = IExec->AllocVecTags(sizeof(struct virtio_scsi_resp_cmd),
                                       AVT_ClearWithValue, 0, AVT_Type, MEMF_SHARED,
                                       SBV_AVT_HostDMA, 0, TAG_DONE);
        if (!req_cmd || !resp_cmd) {
            DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: alloc failed (discovery)\n");
            if (req_cmd)  IExec->FreeVec(req_cmd);
            if (resp_cmd) IExec->FreeVec(resp_cmd);
            return TDERR_NoMem;
        }
        temp_alloc = TRUE;
    } else {
        req_cmd  = unit->req_buf;
        resp_cmd = unit->resp_buf;
    }

    int32 tries = 3;
    int32 result = TDERR_NotSpecified;

    /* For the discovery path we need temporary DMA for req/resp too */
    struct DMABuffer db_req_tmp, db_resp_tmp;
    if (temp_alloc) {
        if (!prepare_dma(IExec, &db_req_tmp, req_cmd, sizeof(*req_cmd), DMA_ReadFromRAM)) {
            IExec->FreeVec(req_cmd);
            IExec->FreeVec(resp_cmd);
            return HFERR_DMA;
        }
        if (!prepare_dma(IExec, &db_resp_tmp, resp_cmd, sizeof(*resp_cmd), 0)) {
            cleanup_dma(IExec, &db_req_tmp);
            IExec->FreeVec(req_cmd);
            IExec->FreeVec(resp_cmd);
            return HFERR_DMA;
        }
    }

    while (tries-- > 0) {
        DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: T%lu L%lu Op 0x%02X DataLen %lu (Tries: %ld)\n",
                target, lun, cdb[0], (uint32)data_len, (long)tries + 1);

        /*
         * Reset the three response fields we test before each attempt so
         * stale values from a previous retry don't confuse the checks.
         *
         * We do NOT zero the full 108-byte buffer (27 volatile stores to
         * non-cacheable MEMF_SHARED memory on every request). The device
         * overwrites sense[] before we read it (only read on CHECK CONDITION),
         * and status_qualifier is not used. The first attempt is already clean
         * because the buffer was zeroed at allocation time (AVT_ClearWithValue).
         *
         * Volatile stores required -- MEMF_SHARED is non-cacheable.
         */
        volatile struct virtio_scsi_resp_cmd *vresp =
            (volatile struct virtio_scsi_resp_cmd *)resp_cmd;
        vresp->response         = 0;
        vresp->status           = 0;
        vresp->residual         = 0;

        /* Fill the request header -- overwrite all fields, no prior zero needed */
        virtio_scsi_set_lun(req_cmd->lun, (uint8)target, (uint16)lun);
        req_cmd->id        = 1; /* Simple tag */
        req_cmd->task_attr = VIRTIO_SCSI_S_SIMPLE;
        req_cmd->prio      = 0;
        req_cmd->crn       = 0;

        /* Copy the CDB (up to 32 bytes) */
        uint32 copy_len = cdb_len < VIRTIO_SCSI_CDB_SIZE ? cdb_len : VIRTIO_SCSI_CDB_SIZE;
        uint32 j;
        for (j = 0; j < copy_len; j++)
            req_cmd->cdb[j] = cdb[j];
        /* Zero any remaining CDB bytes from a previous attempt */
        for (j = copy_len; j < VIRTIO_SCSI_CDB_SIZE; j++)
            req_cmd->cdb[j] = 0;

        /* DMA-map the user data buffer (changes every call) */
        struct DMABuffer db_data;
        if (data && data_len > 0) {
            if (!prepare_dma(IExec, &db_data, data, data_len, is_write ? DMA_ReadFromRAM : 0)) {
                DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: StartDMA failed for data\n");
                result = HFERR_DMA;
                break;
            }
        } else {
            db_data.num_entries = 0;
            db_data.list = NULL;
        }

        /* Build the descriptor chain from DMA lists */
        struct vring_sg sg[MAX_SG_ENTRIES];
        uint32 out_num = 0;
        uint32 in_num  = 0;

        /* Select req DMA list -- pre-mapped for unit path, temp for discovery */
        struct DMAEntry *req_dma_list     = temp_alloc ? db_req_tmp.list  : unit->dma_req_list;
        uint32           req_dma_entries  = temp_alloc ? db_req_tmp.num_entries : unit->dma_req_entries;
        struct DMAEntry *resp_dma_list    = temp_alloc ? db_resp_tmp.list : unit->dma_resp_list;
        uint32           resp_dma_entries = temp_alloc ? db_resp_tmp.num_entries : unit->dma_resp_entries;

        /* SG[out..] = req_cmd (OUT) */
        uint32 i;
        for (i = 0; i < req_dma_entries; i++) {
            sg[out_num].addr = (uint32)req_dma_list[i].PhysicalAddress;
            sg[out_num].len  = req_dma_list[i].BlockLength;
            out_num++;
        }

        if (is_write) {
            /* Data segments (OUT) */
            for (i = 0; i < db_data.num_entries; i++) {
                sg[out_num + i].addr = (uint32)db_data.list[i].PhysicalAddress;
                sg[out_num + i].len  = db_data.list[i].BlockLength;
            }
            out_num += db_data.num_entries;
            /* resp_cmd segments (IN) */
            for (i = 0; i < resp_dma_entries; i++) {
                sg[out_num + i].addr = (uint32)resp_dma_list[i].PhysicalAddress;
                sg[out_num + i].len  = resp_dma_list[i].BlockLength;
            }
            in_num = resp_dma_entries;
        } else {
            /* resp_cmd segments (IN) */
            for (i = 0; i < resp_dma_entries; i++) {
                sg[out_num + i].addr = (uint32)resp_dma_list[i].PhysicalAddress;
                sg[out_num + i].len  = resp_dma_list[i].BlockLength;
            }
            in_num = resp_dma_entries;
            /* Data segments (IN) */
            for (i = 0; i < db_data.num_entries; i++) {
                sg[out_num + in_num + i].addr = (uint32)db_data.list[i].PhysicalAddress;
                sg[out_num + in_num + i].len  = db_data.list[i].BlockLength;
            }
            in_num += db_data.num_entries;
        }

        if (out_num + in_num > MAX_SG_ENTRIES) {
            DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: Chain too long (%lu entries)\n",
                    out_num + in_num);
            cleanup_dma(IExec, &db_data);
            result = HFERR_DMA;
            break;
        }

        /* Submit to the available ring -- hold lock only for AddBuf+Kick */
        IExec->ObtainSemaphore(&libBase->io_lock);
        int32 rc = VirtQueue_AddBuf(IExec, vq, sg, out_num, in_num, (void *)req_cmd);
        if (rc != 0) {
            IExec->ReleaseSemaphore(&libBase->io_lock);
            DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: AddBuf failed\n");
            cleanup_dma(IExec, &db_data);
            result = TDERR_NotSpecified;
            break;
        }

        /*
         * DoIO always issues an unconditional QUEUE_NOTIFY rather than going
         * through VirtQueue_Kick / EVENT_IDX suppression.
         *
         * EVENT_IDX suppression is only safe when the device is actively
         * processing a stream of requests and will signal completion via IRQ.
         * DoIO is a synchronous one-shot call: if the kick is suppressed the
         * device never sees the request, the ISR never fires, and Wait()
         * blocks forever. The extra unconditional PCI write is negligible for
         * these rare synchronous commands (geometry, SCSI passthrough).
         */
        /* Mark unit active so ISR knows to signal it */
        if (unit)
            libBase->active_units_mask |= (uint8)(1 << unit->unit_num);

        VirtQueue_Kick(IExec, vq, pciDev, iobase);
        IExec->ReleaseSemaphore(&libBase->io_lock);

        /* Wait for completion -- interrupt-driven or polling fallback */
        uint32 written = 0;
        void *cookie = NULL;

        if (libBase->irq_installed && unit != NULL) {
            /*
             * Interrupt path: reuse the unit task's persistent signal bit.
             *
             * DoIO() runs inside UnitTask_Dispatch(), which is called from the
             * unit task's event loop.  The same signal bit is used by both DoIO
             * (to wait for a synchronous completion) and Harvest (to drain
             * pipeline completions).
             *
             * Race: while DoIO is waiting, a pipeline CMD_READ on the *other*
             * unit may complete and GetBuf may return that slot's cookie instead
             * of req_cmd.  If DoIO ignores it, the pipeline completion is lost
             * from the ring and that request hangs forever.
             *
             * Fix: drain GetBuf in a loop.  For each cookie that does NOT match
             * req_cmd, call Harvest() to process it immediately.  Continue until
             * we get our own cookie back.
             *
             * Protocol:
             *   1. Restore io_cookie to sentinel (void*)1 before waiting -- the
             *      ISR only checks io_wait_task != NULL, which stays set.
             *   2. Call GetBuf in a loop; forward non-matching cookies to Harvest.
             *   3. Stop when we get req_cmd back (or time out).
             *   4. After our cookie arrives, clear any extra signal.
             */
            uint32 sig_mask = unit->io_signal_mask;
            DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: IRQ path sig=0x%08lX req=%p avail_idx=%u used_idx=%u\n",
                    sig_mask, (void *)req_cmd,
                    (unsigned)vq->avail->idx, (unsigned)vq->used->idx);

            /* Keep sentinel -- ISR stays live for pipeline completions */
            unit->io_cookie = (void *)1;
            __asm__ volatile("sync" ::: "memory");

            /* Drain loop: harvest any pipeline completions that arrive before ours.
             * io_lock serialises GetBuf with the other unit task's Harvest. */
            uint32 retries = 50;
            uint32 wakes   = 0;
            cookie = NULL;
            IExec->ObtainSemaphore(&libBase->io_lock);
            while (retries > 0) {
                /*
                 * Check the pending-cookie slot first.  If another unit's Harvest
                 * ran before this drain loop and dequeued our cookie from the ring,
                 * it will have stashed it in unit->doio_pending_cookie (under
                 * io_lock).  Pick it up here to avoid a 50-retry timeout.
                 */
                if (unit->doio_pending_cookie == (void *)req_cmd) {
                    cookie  = unit->doio_pending_cookie;
                    written = unit->doio_pending_written;
                    unit->doio_pending_cookie  = NULL;
                    unit->doio_pending_written = 0;
                    DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: picked up stashed cookie %p written=%lu\n",
                            cookie, written);
                    break;
                }
                void *c;
                uint32 w;
                while ((c = VirtQueue_GetBuf(IExec, vq, &w)) != NULL) {
                    if (c == (void *)req_cmd) {
                        cookie  = c;
                        written = w;
                        DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: got own cookie %p written=%lu wakes=%lu\n",
                                cookie, written, (uint32)wakes);
                        break; /* stop draining -- outer loop will exit on cookie != NULL */
                    } else {
                        /* Completion belongs to another unit's pipeline slot --
                         * inline-harvest it. Search all units (not just this one).
                         * Release lock around ReplyMsg, re-acquire after. */
                        IExec->ReleaseSemaphore(&libBase->io_lock);
                        DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: forwarding pipeline cookie %p\n", c);
                        struct VirtIOUSCSIDevUnit *owner = NULL;
                        int32 fslot = -1;
                        /* O(1) cross-unit decode via encoded req_cmd->id */
                        {
                            struct virtio_scsi_req_cmd *xrc = (struct virtio_scsi_req_cmd *)c;
                            uint32 cu = (uint32)(xrc->id >> 16);
                            uint32 cs = (uint32)(xrc->id & 0xFFFF);
                            if (cs < MAX_INFLIGHT && cu < MAX_UNITS) {
                                struct VirtIOUSCSIDevUnit *xtgt = libBase->units[cu];
                                if (xtgt
                                    && xtgt->inflight[cs].cookie == c
                                    && xtgt->inflight[cs].ioreq != NULL) {
                                    owner = xtgt;
                                    fslot = (int32)cs;
                                }
                            }
                        }
                        if (owner != NULL) {
                            DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: inline-harvest T%lu slot %ld\n",
                                    (uint32)owner->target_id, (long)fslot);
                            complete_inflight_slot(IExec, libBase, owner, fslot);
                        } else {
                            /*
                             * Not a pipeline cookie -- may be another unit's DoIO cookie.
                             * Stash it so that unit's drain loop can pick it up.
                             */
                            IExec->ObtainSemaphore(&libBase->io_lock);
                            struct VirtIOUSCSIDevUnit *doio_other = NULL;
                            uint32 di;
                            for (di = 0; di < MAX_UNITS; di++) {
                                struct VirtIOUSCSIDevUnit *other2 = libBase->units[di];
                                if (other2 && (void *)other2->req_buf == c) {
                                    doio_other = other2;
                                    break;
                                }
                            }
                            if (doio_other != NULL) {
                                doio_other->doio_pending_cookie  = c;
                                doio_other->doio_pending_written = w;
                                if (doio_other->io_wait_task)
                                    IExec->Signal(doio_other->io_wait_task, doio_other->io_signal_mask);
                            }
                            IExec->ReleaseSemaphore(&libBase->io_lock);
                            if (doio_other != NULL) {
                                DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: inline stash DoIO cookie %p for T%lu\n",
                                        c, (uint32)doio_other->target_id);
                            } else {
                                DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: unmatched pipeline cookie %p (truly unknown)\n", c);
                            }
                        }
                        IExec->ObtainSemaphore(&libBase->io_lock);
                    }
                }
                if (cookie)
                    break; /* got our own completion */
                IExec->ReleaseSemaphore(&libBase->io_lock);
                IExec->Wait(sig_mask);
                wakes++;
                retries--;
                IExec->ObtainSemaphore(&libBase->io_lock);
            }
            IExec->ReleaseSemaphore(&libBase->io_lock);

            if (!cookie)
                DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: TIMEOUT after %lu wakes\n", (uint32)wakes);

            IExec->SetSignal(0, sig_mask); /* clear any extra signals that arrived */
        } else {
            /* Polling fallback (discovery: unit == NULL, or no IRQ installed) */
            DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: poll path req=%p avail_idx=%u used_idx=%u\n",
                    (void *)req_cmd, (unsigned)vq->avail->idx, (unsigned)vq->used->idx);
            uint32 poll_timeout = 500000;
            while (poll_timeout-- > 0) {
                cookie = VirtQueue_GetBuf(IExec, vq, &written);
                if (cookie)
                    break;
            }
            DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: poll done cookie=%p written=%lu remaining=%lu\n",
                    cookie, written, poll_timeout);
        }

        /* Release the data DMA mapping for this attempt.
         *
         * IMPORTANT on timeout (cookie == NULL): the request descriptor is
         * STILL in the VirtIO ring's avail/desc tables. The device may yet
         * complete it (interrupt was dropped / device was slow). Releasing
         * the data DMA mapping here means a late completion would write
         * into an unmapped buffer -- but for unit-path callers the data
         * buffer is in user space (StartDMA on user io_Data) so 'leaking'
         * the mapping is also wrong. Compromise: release for the non-
         * timeout case; on timeout, only release for unit-path callers
         * (their req_cmd lives in the unit struct and the descriptor still
         * points at LIVE memory, so late completion is harmless to the
         * cookie; the user data buffer is the unit's responsibility).
         * For the temp_alloc path see the discovery-timeout block below. */
        if (cookie || !temp_alloc) {
            cleanup_dma(IExec, &db_data);
        }

        if (!cookie) {
            DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: TIMEOUT\n");
            result = IOERR_SELFTEST;
            break;
        }

        /* Check VirtIO response */
        if (resp_cmd->response != VIRTIO_SCSI_S_OK) {
            DPRINTF(IExec, "[virtioscsi] DoIO: VirtIO error response=0x%02X\n", resp_cmd->response);
            if (scsi_status_out)
                *scsi_status_out = 0;
            result = TDERR_NotSpecified;
        } else {
            if (scsi_status_out)
                *scsi_status_out = resp_cmd->status;
            if (residual_out) {
                /* Clamp: callers compute io_Actual = data_len - residual,
                 * so a bogus device value must never exceed data_len. */
                uint32 residual = virtio_scsi_resp_residual(vq->modern, resp_cmd);
                *residual_out = (residual > data_len) ? data_len : residual;
            }

            if (resp_cmd->status == 2) { /* CHECK CONDITION */
                DPRINTF(IExec, "[virtioscsi] DoIO: T%lu L%lu CHECK CONDITION (Tries left: %ld)\n",
                        target, lun, (long)tries);
                if (tries > 0)
                    continue; /* retry -- resp re-zeroed at top of loop */
                result = map_scsi_error(resp_cmd->response, resp_cmd->status,
                                        resp_cmd->sense[2] & 0x0F);
            } else if (resp_cmd->status == 0) {
                result = 0;
            } else {
                result = map_scsi_error(resp_cmd->response, resp_cmd->status, 0xFF);
            }
        }

        break;
    }

    /* Discovery path cleanup: free temporary DMA mappings and buffers.
     *
     * BUT on a timeout (result == IOERR_SELFTEST and we never picked up our
     * cookie), the descriptor we placed in the VirtIO ring still points at
     * req_cmd / resp_cmd. If the device later completes (e.g. it was just
     * slow), it would write into freed memory and Harvest would receive a
     * dangling cookie -- a use-after-free.
     *
     * Discovery DoIO timing out is pathological: the device failed to ack
     * an INQUIRY / similar within 50 ISR wakes. The driver is essentially
     * unusable past this point. Intentionally LEAK these buffers and the
     * data DMA mapping so a late completion writes into still-mapped
     * memory rather than freed memory. The leak is bounded (one set per
     * discovery probe), and freeing the device would require a full
     * device reset to clear the descriptor first, which is too invasive
     * to do here. The driver will fail Init() and the OS will discard us.
     */
    /* result == IOERR_SELFTEST is the unique timeout signal set above on
     * the !cookie path; any other result means the device responded (with
     * success, SCSI error, or VirtIO error) and the descriptor is no
     * longer referenced by hardware -- safe to free. */
    if (temp_alloc && result != IOERR_SELFTEST) {
        cleanup_dma(IExec, &db_resp_tmp);
        cleanup_dma(IExec, &db_req_tmp);
        IExec->FreeVec(resp_cmd);
        IExec->FreeVec(req_cmd);
    } else if (temp_alloc) {
        DPRINTF(IExec,
                "[virtioscsi:virtio_scsi_io.c] DoIO: discovery TIMEOUT - intentionally leaking "
                "req_cmd %p / resp_cmd %p / data DMA to avoid UAF if device completes late\n",
                (void *)req_cmd, (void *)resp_cmd);
    }

    /* Clear active-units mask bit if this unit has no pipeline inflight work.
     * DoIO does not use inflight_count (it uses req_buf[0] directly), so
     * only clear the bit when the pipeline is also idle. */
    if (unit && unit->inflight_count == 0) {
        IExec->ObtainSemaphore(&libBase->io_lock);
        if (unit->inflight_count == 0) /* re-check under lock */
            libBase->active_units_mask &= ~(uint8)(1 << unit->unit_num);
        IExec->ReleaseSemaphore(&libBase->io_lock);
    }

    DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: Done (T%lu L%lu, result %ld)\n",
            target, lun, (long)result);

    return result;
}

/* Push a slot back onto the unit's free list under io_lock.  Used by
 * VirtIOSCSI_Submit's failure paths; the lock matches the pop at Submit
 * entry and the push in complete_inflight_slot. */
static void submit_release_slot(struct ExecIFace *IExec,
                                struct VirtIOSCSIBase *libBase,
                                struct VirtIOUSCSIDevUnit *unit, int32 slot)
{
    IExec->ObtainSemaphore(&libBase->io_lock);
    unit->inflight_next[slot] = unit->free_head;
    unit->free_head = slot;
    IExec->ReleaseSemaphore(&libBase->io_lock);
}

/*
 * VirtIOSCSI_Submit: submit one block I/O request into a free inflight slot.
 *
 * Finds a free inflight slot, DMA-maps the user data buffer, fills the
 * req_buf for that slot, builds the SG chain, calls AddBuf+Kick, and
 * returns immediately.
 *
 * Returns:
 *   0   -- slot acquired, request submitted; Harvest will ReplyMsg when done
 *  -1   -- no free slot; caller should queue the request for later retry
 *  >0   -- hard failure (DMA or AddBuf); caller should set io_Error and ReplyMsg
 */
int32 VirtIOSCSI_Submit(struct VirtIOSCSIBase *libBase, struct VirtIOUSCSIDevUnit *unit,
                        struct IOStdReq *ioreq, uint8 *cdb, uint32 cdb_len, BOOL is_write)
{
    struct ExecIFace *IExec = libBase->IExec;
    struct virtqueue *vq = libBase->vqs[2];

    /* Find a free inflight slot -- O(1) via free list.  io_lock guards
     * the pop: complete_inflight_slot can push a slot back onto THIS
     * unit's free list from another unit's task (cross-unit inline
     * harvest), so the list is not single-task-owned. */
    uint32 i;
    IExec->ObtainSemaphore(&libBase->io_lock);
    int32 slot = unit->free_head;
    if (slot >= 0)
        unit->free_head = unit->inflight_next[slot];
    IExec->ReleaseSemaphore(&libBase->io_lock);
    if (slot < 0)
        return -1; /* No free slot */

    struct virtio_scsi_req_cmd  *req_cmd  = unit->req_bufs[slot];
    struct virtio_scsi_resp_cmd *resp_cmd = unit->resp_bufs[slot];

    /* Reset response fields so stale data from a previous request is gone */
    volatile struct virtio_scsi_resp_cmd *vresp = (volatile struct virtio_scsi_resp_cmd *)resp_cmd;
    vresp->response = 0;
    vresp->status   = 0;
    vresp->residual = 0;

    /* Fill the request header */
    virtio_scsi_set_lun(req_cmd->lun, (uint8)unit->target_id, (uint16)unit->lun_id);
    req_cmd->id        = ((uint64)unit->unit_num << 16) | (uint64)slot;
    req_cmd->task_attr = VIRTIO_SCSI_S_SIMPLE;
    req_cmd->prio      = 0;
    req_cmd->crn       = 0;

    uint32 copy_len = cdb_len < VIRTIO_SCSI_CDB_SIZE ? cdb_len : VIRTIO_SCSI_CDB_SIZE;
    for (i = 0; i < copy_len; i++)
        req_cmd->cdb[i] = cdb[i];
    for (i = copy_len; i < VIRTIO_SCSI_CDB_SIZE; i++)
        req_cmd->cdb[i] = 0;

    /* Map the user data buffer for DMA.
     *
     * Bounce path (data_len <= BOUNCE_BUF_SIZE):
     *   Copy write data into the pre-pinned bounce buffer and use its
     *   pre-computed physical address directly.  Zero per-call DMA overhead.
     *   Read data is copied back from the bounce buffer in Harvest.
     *
     * Direct path (data_len > BOUNCE_BUF_SIZE):
     *   StartDMA/GetDMAList/EndDMA as before -- one-time cost for large I/O.
     */
    APTR  data      = ioreq->io_Data;
    uint32 data_len = ioreq->io_Length;
    uint32 dma_flags = is_write ? DMA_ReadFromRAM : 0;

    unit->inflight[slot].is_write      = is_write;
    unit->inflight[slot].using_bounce  = FALSE;

    if (data && data_len > 0 && data_len <= BOUNCE_BUF_SIZE) {
        /* Bounce path -- no StartDMA needed */
        if (is_write) {
            IExec->CopyMem(data, unit->bounce_bufs[slot], data_len);
            IExec->CacheClearE(unit->bounce_bufs[slot], data_len, CACRF_ClearD);
        }
        unit->inflight[slot].using_bounce  = TRUE;
        unit->inflight[slot].dma_addr      = data;   /* user buf -- for read-back in Harvest */
        unit->inflight[slot].dma_size      = data_len;
        unit->inflight[slot].dma_flags     = 0;
        unit->inflight[slot].dma_list      = NULL;
        unit->inflight[slot].dma_num_entries = 0;
        DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] Submit: bounce slot=%ld len=%lu %s\n",
                (long)slot, data_len, is_write ? "W" : "R");
    } else if (data && data_len > 0) {
        /* Direct DMA path for large transfers (>BOUNCE_BUF_SIZE).
         * Uses pre-allocated DMA entry array from data_dma_pool to avoid
         * per-request AllocSysObjectTags/FreeSysObject overhead. */
        uint32 entries = IExec->StartDMA(data, data_len, dma_flags);
        if (entries == 0) {
            DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] Submit: StartDMA failed slot=%ld\n", (long)slot);
            submit_release_slot(IExec, libBase, unit, slot);
            return HFERR_DMA;
        }
        struct DMAEntry *dlist = unit->data_dma_pool[slot];
        if (!dlist || entries > MAX_SG_ENTRIES) {
            /* Pool entry missing or chain longer than the pre-allocated array */
            IExec->EndDMA(data, data_len, dma_flags | DMAF_NoModify);
            submit_release_slot(IExec, libBase, unit, slot);
            return HFERR_DMA;
        }
        IExec->GetDMAList(data, data_len, dma_flags, dlist);
        unit->inflight[slot].dma_addr        = data;
        unit->inflight[slot].dma_size        = data_len;
        unit->inflight[slot].dma_flags       = dma_flags;
        unit->inflight[slot].dma_list        = dlist;
        unit->inflight[slot].dma_num_entries = entries;
    } else {
        unit->inflight[slot].dma_addr        = NULL;
        unit->inflight[slot].dma_size        = 0;
        unit->inflight[slot].dma_flags       = 0;
        unit->inflight[slot].dma_list        = NULL;
        unit->inflight[slot].dma_num_entries = 0;
    }

    /* Build SG chain */
    struct vring_sg sg[MAX_SG_ENTRIES];
    uint32 out_num = 0;
    uint32 in_num  = 0;

    struct DMAEntry *req_dma  = unit->dma_req_lists[slot];
    uint32  req_ent           = unit->dma_req_entries_arr[slot];
    struct DMAEntry *resp_dma = unit->dma_resp_lists[slot];
    uint32  resp_ent          = unit->dma_resp_entries_arr[slot];

    for (i = 0; i < req_ent; i++) {
        sg[out_num].addr = (uint32)req_dma[i].PhysicalAddress;
        sg[out_num].len  = req_dma[i].BlockLength;
        out_num++;
    }

    if (unit->inflight[slot].using_bounce) {
        /* Bounce path: single pre-mapped physical address, full data_len length */
        uint32 bphys = unit->bounce_dma_phys[slot];
        uint32 blen  = unit->inflight[slot].dma_size;
        if (is_write) {
            sg[out_num].addr = bphys;
            sg[out_num].len  = blen;
            out_num++;
            for (i = 0; i < resp_ent; i++) {
                sg[out_num + i].addr = (uint32)resp_dma[i].PhysicalAddress;
                sg[out_num + i].len  = resp_dma[i].BlockLength;
            }
            in_num = resp_ent;
        } else {
            for (i = 0; i < resp_ent; i++) {
                sg[out_num + i].addr = (uint32)resp_dma[i].PhysicalAddress;
                sg[out_num + i].len  = resp_dma[i].BlockLength;
            }
            in_num = resp_ent;
            sg[out_num + in_num].addr = bphys;
            sg[out_num + in_num].len  = blen;
            in_num++;
        }
    } else if (is_write) {
        struct DMAEntry *dl = unit->inflight[slot].dma_list;
        uint32 dl_ent = unit->inflight[slot].dma_num_entries;
        for (i = 0; i < dl_ent; i++) {
            sg[out_num].addr = (uint32)dl[i].PhysicalAddress;
            sg[out_num].len  = dl[i].BlockLength;
            out_num++;
        }
        for (i = 0; i < resp_ent; i++) {
            sg[out_num + i].addr = (uint32)resp_dma[i].PhysicalAddress;
            sg[out_num + i].len  = resp_dma[i].BlockLength;
        }
        in_num = resp_ent;
    } else {
        for (i = 0; i < resp_ent; i++) {
            sg[out_num + i].addr = (uint32)resp_dma[i].PhysicalAddress;
            sg[out_num + i].len  = resp_dma[i].BlockLength;
        }
        in_num = resp_ent;
        struct DMAEntry *dl = unit->inflight[slot].dma_list;
        uint32 dl_ent = unit->inflight[slot].dma_num_entries;
        for (i = 0; i < dl_ent; i++) {
            sg[out_num + in_num + i].addr = (uint32)dl[i].PhysicalAddress;
            sg[out_num + in_num + i].len  = dl[i].BlockLength;
        }
        in_num += dl_ent;
    }

    if (out_num + in_num > MAX_SG_ENTRIES) {
        DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] Submit: chain too long (%lu) slot=%ld\n",
                out_num + in_num, (long)slot);
        if (unit->inflight[slot].dma_list) {
            /* dma_list points into data_dma_pool -- do NOT FreeSysObject, just EndDMA */
            IExec->EndDMA(unit->inflight[slot].dma_addr, unit->inflight[slot].dma_size,
                          unit->inflight[slot].dma_flags | DMAF_NoModify);
            unit->inflight[slot].dma_list        = NULL;
            unit->inflight[slot].dma_num_entries = 0;
        }
        submit_release_slot(IExec, libBase, unit, slot);
        return HFERR_DMA;
    }

    /* Mark slot occupied before AddBuf so Harvest can't race */
    unit->inflight[slot].ioreq    = ioreq;
    unit->inflight[slot].cookie   = (void *)req_cmd;
    unit->inflight[slot].buf_slot = (uint32)slot;

    IExec->ObtainSemaphore(&libBase->io_lock);
    int32 rc = VirtQueue_AddBuf(IExec, vq, sg, out_num, in_num, (void *)req_cmd);
    if (rc != 0) {
        IExec->ReleaseSemaphore(&libBase->io_lock);
        DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] Submit: AddBuf failed slot=%ld\n", (long)slot);
        /* Roll back slot */
        unit->inflight[slot].ioreq  = NULL;
        unit->inflight[slot].cookie = NULL;
        if (unit->inflight[slot].dma_list) {
            IExec->EndDMA(unit->inflight[slot].dma_addr, unit->inflight[slot].dma_size,
                          unit->inflight[slot].dma_flags | DMAF_NoModify);
            unit->inflight[slot].dma_list        = NULL;
            unit->inflight[slot].dma_num_entries = 0;
        }
        submit_release_slot(IExec, libBase, unit, slot);
        return TDERR_NotSpecified;
    }
    /* Slot successfully submitted -- update occupancy tracking */
    libBase->occupied_count++;
    unit->inflight_count++;
    libBase->active_units_mask |= (uint8)(1 << unit->unit_num);
    IExec->ReleaseSemaphore(&libBase->io_lock);

    /* Kick deferred to caller -- one VirtIOSCSI_Kick() covers the whole batch */
    DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] Submit: slot=%ld queued avail_idx=%u cmd=0x%02X len=%lu\n",
            (long)slot, (unsigned)vq->avail->idx, (uint32)cdb[0], data_len);
    return 0;
}

/*
 * VirtIOSCSI_Kick: notify the device that new requests are in the available ring.
 *
 * Called by the unit task dispatch loop after draining all pending IORequests
 * from the message port.  Batching multiple Submit() calls before a single
 * Kick() eliminates N-1 redundant PCI I/O writes for burst I/O workloads.
 *
 * The sync barrier ensures avail->idx is visible to the device before the
 * QUEUE_NOTIFY write.  io_lock is held only for the barrier+PCI write, not
 * for the full Submit duration.
 */
void VirtIOSCSI_Kick(struct VirtIOSCSIBase *libBase)
{
    struct ExecIFace *IExec = libBase->IExec;
    struct PCIDevice *pciDev = libBase->pciDevice;
    struct virtqueue *vq = libBase->vqs[2];
    uint32 iobase = libBase->bar0 ? (uint32)libBase->bar0->Physical : 0;

    if (!vq || !pciDev)
        return;

    IExec->ObtainSemaphore(&libBase->io_lock);
    VirtQueue_Kick(IExec, vq, pciDev, iobase);
    IExec->ReleaseSemaphore(&libBase->io_lock);
}

/*
 * VirtIOSCSI_Harvest: drain completed VirtIO responses from the used ring.
 *
 * Called by the unit task after waking from the ISR signal. Loops
 * VirtQueue_GetBuf() until no more completions are available. For each,
 * matches the cookie to an inflight slot, fills io_Error/io_Actual,
 * cleans up DMA, clears the slot, and calls ReplyMsg.
 */
void VirtIOSCSI_Harvest(struct VirtIOSCSIBase *libBase, struct VirtIOUSCSIDevUnit *unit)
{
    struct ExecIFace *IExec = libBase->IExec;
    struct virtqueue *vq = libBase->vqs[2];
    uint32 written;
    void *cookie;

    DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] Harvest: entry used_idx=%u last=%u\n",
            (unsigned)vq->used->idx, (unsigned)vq->last_used_idx);

    /*
     * io_lock serialises GetBuf across all unit tasks that share VQ2.
     * Without it, two unit tasks woken by the same ISR both call GetBuf
     * concurrently, increment last_used_idx in lockstep, and each sees
     * the other's cookie as "unmatched" -- losing completions.
     *
     * Protocol: hold lock for GetBuf only, release before ReplyMsg
     * (which can reschedule), then re-acquire for the next iteration.
     */
    /*
     * Drain loop -- lock protocol:
     *   ObtainSemaphore before GetBuf, ReleaseSemaphore immediately after.
     *   This ensures only one unit task advances last_used_idx at a time.
     *   ReplyMsg is called without the lock (it can reschedule).
     *   Re-acquire at the bottom of every loop path before the next GetBuf.
     */
    IExec->ObtainSemaphore(&libBase->io_lock);
    while ((cookie = VirtQueue_GetBuf(IExec, vq, &written)) != NULL) {
        IExec->ReleaseSemaphore(&libBase->io_lock);

        DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] Harvest: cookie=%p written=%lu\n", cookie, written);

        /*
         * O(1) cookie routing: req_cmd->id encodes (unit_num << 16 | slot).
         * Decode to find owning unit and slot in constant time.
         * DoIO cookies (id=1) fall through to the stash path.
         */
        int32 slot = -1;
        struct VirtIOUSCSIDevUnit *owner = NULL;
        {
            struct virtio_scsi_req_cmd *rc = (struct virtio_scsi_req_cmd *)cookie;
            uint32 candidate_unit = (uint32)(rc->id >> 16);
            uint32 candidate_slot = (uint32)(rc->id & 0xFFFF);

            if (candidate_slot < MAX_INFLIGHT && candidate_unit < MAX_UNITS) {
                struct VirtIOUSCSIDevUnit *target = (candidate_unit == unit->unit_num)
                    ? unit : libBase->units[candidate_unit];
                if (target
                    && target->inflight[candidate_slot].cookie == cookie
                    && target->inflight[candidate_slot].ioreq != NULL) {
                    slot = (int32)candidate_slot;
                    if (target != unit)
                        owner = target;
                }
            }
        }

        if (slot < 0) {
            /*
             * Cookie matches no inflight pipeline slot.  This happens when
             * a concurrent VirtIOSCSI_DoIO() call submitted via req_buf[0]
             * (id=1) and Harvest drained it before DoIO's own GetBuf loop.
             * Stash the cookie so DoIO can pick it up.
             */
            uint32 u;
            struct VirtIOUSCSIDevUnit *doio_owner = NULL;
            for (u = 0; u < MAX_UNITS; u++) {
                struct VirtIOUSCSIDevUnit *other = libBase->units[u];
                if (other && (void *)other->req_buf == cookie) {
                    doio_owner = other;
                    break;
                }
            }
            if (doio_owner != NULL) {
                DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] Harvest: stashing DoIO cookie %p for unit T%lu\n",
                        cookie, (uint32)doio_owner->target_id);
                doio_owner->doio_pending_cookie  = cookie;
                doio_owner->doio_pending_written = written;
                if (doio_owner->io_wait_task)
                    IExec->Signal(doio_owner->io_wait_task, doio_owner->io_signal_mask);
            } else {
                DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] Harvest: unmatched cookie %p (truly unknown)\n", cookie);
            }
            IExec->ObtainSemaphore(&libBase->io_lock);
            continue;
        }

        {
            struct VirtIOUSCSIDevUnit *target = owner ? owner : unit;
            DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] Harvest: %scookie %p -> T%lu slot %ld\n",
                    owner ? "cross-unit " : "", cookie, (uint32)target->target_id, (long)slot);
            complete_inflight_slot(IExec, libBase, target, slot);
        }

        IExec->ObtainSemaphore(&libBase->io_lock); /* re-acquire for next GetBuf */
    }

    /*
     * Interrupt coalescing: when multiple requests remain in-flight after
     * harvesting, tell the device to wait for all of them before raising the
     * next interrupt -- one ISR per burst instead of one per completion.
     *
     * EVENT_IDX formula (VirtIO spec §2.6.7):
     *   Device interrupts when: (new_used - used_event - 1) < (new_used - old_used)
     *
     *   used_event = last_used_idx + (N - 1) causes the Nth completion to
     *   trigger the interrupt:
     *   - N=1 (idle or 1 in-flight): used_event = last_used_idx → fires on +1
     *   - N=2 (2 in-flight): used_event = last_used_idx + 1 → fires on +2
     *   - N=8 (full pipeline): used_event = last_used_idx + 7 → fires on +8
     *
     * VirtQueue_GetBuf() already wrote used_event = last_used_idx (N=1) after
     * each drain call.  Only override it when occupied >= 2; for 0 or 1 the
     * GetBuf baseline is already correct.
     *
     * This call holds io_lock (outer loop just exited with it held).
     * used_event lives at vq->avail->ring[vq->num] (uint16 past ring[]).
     * eieio barrier ensures the store is visible before we release the lock.
     *
     * Only written when EVENT_IDX was negotiated.
     */
    if (vq->use_event_idx) {
        uint16 occupied = (uint16)libBase->occupied_count;
        if (occupied >= 2) {
            /* Coalesce: fire after all occupied completions arrive.
             * used_event is a vring field -- little-endian in modern mode
             * (vr16), exactly like the baseline write in VirtQueue_GetBuf.
             * Writing it raw here byte-swapped the threshold on the modern
             * path, making the device suppress interrupts it should have
             * delivered. */
            uint16 next_event = vq->last_used_idx + (uint16)(occupied - 1);
            vq->avail->ring[vq->num] = vr16(vq->modern, next_event);
            __asm__ volatile("eieio" ::: "memory");
            DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] Harvest: used_event=%u (occupied=%u, coalescing)\n",
                    (unsigned)next_event, (unsigned)occupied);
        }
        /* occupied <= 1: GetBuf already wrote last_used_idx (fire on next +1) */
    }

    IExec->ReleaseSemaphore(&libBase->io_lock);
}
