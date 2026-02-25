# VirtIO Modern PCI Interface Specification

> **Platform note**: Modern VirtIO (device ID 0x1048) requires transparent CPU↔PCI MMIO
> access. This works on **Pegasos2** (MV64361 bridge) but **NOT on AmigaOne** (Articia S
> floating buffer). See Section 7 for full compatibility details.

This document extracts the relevant information from the OASIS VirtIO v1.3 specification for implementing a VirtIO SCSI device using the **Modern PCI** transport on a Big Endian PowerPC AmigaOS 4.1 system.

## 1. Device Discovery
- **PCI Vendor ID:** `0x1AF4`
- **PCI Device ID (Non-Transitional):** `0x1040` to `0x107F`. To calculate the PCI Device ID, add `0x1040` to the VirtIO Device ID. For SCSI (Device ID 8), the PCI Device ID is `0x1048`.
- **PCI Revision ID:** `1` or higher.
- **Subsystem Device ID:** `0x40` or higher.

## 2. Virtio Structure PCI Capabilities
Unlike the legacy interface (which hardcodes offsets in BAR 0), the Modern PCI interface uses PCI capabilities to dynamically map structures into specific BARs and offsets.

The driver must scan the PCI capability list for Vendor Specific capabilities (ID `0x09`) and parse the `cfg_type` field:

- `VIRTIO_PCI_CAP_COMMON_CFG (1)`: Points to the Common Configuration Structure.
- `VIRTIO_PCI_CAP_NOTIFY_CFG (2)`: Points to the Notification Structure.
- `VIRTIO_PCI_CAP_ISR_CFG (3)`: Points to the ISR Status Structure.
- `VIRTIO_PCI_CAP_DEVICE_CFG (4)`: Points to the Device Specific Configuration Structure.
- `VIRTIO_PCI_CAP_PCI_CFG (5)`: PCI Configuration Access Structure.

### Cap Structure:
* `cap_vndr` (byte): `0x09`
* `cap_next` (byte)
* `cap_len` (byte)
* `cfg_type` (byte)
* `bar` (byte)
* `offset` (32-bit LE)
* `length` (32-bit LE)

## 3. Common Configuration Structure Layout
Once located via `VIRTIO_PCI_CAP_COMMON_CFG`, all fields are strictly **Little Endian**.
- `device_feature_select` (32-bit W)
- `device_feature` (32-bit R)
- `driver_feature_select` (32-bit W)
- `driver_feature` (32-bit W)
- `config_msix_vector` (16-bit RW)
- `num_queues` (16-bit R)
- `device_status` (8-bit RW)
- `config_generation` (8-bit R)
- `queue_select` (16-bit W)
- `queue_size` (16-bit RW)
- `queue_msix_vector` (16-bit RW)
- `queue_enable` (16-bit RW)
- `queue_notify_off` (16-bit R)
- `queue_desc` (64-bit W)
- `queue_driver` (64-bit W)
- `queue_device` (64-bit W)

## 4. Endianness
- **All Configuration Structures:** Strict Little-Endian. Must byte-swap 16-bit, 32-bit, and 64-bit fields on PowerPC.
- **Device-Specific Configuration:** Strict Little-Endian. Must byte-swap all SCSI specific bounds limits!
- **VirtQueues:** Strict Little-Endian.

## 5. Queue Notification
To notify the device, the driver writes the queue index to the Queue Notify address.
The address is calculated using the `VIRTIO_PCI_CAP_NOTIFY_CFG` capability:
`Queue Notify Address = cap.offset + (queue_notify_off * notify_off_multiplier)`

## 6. Initialization Sequence Differences
1. Parse PCI Capabilities to find all structures.
2. Read/Write 64-bit feature bits using `device_feature_select`. Modern mode requires negotiating `VIRTIO_F_VERSION_1` (Bit 32).
3. Queue memory uses 64-bit pointers (`queue_desc`, `queue_driver`, `queue_device`) instead of a single Shared 32-bit PFN.
4. Set `queue_enable` to `1`.

Full status sequence (validated on Pegasos2/QEMU Feb 2026):
```
1. Write STATUS=0x00  (reset)
2. Write STATUS=0x01  (ACKNOWLEDGE)
3. Write STATUS=0x03  (ACKNOWLEDGE | DRIVER)
4. Write DFSELECT=0, read DF (feature lo)
5. Write DFSELECT=1, read DF (feature hi)
6. Write DFSELECTG=0, write DFG (accepted lo; include INDIRECT_DESC, EVENT_IDX, SCSI features)
7. Write DFSELECTG=1, write DFG (accepted hi; MUST include VERSION_1)
8. Write STATUS=0x0B  (ACKNOWLEDGE | DRIVER | FEATURES_OK)
9. Read STATUS back — must have bit 0x08 set; if not, abort
10. Write QUEUE_SELECT, read QUEUE_SIZE per queue
11. Allocate vring (MEMF_SHARED, page-aligned), DMA-map, write QUEUE_DESC/AVAIL/USED low/high
12. Write QUEUE_ENABLE=1 per queue
13. Write STATUS=0x0F  (add DRIVER_OK)
```

QEMU Pegasos2 confirmed feature advertisement (Feb 2026):
- Features lo = 0x30000006: INDIRECT_DESC (28), EVENT_IDX (29), HOTPLUG (1), CHANGE (2)
- Features hi = 0x00000101: VERSION_1 (32), RING_RESET (40)
- Note: VIRTIO_SCSI_F_INOUT (bit 0) NOT offered — per spec 5.6.6.1.1 this means
  commands with simultaneous IN+OUT buffers will fail with VIRTIO_SCSI_S_FAILURE.
  Standard READ/WRITE SCSI commands (OUT header + IN data, or OUT header + OUT data)
  are unidirectional and work fine.

## 7. AmigaOS 4.1 MMIO Access Constraints

### The MMIO Access Problem

On AmigaOS 4.1 FE, `PCIDevice->InWord()`, `InLong()`, `OutWord()`, `OutLong()` silently
return 0 when called with memory BAR physical addresses. Only `InByte()`/`OutByte()`
issue real PCI memory cycles.

`SetEndian(PCI_MODE_LITTLE_ENDIAN)` does NOT affect memory BAR access — it only changes
byte-swapping for I/O port operations.

### Correct MMIO Access Methods

**Option A — byte-assembly helpers** (portable, both AmigaOne and Pegasos2):
```c
static inline uint32 mmio_r32(struct PCIDevice *dev, uint32 base, uint32 off) {
    return (uint32)dev->InByte(base+off+0)        |
           (uint32)dev->InByte(base+off+1) << 8   |
           (uint32)dev->InByte(base+off+2) << 16  |
           (uint32)dev->InByte(base+off+3) << 24;  /* LE assembly */
}
static inline void mmio_w32(struct PCIDevice *dev, uint32 base, uint32 off, uint32 v) {
    dev->OutByte(base+off+0, (uint8)(v      ));
    dev->OutByte(base+off+1, (uint8)(v >>  8));
    dev->OutByte(base+off+2, (uint8)(v >> 16));
    dev->OutByte(base+off+3, (uint8)(v >> 24));
}
```

**Option B — stwbrx/lwbrx inline assembly** (Pegasos2 only, zero overhead):
```c
#define MMIO_W32(addr, val) do { \
    uint32 _v=(val); uint32 *_a=(uint32*)(addr); \
    __asm__ volatile("stwbrx %1,0,%0; mbar"::"r"(_a),"r"(_v):"memory"); } while(0)
#define MMIO_R32(addr) __extension__({ \
    uint32 _r; uint32 *_a=(uint32*)(addr); \
    __asm__ volatile("lwbrx %0,0,%1":"=r"(_r):"r"(_a):"memory"); _r; })
```
`stwbrx`/`lwbrx` are store/load word byte-reversed — canonical PPC MMIO with
built-in LE↔BE conversion. `mbar` is the required memory barrier after MMIO writes.
`lwbrx` does not need a barrier before the load (the CPU issues the read and blocks).

### Platform Compatibility Matrix

| Machine | MMIO works? | Method |
|---------|-------------|--------|
| Pegasos2 (MV64361) | ✓ Yes | stwbrx/lwbrx OR InByte helpers |
| AmigaOne (Articia S) | ✗ No | No workaround — hardware limitation |

**AmigaOne recommendation**: Use legacy/transitional VirtIO (device ID 0x1004) with
I/O BAR0 and `pciDev->InByte/OutByte(iobase + offset)`. The legacy driver fully
works and is the correct solution for AmigaOne.
