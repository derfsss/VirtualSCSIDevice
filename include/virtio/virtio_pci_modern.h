#ifndef VIRTIO_PCI_MODERN_H
#define VIRTIO_PCI_MODERN_H

/*
 * virtio_pci_modern.h — VirtIO 1.0 Modern PCI Transport Definitions
 *
 * Modern VirtIO devices advertise their configuration regions via vendor-
 * specific PCI capabilities (type 0x09).  Each capability describes one
 * config region (COMMON, NOTIFY, ISR, or DEVICE) mapped within a BAR.
 *
 * Reference: VirtIO v1.0 spec, Section 4.1.4
 */

/* VirtIO vendor-specific PCI capability cfg_type values */
#define VIRTIO_PCI_CAP_COMMON_CFG  1  /* Common configuration */
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2  /* Notifications */
#define VIRTIO_PCI_CAP_ISR_CFG     3  /* ISR status */
#define VIRTIO_PCI_CAP_DEVICE_CFG  4  /* Device-specific configuration */
#define VIRTIO_PCI_CAP_PCI_CFG     5  /* PCI configuration access */

/*
 * Byte offsets within a VirtIO vendor-specific PCI capability structure.
 *
 * Capability layout (from CapOffset):
 *   +0  cap_vndr   (uint8)  = 0x09 (vendor-specific)
 *   +1  cap_next   (uint8)  next cap offset
 *   +2  cap_len    (uint8)  length of this cap structure
 *   +3  cfg_type   (uint8)  which config region (VIRTIO_PCI_CAP_*)
 *   +4  bar        (uint8)  which BAR contains the region
 *   +5  padding[3] (uint8)
 *   +8  offset     (uint32) byte offset within the BAR
 *  +12  length     (uint32) length of the region in bytes
 *
 * For NOTIFY_CFG only:
 *  +16  notify_off_multiplier (uint32)
 *       Per-queue notify address = notify_cfg_base + queue_notify_off * multiplier
 *       If multiplier == 0, all queues use the same address (notify_cfg_base).
 */
#define VIRTIO_CAP_OFF_CFG_TYPE  3
#define VIRTIO_CAP_OFF_BAR       4
#define VIRTIO_CAP_OFF_OFFSET    8
#define VIRTIO_CAP_OFF_LENGTH   12
#define VIRTIO_CAP_OFF_NOTIFY_MULT 16  /* NOTIFY_CFG only */

/*
 * MMIO access helpers for Modern VirtIO Common Configuration.
 *
 * Testing shows that on AmigaOS 4.1 / QEMU, pciDev->InWord() and InLong()
 * return 0 for MMIO (memory BAR) addresses — only InByte()/OutByte() work
 * correctly for MMIO.  All multi-byte register accesses must be assembled
 * from individual byte reads/writes in little-endian order.
 *
 * Usage: pass pciDev pointer and the absolute physical register address.
 */
#include <interfaces/expansion.h>

static inline uint16 mmio_r16(struct PCIDevice *d, uint32 addr)
{
    return (uint16)d->InByte(addr) |
           ((uint16)d->InByte(addr + 1) << 8);
}

static inline uint32 mmio_r32(struct PCIDevice *d, uint32 addr)
{
    return (uint32)d->InByte(addr)       |
           ((uint32)d->InByte(addr + 1) << 8)  |
           ((uint32)d->InByte(addr + 2) << 16) |
           ((uint32)d->InByte(addr + 3) << 24);
}

static inline void mmio_w16(struct PCIDevice *d, uint32 addr, uint16 v)
{
    d->OutByte(addr,     (uint8)(v));
    d->OutByte(addr + 1, (uint8)(v >> 8));
}

static inline void mmio_w32(struct PCIDevice *d, uint32 addr, uint32 v)
{
    d->OutByte(addr,     (uint8)(v));
    d->OutByte(addr + 1, (uint8)(v >> 8));
    d->OutByte(addr + 2, (uint8)(v >> 16));
    d->OutByte(addr + 3, (uint8)(v >> 24));
}

/*
 * Modern VirtIO Common Configuration register offsets (Section 4.1.4.3).
 * All registers are little-endian; use mmio_r16/mmio_r32/mmio_w16/mmio_w32 helpers above.
 */
#define VIRTIO_PCI_COMMON_DFSELECT  0x00  /* uint32 rw: device feature word select */
#define VIRTIO_PCI_COMMON_DF        0x04  /* uint32 r:  device feature bits */
#define VIRTIO_PCI_COMMON_DFSELECTG 0x08  /* uint32 rw: driver feature word select (guest) */
#define VIRTIO_PCI_COMMON_DFG       0x0C  /* uint32 rw: driver feature bits (guest) */
#define VIRTIO_PCI_COMMON_MSIX_CFG  0x10  /* uint16 rw: config vector (MSI-X) */
#define VIRTIO_PCI_COMMON_NUMQ      0x12  /* uint16 r:  number of virtqueues */
#define VIRTIO_PCI_COMMON_STATUS    0x14  /* uint8  rw: device status */
#define VIRTIO_PCI_COMMON_CFGGEN    0x15  /* uint8  r:  config generation */
#define VIRTIO_PCI_COMMON_Q_SELECT  0x16  /* uint16 rw: queue select */
#define VIRTIO_PCI_COMMON_Q_SIZE    0x18  /* uint16 rw: queue size */
#define VIRTIO_PCI_COMMON_Q_MSIX    0x1A  /* uint16 rw: queue MSI-X vector */
#define VIRTIO_PCI_COMMON_Q_ENABLE  0x1C  /* uint16 rw: queue enable */
#define VIRTIO_PCI_COMMON_Q_NOFF    0x1E  /* uint16 r:  queue notify offset */
#define VIRTIO_PCI_COMMON_Q_DESCLO  0x20  /* uint32 rw: descriptor area low 32 bits */
#define VIRTIO_PCI_COMMON_Q_DESCHI  0x24  /* uint32 rw: descriptor area high 32 bits */
#define VIRTIO_PCI_COMMON_Q_AVAILLO 0x28  /* uint32 rw: driver area (avail ring) low */
#define VIRTIO_PCI_COMMON_Q_AVAILHI 0x2C  /* uint32 rw: driver area high */
#define VIRTIO_PCI_COMMON_Q_USEDLO  0x30  /* uint32 rw: device area (used ring) low */
#define VIRTIO_PCI_COMMON_Q_USEDHI  0x34  /* uint32 rw: device area high */

#endif /* VIRTIO_PCI_MODERN_H */
