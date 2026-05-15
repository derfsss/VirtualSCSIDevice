#ifndef VIRTIOSCSI_H
#define VIRTIOSCSI_H

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
#include <interfaces/mounter.h>
#include <interfaces/utility.h>
#include <libraries/mounter.h>
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

    /*
     * Phase 10: mounter.library handle for hot-add DOSNode publication.
     *
     * Lifecycle:
     *   - Both pointers start NULL.  First-time use is lazy: the event task
     *     calls EnsureMounter() before AnnounceDevice / DenounceDevice and
     *     skips the call if open fails (mounter is non-fatal — the unit is
     *     still reachable via OpenDevice even without auto-mount).
     *   - Cleanup happens in _manager_Expunge AFTER ShutdownEventQueue
     *     completes, so no event-task code is racing the close.
     *
     * Why lazy: at boot (resident priority 0), mounter.library is not yet
     * initialised — calling AnnounceDevice from Init crashes MediaToolbox
     * (verified empirically pre-v53.8).  Boot-time disks already get their
     * DOSNodes from diskboot.kmod via diskboot.config; the mounter path is
     * exclusively for hot-added disks discovered after boot.
     */
    struct Library      *MounterBase;
    struct MounterIFace *IMounter;
    struct PCIDevice *pciDevice;
    struct PCIResourceRange *bar0; /* Legacy: I/O BAR; Modern: may be memory BAR */
    struct PCIResourceRange *bar4; /* MMIO BAR (unused in legacy) */

    /* Modern VirtIO mode flag and config region physical addresses.
     * Populated by DetectModernVirtIO() before InitVirtIOSCSI(). */
    BOOL   modern_mode;        /* TRUE if VIRTIO_F_VERSION_1 caps found */
    uint32 common_cfg_base;    /* Physical: BAR base + COMMON_CFG cap offset */
    uint32 notify_cfg_base;    /* Physical: BAR base + NOTIFY_CFG cap offset */
    uint32 notify_off_mult;    /* Per-queue notify offset multiplier (0 = shared) */
    uint32 isr_cfg_base;       /* Physical: BAR base + ISR_CFG cap offset */
    uint32 device_cfg_base;    /* Physical: BAR base + DEVICE_CFG cap offset */

    /* VirtIO */
    struct virtqueue *vqs[3]; /* 0: controlq, 1: eventq, 2: requestq */

    BPTR dev_SegList;
    struct VirtIOUSCSIDevUnit *units[8];

    /* Phase 5/6: VirtQueue submit lock (held only during AddBuf+Kick, not during Wait) */
    struct SignalSemaphore io_lock;

    /* Phase 12: Global inflight occupancy counter (all units combined).
     * Used by interrupt coalescing to avoid scanning all units × slots. */
    uint32 occupied_count;

    /* Bitmask of units with inflight I/O (bit N = unit N has pending requests).
     * Set in Submit/DoIO, cleared in Harvest when per-unit inflight_count drops to 0.
     * Read by ISR without lock — benign races (see virtio_irq.c). */
    uint8  active_units_mask;

    /* Phase 5: Interrupt support */
    struct Interrupt irq_handler;   /* Interrupt server node */
    uint32          irq_number;     /* Vector from MapInterrupt() */
    BOOL            irq_installed;  /* TRUE if handler is active */

    /* Phase 9: VirtIO SCSI event queue (VQ1) — HOTPLUG + PARAM_CHANGE.
     * Allocated/populated in InitEventQueue() when HOTPLUG or CHANGE is
     * negotiated.  Event buffers are posted on init and re-posted after
     * the event task consumes them. */
#define VIRTIO_SCSI_EVENT_BUFS 8
    BOOL             events_enabled;                 /* HOTPLUG || CHANGE negotiated */
    uint8           *event_pool;                     /* flat MEMF_SHARED pool, N*sizeof(event) */
    uint32           event_pool_size;                /* bytes */
    uint32           event_pool_phys;                /* DMA-mapped physical base */
    struct DMAEntry *event_pool_dma_list;
    uint32           event_pool_dma_entries;
    struct Task     *event_task;                     /* Event consumer task */
    uint32           event_signal_mask;              /* Signal the ISR fires */
    uint8            event_signal_bit;               /* Bit allocated by event_task */
    BOOL             event_task_shutdown;            /* Set to request task exit */

    /* Cross-task shutdown handshake: the caller of ShutdownEventQueue owns
     * event_exit_task + event_exit_mask; the worker signals it on exit. */
    struct Task     *event_exit_task;
    uint32           event_exit_mask;
};

struct VirtIOUSCSIDevUnit
{
    struct Unit dev_Unit;
    uint32 unit_num;
    uint32 target_id;
    uint32 lun_id;
    uint32 open_count;
    /* Cached disk geometry (populated on first READ CAPACITY) */
    uint64 total_blocks; /* 64-bit: supports >2TB disks (READ CAPACITY 16) */
    uint32 block_size;
    BOOL geometry_valid;

    /*
     * Physical CHS values as declared in the disk's RDB (RigidDiskBlock).
     * Populated on first TD_GETGEOMETRY by reading block 0 and checking for
     * the "RDSK" signature.  If present, we return these values so the
     * geometry we report matches what HDToolbox (or whoever initialised
     * the disk) wrote into the RDB — which is what every downstream
     * partition block is shaped around.  If there's no RDB (fresh disk),
     * `rdb_geometry_valid` stays FALSE and TD_GETGEOMETRY falls back to a
     * synthesized 1 head × 1 sector-per-track "linear" geometry that makes
     * no assumptions about shape.
     */
    uint32 rdb_phys_cyls;      /* rdb_Cylinders    @ RDB offset 0x40 */
    uint32 rdb_phys_sectors;   /* rdb_Sectors      @ RDB offset 0x44 (BlocksPerTrack) */
    uint32 rdb_phys_heads;     /* rdb_Heads        @ RDB offset 0x48 (Surfaces) */
    BOOL   rdb_geometry_valid; /* TRUE if block 0 had a valid RDSK signature */
    BOOL   rdb_geometry_checked; /* TRUE once the probe has run (success or fail) */
    /* Held change notification requests (must NOT be replied to until removed) */
    struct IOStdReq *changeint_req; /* Held TD_ADDCHANGEINT IORequest */
    struct IOStdReq *remove_req;    /* Held TD_REMOVE IORequest */

    /* Phase 10: media change tracking (CD insert/eject, capacity change).
     * change_count is reported by TD_CHANGENUM and incremented on every
     * event that alters media state.  media_present tells TD_CHANGESTATE
     * whether a disk is inserted (0 = present, 1 = no disk). */
    uint32 change_count;
    BOOL   media_present;

    /*
     * Phase 10: mounter announcement state.
     * TRUE once IMounter->AnnounceDeviceTags() has successfully handed the
     * unit over to mounter.library.  Once TRUE, the driver MUST call
     * DenounceDevice() before freeing the unit or on hot-removal — failing
     * to do so leaves stale DOSNodes and a dangling reference in mounter.
     * Only set by AnnounceIfHotAdded(); only cleared by DenounceIfAnnounced().
     */
    BOOL   announced;

    /* Phase 6: Async I/O — unit device task */
    struct Task    *task;           /* Unit device task (NULL if not running) */
    struct MsgPort *io_port;        /* Incoming IORequest queue */
    uint32          io_port_mask;   /* Precomputed 1 << io_port->mp_SigBit */
    BOOL            task_shutdown;  /* Set TRUE to request task exit */
    char            task_name[32];  /* Persistent name for CreateTaskTags (ln_Name stores pointer) */

    /* Shutdown handshake: caller of UnitTask_Shutdown sets these, then Waits.
     * The unit task signals shutdown_exit_task on exit. */
    struct Task    *shutdown_exit_task;
    uint32          shutdown_exit_mask;

    /* Mutex protecting io_port message queue traversal (AbortIO vs GetMsg).
     * Replaces deprecated Forbid/Permit.  Allocated by unit task, freed on exit. */
    APTR            port_mutex;

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
     * DoIO cookie handoff: when Harvest dequeues a cookie that matches no
     * inflight pipeline slot, it may belong to a concurrent DoIO call on
     * another unit that hasn't yet reached its GetBuf drain loop.
     *
     * Instead of discarding the cookie (which would cause DoIO to time out),
     * Harvest stores it here and signals io_wait_task.  DoIO's drain loop
     * checks this field (under io_lock) before calling GetBuf.
     *
     * Written by Harvest (holding io_lock), read+cleared by DoIO drain loop
     * (holding io_lock). Initialised to NULL.
     */
    void   *doio_pending_cookie;  /* stashed cookie for DoIO pickup */
    uint32  doio_pending_written; /* bytes written for the stashed cookie */

    /*
     * Pipeline inflight table (Phase 7): allows multiple block I/O requests
     * to be submitted to VirtIO without waiting for each to complete.
     * Slot 0 is also used by the synchronous path to avoid double-allocation.
     * ioreq == NULL means the slot is free.
     */
#define MAX_INFLIGHT 16

/*
     * Bounce buffer size threshold.  Transfers at or below this size use a
     * pre-pinned MEMF_SHARED bounce buffer instead of per-call StartDMA/EndDMA.
     * 65536 (64KB) covers virtually all AmigaOS filesystem block I/O (SFS, FFS2)
     * with zero DMA setup overhead.  Larger transfers use direct user-buffer DMA.
     * Memory cost: 64KB × MAX_INFLIGHT per active unit (~1MB).
     */
#define BOUNCE_BUF_SIZE 65536

    /* Phase 12: O(1) free-slot allocation via singly-linked free list.
     * free_head is the index of the first free slot, or -1 if all occupied.
     * inflight_next[i] is the next free slot after i, or -1 for end of list. */
    int32 free_head;
    int32 inflight_next[MAX_INFLIGHT];

    /* Per-unit count of occupied pipeline inflight slots.
     * Used to clear active_units_mask bit when this unit drains to zero. */
    uint32 inflight_count;

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
        /* Bounce-buffer path: set when data_len <= BOUNCE_BUF_SIZE */
        BOOL             using_bounce; /* TRUE = bounce_bufs[slot] used */
        BOOL             is_write;     /* TRUE = write, FALSE = read (for read-back) */
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

    /*
     * Per-slot bounce buffers (BOUNCE_BUF_SIZE bytes each, MEMF_SHARED).
     * Permanently DMA-mapped so transfers <= BOUNCE_BUF_SIZE incur zero
     * per-call StartDMA/EndDMA overhead.  Write path: copy user→bounce before
     * Submit.  Read path: copy bounce→user in Harvest before ReplyMsg.
     * bounce_dma_phys[s] is the single physical address (contiguous alloc).
     */
    uint8   *bounce_bufs[MAX_INFLIGHT];          /* virtual address of bounce buffer */
    uint32   bounce_dma_phys[MAX_INFLIGHT];      /* physical address for SG entry */
    uint32   bounce_dma_entries[MAX_INFLIGHT];   /* StartDMA entry count (kept for EndDMA) */

    /*
     * Phase 12: Pre-allocated DMA entry arrays for the direct DMA path.
     * One array per inflight slot, each with MAX_SG_ENTRIES capacity.
     * Eliminates per-request AllocSysObjectTags/FreeSysObject on the hot path.
     * Allocated at unit startup, freed at shutdown.
     */
    struct DMAEntry *data_dma_pool[MAX_INFLIGHT];
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
 * Reply and clear any held asynchronous TD_ADDCHANGEINT / TD_REMOVE
 * IORequest on a unit. Both fields are set NULL after replying.
 *
 * error_code: 0 for "normal" completion (e.g. RESET_REMOVED completing
 * a TD_REMOVE handshake), IOERR_ABORTED for teardown/shutdown where the
 * caller's wait is being broken rather than satisfied.
 *
 * Safe to call with unit == NULL (no-op). Safe to call when neither
 * field is set (no-op).
 */
void Reply_Held_Async_Reqs(struct ExecIFace *IExec,
                           struct VirtIOUSCSIDevUnit *unit,
                           int8 error_code);

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

#endif /* VIRTIOSCSI_H */
