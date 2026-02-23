#include "virtio/virtqueue.h"
#include "virtio/virtio_scsi.h"
#include "virtioscsi.h"
#include <exec/exectags.h>
#include <exec/memory.h>
#include <expansion/pci.h>
#include <interfaces/exec.h>
#include <interfaces/expansion.h>

/*
 * Legacy VirtQueue Layout (VirtIO Spec Section 2.7.2)
 *
 * Queue Align = 4096:
 *   Descriptor Table:  offset 0
 *   Available Ring:     immediately after descriptors
 *   (padding to next 4096-byte boundary)
 *   Used Ring:          aligned to 4096
 *
 * Endianness (Section 2.7.3):
 *   Legacy = NATIVE GUEST ENDIAN (Big Endian on PPC).
 *   No byte-swapping needed for any vring fields.
 */

#define VIRTIO_PCI_VRING_ALIGN 4096

struct virtqueue *VirtQueue_Allocate(struct ExecIFace *IExec, uint32 queue_index, uint32 queue_size)
{
    /* Calculate sizes per the Legacy vring formula */
    uint32 desc_size = sizeof(struct vring_desc) * queue_size;
    uint32 avail_size = sizeof(uint16) * (3 + queue_size);
    uint32 avail_end = desc_size + avail_size;

    uint32 used_offset = (avail_end + VIRTIO_PCI_VRING_ALIGN - 1) & ~(VIRTIO_PCI_VRING_ALIGN - 1);
    uint32 used_size = sizeof(uint16) * 3 + sizeof(struct vring_used_elem) * queue_size;
    uint32 total_mem = used_offset + used_size;

    /* Over-allocate by one page for manual alignment */
    uint32 alloc_size = total_mem + VIRTIO_PCI_VRING_ALIGN;
    void *raw = IExec->AllocVecTags(alloc_size, AVT_ClearWithValue, 0, AVT_Type, MEMF_SHARED, TAG_DONE);

    if (!raw) {
        DPRINTF(IExec, "[virtioscsi:virtqueue.c] VQ%lu: ring alloc failed (%lu bytes)\n", queue_index, alloc_size);
        return NULL;
    }

    /* Page-align within the raw allocation */
    uint32 raw_addr = (uint32)raw;
    uint32 aligned_addr = (raw_addr + VIRTIO_PCI_VRING_ALIGN - 1) & ~(VIRTIO_PCI_VRING_ALIGN - 1);
    uint8 *base = (uint8 *)aligned_addr;

    /* Allocate the management struct */
    struct virtqueue *vq =
        IExec->AllocVecTags(sizeof(struct virtqueue), AVT_ClearWithValue, 0, AVT_Type, MEMF_SHARED, TAG_DONE);

    if (!vq) {
        IExec->FreeVec(raw);
        return NULL;
    }

    /* Allocate cookie array (one pointer per descriptor slot) */
    void **cookies =
        IExec->AllocVecTags(sizeof(void *) * queue_size, AVT_ClearWithValue, 0, AVT_Type, MEMF_PRIVATE, TAG_DONE);

    if (!cookies) {
        IExec->FreeVec(raw);
        IExec->FreeVec(vq);
        return NULL;
    }

    /* Allocate indirect_tables array — parallel to cookies[], initialised to NULL */
    void **indirect_tables =
        IExec->AllocVecTags(sizeof(void *) * queue_size, AVT_ClearWithValue, 0, AVT_Type, MEMF_PRIVATE, TAG_DONE);

    if (!indirect_tables) {
        IExec->FreeVec(cookies);
        IExec->FreeVec(raw);
        IExec->FreeVec(vq);
        return NULL;
    }

    vq->index = queue_index;
    vq->num = queue_size;
    vq->mem_block = raw;
    vq->mem_size = total_mem;
    vq->cookies = cookies;
    vq->indirect_tables = indirect_tables;

    /* Map vring pointers */
    vq->desc = (struct vring_desc *)base;
    vq->avail = (struct vring_avail *)(base + desc_size);
    vq->used = (struct vring_used *)(base + used_offset);

    /* Initialize the free descriptor chain: each desc points to the next */
    vq->free_head = 0;
    vq->num_free = (uint16)queue_size;
    vq->last_used_idx = 0;

    uint32 j;
    for (j = 0; j < queue_size - 1; j++) {
        vq->desc[j].next = (uint16)(j + 1);
    }
    vq->desc[queue_size - 1].next = 0; /* Last wraps (won't be followed due to no NEXT flag) */

    /* Allow device to send interrupts on used ring updates */
    vq->avail->flags = 0;

    DPRINTF(IExec, "[virtioscsi] VQ%lu: %lu entries @ 0x%08lX (PFN=0x%08lX)\n", queue_index, queue_size, aligned_addr,
            aligned_addr / 4096);

    return vq;
}

void VirtQueue_Free(struct ExecIFace *IExec, struct virtqueue *vq)
{
    if (!vq)
        return;
    /* Release the DMA mapping established in VirtQueue_Allocate/InitVirtIO */
    if (vq->dma_entries > 0 && vq->desc) {
        IExec->EndDMA(vq->desc, vq->mem_size, DMA_ReadFromRAM | DMAF_NoModify);
        vq->dma_entries = 0;
    }
    if (vq->indirect_tables)
        IExec->FreeVec(vq->indirect_tables);
    if (vq->cookies)
        IExec->FreeVec(vq->cookies);
    if (vq->mem_block)
        IExec->FreeVec(vq->mem_block);
    IExec->FreeVec(vq);
}

/*
 * VirtQueue_AddBuf: Add a scatter-gather buffer chain to the available ring.
 *
 * sg[0..out_num-1] = device-readable (OUT) entries
 * sg[out_num..out_num+in_num-1] = device-writable (IN) entries
 * cookie = opaque pointer returned by GetBuf when the request completes
 *
 * Returns 0 on success, -1 if not enough free descriptors.
 */
int32 VirtQueue_AddBuf(struct ExecIFace *IExec, struct virtqueue *vq, struct vring_sg *sg, uint32 out_num, uint32 in_num, void *cookie)
{
    uint32 total = out_num + in_num;

    /* Need at least one free descriptor (one for indirect, or 'total' for direct) */
    if (vq->use_indirect && total > 1) {
        if (vq->num_free < 1)
            return -1;
    } else {
        if (vq->num_free < total)
            return -1;
    }

    uint16 head = vq->free_head;
    uint32 n;

    if (vq->use_indirect && total > 1) {
        /*
         * VIRTIO_F_INDIRECT_DESC path: allocate a flat array of
         * vring_indirect_desc in MEMF_SHARED, fill it, DMA-map it, and
         * place a single descriptor in the vring that points to the table.
         * Consumes exactly 1 vring descriptor regardless of SG count.
         */
        struct vring_indirect_desc *itbl =
            IExec->AllocVecTags(sizeof(struct vring_indirect_desc) * total,
                                AVT_ClearWithValue, 0, AVT_Type, MEMF_SHARED, TAG_DONE);

        if (itbl) {
            /* Fill the indirect table */
            for (n = 0; n < total; n++) {
                itbl[n].addr  = (uint64)sg[n].addr;
                itbl[n].len   = sg[n].len;
                itbl[n].flags = (n >= out_num) ? VRING_DESC_F_WRITE : 0;
                itbl[n].next  = 0; /* unused */
            }

            /* DMA-map the indirect table so the device can read it */
            uint32 itbl_size = sizeof(struct vring_indirect_desc) * total;
            uint32 itbl_entries = IExec->StartDMA(itbl, itbl_size, DMA_ReadFromRAM);
            if (itbl_entries > 0) {
                struct DMAEntry *itbl_dma = (struct DMAEntry *)IExec->AllocSysObjectTags(
                    ASOT_DMAENTRY, ASODMAE_NumEntries, itbl_entries, TAG_DONE);
                if (itbl_dma) {
                    IExec->GetDMAList(itbl, itbl_size, DMA_ReadFromRAM, itbl_dma);
                    uint32 itbl_phys = (uint32)itbl_dma[0].PhysicalAddress;
                    IExec->FreeSysObject(ASOT_DMAENTRY, itbl_dma);

                    /* Place one descriptor in the vring */
                    vq->desc[head].addr  = (uint64)itbl_phys;
                    vq->desc[head].len   = itbl_size;
                    vq->desc[head].flags = VRING_DESC_F_INDIRECT;
                    vq->desc[head].next  = 0;
                    vq->free_head = vq->desc[head].next;
                    vq->num_free -= 1;

                    /* Save virtual address for cleanup in GetBuf */
                    vq->cookies[head] = cookie;
                    vq->indirect_tables[head] = (void *)itbl;

                    /* Add to available ring */
                    uint16 avail_idx = vq->avail->idx;
                    vq->avail->ring[avail_idx % vq->num] = head;
                    __asm__ volatile("eieio" ::: "memory");
                    vq->avail->idx = avail_idx + 1;
                    return 0;
                }
                IExec->EndDMA(itbl, itbl_size, DMA_ReadFromRAM | DMAF_NoModify);
            }
            /* DMA mapping failed — fall through to direct path */
            IExec->FreeVec(itbl);
        }
        /* Indirect allocation failed — fall through to direct path if room */
        if (vq->num_free < total)
            return -1;
    }

    /* Direct descriptor chain path */
    uint16 idx = head;

    for (n = 0; n < total; n++) {
        vq->desc[idx].addr = (uint64)sg[n].addr;
        vq->desc[idx].len = sg[n].len;
        vq->desc[idx].flags = 0;

        /* Set WRITE flag for device-writable (IN) entries */
        if (n >= out_num) {
            vq->desc[idx].flags |= VRING_DESC_F_WRITE;
        }

        /* Chain to next descriptor (except the last one) */
        if (n < total - 1) {
            vq->desc[idx].flags |= VRING_DESC_F_NEXT;
            idx = vq->desc[idx].next;
        } else {
            /* Advance free_head past this chain */
            vq->free_head = vq->desc[idx].next;
        }
    }

    vq->num_free -= (uint16)total;

    /* Store the cookie at the head descriptor index */
    vq->cookies[head] = cookie;
    /* No indirect table for direct path */
    vq->indirect_tables[head] = NULL;

    /* Add head to the available ring */
    uint16 avail_idx = vq->avail->idx;
    vq->avail->ring[avail_idx % vq->num] = head;

    /* Memory barrier: ensure descriptor writes are visible before idx update. */
    __asm__ volatile("eieio" ::: "memory");

    vq->avail->idx = avail_idx + 1;

    return 0;
}

/*
 * VirtQueue_Kick: Notify the device that new buffers are available.
 * Writes the queue index to VIRTIO_PCI_QUEUE_NOTIFY via PCI I/O.
 *
 * With VIRTIO_F_EVENT_IDX: the device writes the avail_event index into
 * the word immediately after the used ring's ring[] array. We only need
 * to notify if avail->idx has reached or crossed that threshold, avoiding
 * unnecessary PCI writes under sustained load.
 */
void VirtQueue_Kick(struct virtqueue *vq, struct PCIDevice *pciDev, uint32 iobase)
{
    /* Memory barrier: ensure avail->idx write is visible before the notify */
    __asm__ volatile("sync" ::: "memory");

    if (vq->use_event_idx) {
        /*
         * avail_event is stored by the device at the end of the used ring:
         *   used->ring[num] (first word past the last used_elem)
         * Cast: vring_used has a flexible ring[], so index past num entries.
         */
        uint16 avail_event = vq->used->ring[vq->num].id; /* id field = low 16 bits */
        uint16 new_idx     = vq->avail->idx;
        uint16 old_idx     = vq->last_kick_avail_idx;

        /*
         * Notify if new_idx wrapped around avail_event since last kick.
         * Condition: (new_idx - avail_event - 1) < (new_idx - old_idx)
         * (unsigned 16-bit arithmetic handles wraparound correctly)
         */
        if ((uint16)(new_idx - avail_event - 1) < (uint16)(new_idx - old_idx)) {
            vq->last_kick_avail_idx = new_idx;
            pciDev->OutWord(iobase + VIRTIO_PCI_QUEUE_NOTIFY, (uint16)vq->index);
        }
    } else {
        pciDev->OutWord(iobase + VIRTIO_PCI_QUEUE_NOTIFY, (uint16)vq->index);
    }
}

/*
 * VirtQueue_GetBuf: Check the used ring for completed descriptors.
 *
 * Returns the cookie for a completed buffer, or NULL if the used ring
 * hasn't advanced. *len_out receives the bytes written by the device.
 */
void *VirtQueue_GetBuf(struct ExecIFace *IExec, struct virtqueue *vq, uint32 *len_out)
{
    /* Memory barrier: read used->idx after device writes */
    __asm__ volatile("lwsync" ::: "memory");

    if (vq->last_used_idx == vq->used->idx)
        return NULL;

    /* Read the next used ring entry */
    uint16 used_slot = vq->last_used_idx % vq->num;
    uint32 desc_id = vq->used->ring[used_slot].id;
    uint32 written = vq->used->ring[used_slot].len;

    if (len_out)
        *len_out = written;

    /* Retrieve the cookie */
    void *cookie = vq->cookies[desc_id];
    vq->cookies[desc_id] = NULL;

    /* Check if this was an indirect descriptor */
    void *itbl_virt = vq->indirect_tables[desc_id];
    vq->indirect_tables[desc_id] = NULL;

    /* Return descriptors to the free list */
    uint16 idx = (uint16)desc_id;
    uint32 freed = 0;

    if (itbl_virt) {
        /*
         * Indirect path: a single descriptor pointed to the indirect table.
         * Release the DMA mapping and free the table, then return 1 descriptor.
         * The indirect table length was stored in desc[idx].len.
         */
        uint32 itbl_size = vq->desc[idx].len;
        IExec->EndDMA(itbl_virt, itbl_size, DMA_ReadFromRAM | DMAF_NoModify);
        IExec->FreeVec(itbl_virt);

        vq->desc[idx].addr  = 0;
        vq->desc[idx].len   = 0;
        vq->desc[idx].flags = 0;
        vq->desc[idx].next  = vq->free_head;
        vq->free_head = idx;
        freed = 1;
    } else {
        /* Direct chain path: walk and free all descriptors in the chain */
        while (1) {
            uint16 next = vq->desc[idx].next;
            uint16 has_next = vq->desc[idx].flags & VRING_DESC_F_NEXT;

            vq->desc[idx].addr = 0;
            vq->desc[idx].len = 0;
            vq->desc[idx].flags = 0;
            vq->desc[idx].next = vq->free_head;
            vq->free_head = idx;
            freed++;

            if (!has_next)
                break;
            idx = next;
        }
    }

    vq->num_free += (uint16)freed;

    vq->last_used_idx++;

    return cookie;
}
