#include "virtio/virtio_scsi_io.h"
#include "virtio/virtio_scsi.h"
#include "virtio/virtio_scsi_cmd.h"
#include "virtio/virtqueue.h"
#include "virtioscsi.h"
#include <exec/exectags.h>
#include <exec/memory.h>

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
 * Zero a MEMF_SHARED (non-cacheable) buffer using a volatile byte loop.
 *
 * ClearMem/SetMem must NOT be used on non-cacheable memory — they use cache
 * manipulation instructions and fall back to a slow exception handler path
 * when the memory is cache-inhibited. A volatile loop bypasses the cache
 * entirely and is safe for DMA-mapped (MEMF_SHARED) regions.
 */
/*
 * GCC replaces simple fill loops with memset() even through 'volatile',
 * and memset lives in newlib which is not linked (-nostartfiles, no INewlib).
 * Using uint32 stores with noinline prevents that substitution while still
 * emitting one store per word. size must be a multiple of 4.
 */
static void __attribute__((noinline)) zero_shared(void *buf, uint32 size)
{
    volatile uint32 *p = (volatile uint32 *)buf;
    uint32 words = size / 4;
    uint32 i;
    for (i = 0; i < words; i++)
        p[i] = 0;
}

/*
 * VirtIOSCSI_DoIO: Execute a SCSI command through VirtIO Queue 2 (requestq).
 *
 * When unit != NULL and interrupts are installed, the calling task sleeps
 * via Wait/Signal instead of busy-polling. Falls back to polling otherwise
 * (unit == NULL during discovery, before unit tasks exist).
 *
 * req_buf and resp_buf on unit are pre-allocated MEMF_SHARED buffers with
 * live DMA mappings — no per-call allocation or DMA setup for these.
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
    uint32 iobase = (uint32)libBase->bar0->Physical;

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
                                      AVT_ClearWithValue, 0, AVT_Type, MEMF_SHARED, TAG_DONE);
        resp_cmd = IExec->AllocVecTags(sizeof(struct virtio_scsi_resp_cmd),
                                       AVT_ClearWithValue, 0, AVT_Type, MEMF_SHARED, TAG_DONE);
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
         * Zero resp_cmd before each attempt so stale data from a previous
         * try doesn't confuse the response check.
         *
         * For temp_alloc (discovery), resp was zeroed at alloc time for the
         * first try; on retry it needs re-zeroing too.
         *
         * Use volatile loop — ClearMem is unsafe on non-cacheable MEMF_SHARED.
         */
        zero_shared(resp_cmd, sizeof(struct virtio_scsi_resp_cmd));

        /* Fill the request header — overwrite all fields, no prior zero needed */
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

        /* Select req DMA list — pre-mapped for unit path, temp for discovery */
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

        /* Submit to the available ring — hold lock only for AddBuf+Kick */
        IExec->ObtainSemaphore(&libBase->io_lock);
        int32 rc = VirtQueue_AddBuf(vq, sg, out_num, in_num, (void *)req_cmd);
        if (rc != 0) {
            IExec->ReleaseSemaphore(&libBase->io_lock);
            DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: AddBuf failed\n");
            cleanup_dma(IExec, &db_data);
            result = TDERR_NotSpecified;
            break;
        }

        VirtQueue_Kick(vq, pciDev, iobase);
        IExec->ReleaseSemaphore(&libBase->io_lock);

        /* Wait for completion — interrupt-driven or polling fallback */
        uint32 written = 0;
        void *cookie = NULL;

        if (libBase->irq_installed && unit != NULL) {
            /*
             * Interrupt path: register this unit as the one expecting a signal,
             * then sleep. The ISR will call Signal(unit->io_wait_task).
             * Per-unit fields mean multiple units can have in-flight requests.
             */
            int8 sig_bit = IExec->AllocSignal(-1);
            if (sig_bit >= 0) {
                uint32 sig_mask = 1UL << sig_bit;

                unit->io_cookie      = (void *)req_cmd;
                unit->io_signal_mask = sig_mask;
                unit->io_wait_task   = IExec->FindTask(NULL);

                /* Ensure signal fields are visible before checking used ring */
                __asm__ volatile("sync" ::: "memory");

                /* Fast path: already completed before we registered */
                cookie = VirtQueue_GetBuf(vq, &written);

                if (!cookie) {
                    uint32 retries = 50;
                    while (retries-- > 0) {
                        IExec->Wait(sig_mask);
                        cookie = VirtQueue_GetBuf(vq, &written);
                        if (cookie)
                            break;
                    }
                }

                unit->io_wait_task   = NULL;
                unit->io_cookie      = NULL;
                unit->io_signal_mask = 0;
                IExec->SetSignal(0, sig_mask);
                IExec->FreeSignal(sig_bit);
            } else {
                /* AllocSignal failed — polling fallback */
                DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: AllocSignal failed, polling\n");
                uint32 poll_timeout = 5000000;
                while (poll_timeout-- > 0) {
                    cookie = VirtQueue_GetBuf(vq, &written);
                    if (cookie)
                        break;
                }
            }
        } else {
            /* No interrupt handler installed — polling mode */
            uint32 poll_timeout = 5000000;
            while (poll_timeout-- > 0) {
                cookie = VirtQueue_GetBuf(vq, &written);
                if (cookie)
                    break;
            }
        }

        /* Always release the data DMA mapping for this attempt */
        cleanup_dma(IExec, &db_data);

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
            if (residual_out)
                *residual_out = resp_cmd->residual;

            if (resp_cmd->status == 2) { /* CHECK CONDITION */
                DPRINTF(IExec, "[virtioscsi] DoIO: T%lu L%lu CHECK CONDITION (Tries left: %ld)\n",
                        target, lun, (long)tries);
                if (tries > 0)
                    continue; /* retry — resp re-zeroed at top of loop */
                result = HFERR_BadStatus;
            } else if (resp_cmd->status == 0) {
                result = 0;
            } else {
                result = HFERR_BadStatus;
            }
        }

        break;
    }

    /* Discovery path: free temporary DMA mappings and buffers */
    if (temp_alloc) {
        cleanup_dma(IExec, &db_resp_tmp);
        cleanup_dma(IExec, &db_req_tmp);
        IExec->FreeVec(resp_cmd);
        IExec->FreeVec(req_cmd);
    }

    DPRINTF(IExec, "[virtioscsi:virtio_scsi_io.c] DoIO: Done (T%lu L%lu, result %ld)\n",
            target, lun, (long)result);

    return result;
}
