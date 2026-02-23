#ifndef VIRTIO_SCSI_H
#define VIRTIO_SCSI_H

#include <devices/scsidisk.h>
#include <devices/trackdisk.h>
#include <exec/devices.h>
#include <exec/interrupts.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/libraries.h>
#include <exec/types.h>

#include <proto/dos.h>
#include <proto/exec.h>

#include <expansion/pci.h>
#include <interfaces/expansion.h>
#include <interfaces/utility.h>
#include <utility/utility.h>

#include "version.h"
#include "virtio/virtio_scsi_cmd.h"

struct VirtIOSCSIBase
{
    struct Device dev_Base;
    struct ExecIFace *IExec;
    struct Library *ExpansionBase;
    struct PCIIFace *IPCI;
    struct Library *UtilityBase;
    struct UtilityIFace *IUtility;
    struct PCIDevice *pciDevice;
    struct PCIResourceRange *bar0; // I/O
    struct PCIResourceRange *bar4; // MMIO

    /* VirtIO */
    struct virtqueue *vqs[3]; // 0: controlq, 1: eventq, 2: requestq

    BPTR dev_SegList;
    struct VirtIOUSCSIDevUnit *units[8];

    /* Phase 5/6: VirtQueue submit lock (held only during AddBuf+Kick, not during Wait) */
    struct SignalSemaphore io_lock;

    /* Phase 5: Interrupt support */
    struct Interrupt irq_handler;   /* Interrupt server node */
    uint32          irq_number;     /* Vector from MapInterrupt() */
    BOOL            irq_installed;  /* TRUE if handler is active */
};

struct VirtIOUSCSIDevUnit
{
    struct Unit dev_Unit;
    uint32 unit_num;
    uint32 target_id;
    uint32 lun_id;
    uint32 open_count;
    /* Cached disk geometry (populated on first READ CAPACITY) */
    uint32 total_blocks;
    uint32 block_size;
    BOOL geometry_valid;
    /* Held change notification requests (must NOT be replied to until removed) */
    struct IOStdReq *changeint_req; /* Held TD_ADDCHANGEINT IORequest */
    struct IOStdReq *remove_req;    /* Held TD_REMOVE IORequest */

    /* Phase 6: Async I/O — unit device task */
    struct Task    *task;           /* Unit device task (NULL if not running) */
    struct MsgPort *io_port;        /* Incoming IORequest queue */
    uint32          io_port_mask;   /* Precomputed 1 << io_port->mp_SigBit */
    BOOL            task_shutdown;  /* Set TRUE to request task exit */

    /*
     * Persistent ISR signal: the unit task allocates one signal bit at startup
     * and stores it here. The ISR fires this to wake the task on any VirtIO
     * completion for this unit.
     *
     * In the synchronous DoIO path (HD_SCSICMD, discovery), io_wait_task and
     * io_signal_mask are set/cleared per-call (old behaviour, kept for compat).
     * In the pipeline path (block I/O), io_wait_task == task permanently so the
     * ISR signals the unit task on every completion.
     */
    struct Task    *io_wait_task;   /* Task to signal on VirtIO completion */
    uint32          io_signal_mask; /* Signal mask for io_wait_task */
    void           *io_cookie;      /* Unused in pipeline path; kept for DoIO compat */

    /*
     * Pre-allocated I/O buffers (set up at UnitTask_Start, freed at Shutdown).
     * req_buf and resp_buf are MEMF_SHARED allocations with DMA mappings kept
     * live for the unit's lifetime. Used by the synchronous VirtIOSCSI_DoIO path.
     */
    struct virtio_scsi_req_cmd  *req_buf;          /* VirtIO SCSI request header */
    struct virtio_scsi_resp_cmd *resp_buf;          /* VirtIO SCSI response buffer */
    struct DMAEntry             *dma_req_list;      /* DMA scatter list for req_buf */
    uint32                       dma_req_entries;   /* Entry count from StartDMA */
    struct DMAEntry             *dma_resp_list;     /* DMA scatter list for resp_buf */
    uint32                       dma_resp_entries;  /* Entry count from StartDMA */

    /*
     * Pipeline inflight table (Phase 7): allows multiple block I/O requests
     * to be submitted to VirtIO without waiting for each to complete.
     * Slot 0 is also used by the synchronous path to avoid double-allocation.
     * ioreq == NULL means the slot is free.
     */
#define MAX_INFLIGHT 8

    struct {
        struct IOStdReq *ioreq;    /* NULL = slot free */
        void            *cookie;   /* req_cmd ptr for VirtQueue_GetBuf matching */
        /* User-data DMA buffer for this in-flight request */
        APTR             dma_addr;
        uint32           dma_size;
        uint32           dma_flags;
        struct DMAEntry *dma_list;
        uint32           dma_num_entries;
        /* Which pre-alloc slot's req_buf/resp_buf this uses */
        uint32           buf_slot;
        /* Decoded result for ReplyMsg (set by Harvest, read nowhere — ReplyMsg done inline) */
        uint8            scsi_status;
        uint32           residual;
    } inflight[MAX_INFLIGHT];

    /* Per-slot pre-allocated req/resp buffers with permanent DMA mappings.
     * Slots 1..MAX_INFLIGHT-1 are additional pipeline buffers beyond req_buf. */
    struct virtio_scsi_req_cmd  *req_bufs[MAX_INFLIGHT];  /* [0] == req_buf */
    struct virtio_scsi_resp_cmd *resp_bufs[MAX_INFLIGHT]; /* [0] == resp_buf */
    struct DMAEntry             *dma_req_lists[MAX_INFLIGHT];
    uint32                       dma_req_entries_arr[MAX_INFLIGHT];
    struct DMAEntry             *dma_resp_lists[MAX_INFLIGHT];
    uint32                       dma_resp_entries_arr[MAX_INFLIGHT];
};

/* Prototypes for _man functions */
struct Library *_manager_Init(struct Library *library, BPTR seglist, struct Interface *exec);
struct VirtIOSCSIBase *_manager_Open(struct DeviceManagerInterface *Self, struct IOStdReq *ioreq, ULONG unitNum,
                                     ULONG flags);
BPTR _manager_Expunge(struct DeviceManagerInterface *Self);
BPTR _manager_Close(struct DeviceManagerInterface *Self, struct IOStdReq *ioreq);
void _manager_BeginIO(struct DeviceManagerInterface *Self, struct IOStdReq *ioreq);
LONG _manager_AbortIO(struct DeviceManagerInterface *Self, struct IOStdReq *ioreq);

uint32 _manager_Obtain(struct DeviceManagerInterface *Self);
uint32 _manager_Release(struct DeviceManagerInterface *Self);

/*
 * Debugging macro.
 * Define DEBUG and DEBUG_VERBOSE for additional output.
 */
#ifdef DEBUG
#define DPRINTF(iexec, ...) ((iexec)->DebugPrintF(__VA_ARGS__))
#else
#define DPRINTF(iexec, ...)                                                                                            \
    do {                                                                                                               \
        if (0)                                                                                                         \
            ((struct ExecIFace *)(iexec))->DebugPrintF(__VA_ARGS__);                                                   \
    } while (0)
#endif

#endif /* VIRTIO_SCSI_H */
