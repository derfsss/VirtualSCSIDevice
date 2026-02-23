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

    /* Per-unit in-flight tracking (written by unit task, read by ISR) */
    struct Task    *io_wait_task;   /* Unit task sleeping for I/O completion */
    uint32          io_signal_mask; /* Signal mask for that task */
    void           *io_cookie;      /* Expected req_cmd cookie; NULL = ISR cleared it */

    /*
     * Pre-allocated I/O buffers (set up at UnitTask_Start, freed at Shutdown).
     * req_buf and resp_buf are MEMF_SHARED allocations with DMA mappings kept
     * live for the unit's lifetime, eliminating per-request alloc/DMA overhead.
     */
    struct virtio_scsi_req_cmd  *req_buf;          /* VirtIO SCSI request header */
    struct virtio_scsi_resp_cmd *resp_buf;          /* VirtIO SCSI response buffer */
    struct DMAEntry             *dma_req_list;      /* DMA scatter list for req_buf */
    uint32                       dma_req_entries;   /* Entry count from StartDMA */
    struct DMAEntry             *dma_resp_list;     /* DMA scatter list for resp_buf */
    uint32                       dma_resp_entries;  /* Entry count from StartDMA */
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
