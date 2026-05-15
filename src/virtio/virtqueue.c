#include "virtio/virtqueue.h"
#include "virtio/virtio_scsi.h"
#include "virtio/virtio_pci_modern.h"
#include "virtioscsi.h"
#include "sandboxvm_tags.h"
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

    /* Over-allocate by one page for manual alignment. SBV_AVT_HostDMA
     * tag is required so SandboxVM-hosted runs get a host-heap (DMA-
     * mappable) buffer; on native AOS4 the tag is ignored. */
    uint32 alloc_size = total_mem + VIRTIO_PCI_VRING_ALIGN;
    void *raw = IExec->AllocVecTags(alloc_size,
                                    AVT_ClearWithValue, 0,
                                    AVT_Type, MEMF_SHARED,
                                    SBV_AVT_HostDMA, 0,
                                    TAG_DONE);

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
    vq->modern = FALSE;    /* set TRUE by InitVirtIOSCSI_Modern() if applicable */
    vq->notify_addr = 0;   /* set by InitVirtIOSCSI_Modern() */
    vq->avail_phys = 0;    /* set after DMA mapping in virtio_init.c */
    vq->used_phys  = 0;    /* set after DMA mapping in virtio_init.c */

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
                                AVT_ClearWithValue, 0,
                                AVT_Type, MEMF_SHARED,
                                SBV_AVT_HostDMA, 0,
                                TAG_DONE);

        if (itbl) {
            /* Fill the indirect table.  Entries are in device-visible memory
             * so fields must match the negotiated endianness: modern = LE,
             * legacy = native (BE on PPC). */
            for (n = 0; n < total; n++) {
                uint16 f = (n >= out_num) ? VRING_DESC_F_WRITE : 0;
                /* Chain flag links consecutive table entries; the device walks
                 * the table via `next` even though in practice many devices
                 * accept a flat ordered list.  Set NEXT for all but the last. */
                if (n < total - 1)
                    f |= VRING_DESC_F_NEXT;
                itbl[n].addr  = vr64(vq->modern, (uint64)sg[n].addr);
                itbl[n].len   = vr32(vq->modern, sg[n].len);
                itbl[n].flags = vr16(vq->modern, f);
                itbl[n].next  = vr16(vq->modern, (uint16)(n + 1));
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

                    /* Capture the pre-chained free-list link BEFORE overwriting
                     * vq->desc[head].next with the indirect-descriptor format.
                     * Free-list links are always stored native. */
                    uint16 next_head = vq->desc[head].next;

                    /* Place one descriptor in the vring pointing at the table.
                     * Fields must match negotiated endianness (vr64/vr32/vr16). */
                    vq->desc[head].addr  = vr64(vq->modern, (uint64)itbl_phys);
                    vq->desc[head].len   = vr32(vq->modern, itbl_size);
                    vq->desc[head].flags = vr16(vq->modern, VRING_DESC_F_INDIRECT);
                    vq->desc[head].next  = 0;
                    vq->free_head = next_head;
                    vq->num_free -= 1;

                    /* Save virtual address for cleanup in GetBuf */
                    vq->cookies[head] = cookie;
                    vq->indirect_tables[head] = (void *)itbl;

                    /* Add to available ring */
                    uint16 avail_idx = vr16(vq->modern, vq->avail->idx);
                    vq->avail->ring[avail_idx % vq->num] = vr16(vq->modern, head);
                    __asm__ volatile("eieio" ::: "memory");
                    vq->avail->idx = vr16(vq->modern, (uint16)(avail_idx + 1));
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
        uint16 desc_flags = 0;

        /* Set WRITE flag for device-writable (IN) entries */
        if (n >= out_num)
            desc_flags |= VRING_DESC_F_WRITE;

        /* Chain to next descriptor (except the last one) */
        if (n < total - 1)
            desc_flags |= VRING_DESC_F_NEXT;

        /*
         * Write descriptor fields.  In modern mode all fields are little-endian;
         * vr64/vr32/vr16 byte-swap as needed.  In legacy mode they are no-ops.
         *
         * Read vq->desc[idx].next (the pre-chained free-list link) BEFORE
         * overwriting flags, since we need the native value for the chain walk.
         */
        uint16 next_idx = vq->desc[idx].next; /* always stored native in free list */
        vq->desc[idx].addr  = vr64(vq->modern, (uint64)sg[n].addr);
        vq->desc[idx].len   = vr32(vq->modern, sg[n].len);
        vq->desc[idx].flags = vr16(vq->modern, desc_flags);
        if (n < total - 1) {
            vq->desc[idx].next = vr16(vq->modern, next_idx);
            idx = next_idx;
        } else {
            /* Advance free_head past this chain (next_idx is the next free slot) */
            vq->free_head = next_idx;
        }
    }

    vq->num_free -= (uint16)total;

    /* Store the cookie at the head descriptor index */
    vq->cookies[head] = cookie;
    /* No indirect table for direct path */
    vq->indirect_tables[head] = NULL;

    /* Add head to the available ring */
    uint16 avail_idx = vr16(vq->modern, vq->avail->idx);
    vq->avail->ring[avail_idx % vq->num] = vr16(vq->modern, head);

    /* Memory barrier: ensure descriptor writes are visible before idx update. */
    __asm__ volatile("eieio" ::: "memory");

    vq->avail->idx = vr16(vq->modern, (uint16)(avail_idx + 1));

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
void VirtQueue_Kick(struct ExecIFace *IExec, struct virtqueue *vq, struct PCIDevice *pciDev, uint32 iobase)
{
    /* Memory barrier: ensure avail->idx write is visible before the notify */
    __asm__ volatile("sync" ::: "memory");

    /*
     * Always notify the device unconditionally.
     *
     * VIRTIO_F_EVENT_IDX has two halves:
     *   - Driver kick suppression: device writes avail_event into used->ring[num].
     *     Driver only kicks when avail->idx crosses avail_event.
     *   - Interrupt suppression: driver writes used_event into avail->ring[num].
     *     Device only interrupts when used->idx crosses used_event.
     *
     * In practice QEMU legacy mode does NOT update avail_event (stays 0 forever),
     * so the kick-suppression check always evaluates FALSE after the first kick,
     * silently starving the device of notifications.  We accept EVENT_IDX to
     * enable the interrupt-suppression half (used_event, written in GetBuf) but
     * skip the kick-suppression half and always send QUEUE_NOTIFY.
     */
    DPRINTF(IExec, "[virtioscsi:virtqueue.c] Kick VQ%lu: notify (avail_idx=%u)\n",
            vq->index, (unsigned)vr16(vq->modern, vq->avail->idx));

    if (vq->modern) {
        /*
         * Modern VirtIO: write the queue index to the per-queue notification
         * address (notify_cfg_base + queue_notify_off * notify_off_mult).
         * SetEndian was already called once in InitVirtIOSCSI_Modern(); it
         * persists for the lifetime of this PCIDevice handle.
         */
        /* notify region is MMIO — OutWord doesn't work, use byte-assembly helper */
        mmio_w16(pciDev, vq->notify_addr, (uint16)vq->index);
    } else {
        /* Legacy: write queue index to the shared QUEUE_NOTIFY I/O port. */
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
    /* Memory barrier: read used->idx after device writes.
     *
     * Must be a base-ISA barrier that is implemented on every PowerPC
     * variant AmigaOS 4.1 FE runs on:
     *   - AmigaOne  (G3/G4, classic PowerPC)     — sync ✓, lwsync optional
     *   - Pegasos2  (G4 7457, classic PowerPC)   — sync ✓, lwsync ✓
     *   - SAM460ex  (PPC 440, Book-E)            — sync ✓, lwsync *absent*
     *
     * lwsync is an optional Power ISA Book II instruction; on PPC 440 it
     * is not part of the defined instruction set and may trap or be
     * silently remapped to sync depending on the implementation.  Using
     * sync is architecturally safe on all three cores and costs only an
     * extra ~10-40 cycles here — invisible against the MMIO latency the
     * caller is already paying to read the used ring.  The load-before-
     * load ordering we actually need (read used->idx after the device
     * wrote it) is provided by both; sync is simply the universally
     * available form. */
    __asm__ volatile("sync" ::: "memory");

    if (vq->last_used_idx == vr16(vq->modern, vq->used->idx))
        return NULL;

    /* Read the next used ring entry */
    uint16 used_slot = vq->last_used_idx % vq->num;
    uint32 desc_id = vr32(vq->modern, vq->used->ring[used_slot].id);
    uint32 written = vr32(vq->modern, vq->used->ring[used_slot].len);

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
        uint32 itbl_size = vr32(vq->modern, vq->desc[idx].len);
        IExec->EndDMA(itbl_virt, itbl_size, DMA_ReadFromRAM | DMAF_NoModify);
        IExec->FreeVec(itbl_virt);

        vq->desc[idx].addr  = 0;
        vq->desc[idx].len   = 0;
        vq->desc[idx].flags = 0;
        vq->desc[idx].next  = vq->free_head; /* free-list links always native */
        vq->free_head = idx;
        freed = 1;
    } else {
        /* Direct chain path: walk and free all descriptors in the chain */
        while (1) {
            uint16 next     = vr16(vq->modern, vq->desc[idx].next);
            uint16 has_next = vr16(vq->modern, vq->desc[idx].flags) & VRING_DESC_F_NEXT;

            vq->desc[idx].addr  = 0;
            vq->desc[idx].len   = 0;
            vq->desc[idx].flags = 0;
            vq->desc[idx].next  = vq->free_head; /* free-list links always native */
            vq->free_head = idx;
            freed++;

            if (!has_next)
                break;
            idx = next;
        }
    }

    vq->num_free += (uint16)freed;

    vq->last_used_idx++;

    /*
     * EVENT_IDX: write used_event = last_used_idx to request an interrupt on
     * the very next completion.  This is the correct baseline for:
     *   - The polling path (discovery, before unit tasks exist): GetBuf is
     *     called N times while used_event would otherwise stay at 0.  If
     *     last_used_idx drifts ahead of used_event, QEMU's check
     *       (used->idx - used_event - 1) < (used->idx - old_used)
     *     evaluates FALSE and the device never fires an interrupt when the
     *     IRQ path takes over — drives fail to mount.
     *   - Single-request or low-inflight workloads: fire immediately.
     *
     * VirtIOSCSI_Harvest() overrides this with last_used_idx + (occupied-1)
     * when occupied >= 2 (pipeline coalescing).  For occupied <= 1 the
     * baseline written here is already optimal.
     *
     * Spec formula: device interrupts when
     *   (new_used - used_event - 1) < (new_used - old_used)
     * With used_event = last_used_idx and new_used = last_used_idx + 1:
     *   (last+1 - last - 1) < (last+1 - last)  →  0 < 1  →  TRUE ✓
     */
    if (vq->use_event_idx) {
        vq->avail->ring[vq->num] = vr16(vq->modern, vq->last_used_idx);
        __asm__ volatile("eieio" ::: "memory");
    }

    return cookie;
}
