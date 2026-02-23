#ifndef VIRTQUEUE_H
#define VIRTQUEUE_H

#include <exec/types.h>

/* VirtQueue Descriptor Flags */
#define VRING_DESC_F_NEXT 1
#define VRING_DESC_F_WRITE 2
#define VRING_DESC_F_INDIRECT 4

/* VirtQueue Available Ring Flags */
#define VRING_AVAIL_F_NO_INTERRUPT 1

/* Maximum scatter-gather entries per VirtIO request.
 * 64KB at 4KB pages = 16 data entries + req_cmd + resp_cmd = 18 max.
 * 32 provides safe headroom. */
#define MAX_SG_ENTRIES 32

/* vring_desc - 16 bytes */
struct vring_desc
{
    uint64 addr;
    uint32 len;
    uint16 flags;
    uint16 next;
} __attribute__((packed));

/* vring_avail */
struct vring_avail
{
    uint16 flags;
    uint16 idx;
    uint16 ring[];
} __attribute__((packed));

/* vring_used_elem - 8 bytes */
struct vring_used_elem
{
    uint32 id;
    uint32 len;
} __attribute__((packed));

/* vring_used */
struct vring_used
{
    uint16 flags;
    uint16 idx;
    struct vring_used_elem ring[];
} __attribute__((packed));

/*
 * Scatter-gather entry for AddBuf.
 * addr = physical address of buffer, len = length in bytes.
 */
struct vring_sg
{
    uint32 addr;
    uint32 len;
};

/* VirtQueue High-Level Management Struct */
struct virtqueue
{
    uint32 index;
    uint32 num; /* Size of the rings (e.g. 256) */

    struct vring_desc *desc;
    struct vring_avail *avail;
    struct vring_used *used;

    /* Descriptor chain management */
    uint16 free_head;     /* Head of the free descriptor list */
    uint16 num_free;      /* Number of free descriptors */
    uint16 last_used_idx; /* Last used ring index we processed */

    /* Cookie storage: one per descriptor slot, for tracking IORequests */
    void **cookies;

    /* Base pointer to the raw allocation (for FreeVec) */
    void *mem_block;
    uint32 mem_size;

    /* DMA mapping for the vring (kept live; freed in VirtQueue_Free) */
    uint32 dma_phys;    /* Physical base address of the vring */
    uint32 dma_entries; /* Entry count returned by StartDMA (0 = not mapped) */

    /* VIRTIO_F_EVENT_IDX: suppress redundant kicks.
     * avail_event lives at the end of the used ring (device writes it).
     * We only notify when avail->idx crosses avail_event. */
    BOOL   use_event_idx; /* TRUE if VIRTIO_F_EVENT_IDX was negotiated */
    uint16 last_kick_avail_idx; /* avail->idx value at last Kick */
};

/* Function Prototypes */
struct ExecIFace; /* forward declaration */
struct PCIDevice; /* forward declaration */

struct virtqueue *VirtQueue_Allocate(struct ExecIFace *IExec, uint32 queue_index, uint32 queue_size);
void VirtQueue_Free(struct ExecIFace *IExec, struct virtqueue *vq);

/*
 * Add a buffer chain to the virtqueue's available ring.
 * sg[] contains out_num device-readable entries followed by in_num device-writable entries.
 * cookie is returned by GetBuf when the device completes the request.
 * Returns 0 on success, -1 on failure (no free descriptors).
 */
int32 VirtQueue_AddBuf(struct virtqueue *vq, struct vring_sg *sg, uint32 out_num, uint32 in_num, void *cookie);

/*
 * Notify the device that new buffers are available.
 * Writes the queue index to VIRTIO_PCI_QUEUE_NOTIFY via PCI I/O.
 */
void VirtQueue_Kick(struct virtqueue *vq, struct PCIDevice *pciDev, uint32 iobase);

/*
 * Check for completed buffers in the used ring.
 * Returns the cookie for a completed buffer, or NULL if none ready.
 * *len_out receives the number of bytes written by the device (if non-NULL).
 */
void *VirtQueue_GetBuf(struct virtqueue *vq, uint32 *len_out);

#endif /* VIRTQUEUE_H */
