# VirtIO Legacy PCI Interface Specification

This document extracts the relevant information from the OASIS VirtIO v1.3 specification for implementing a VirtIO SCSI device using the **Legacy PCI** transport on a Big Endian PowerPC AmigaOS 4.1 system.

## 1. Device Discovery
- **PCI Vendor ID:** `0x1AF4`
- **PCI Device ID (Transitional):** `0x1000` to `0x103F` (SCSI is typically `0x1004` in legacy mode).
- **PCI Revision ID:** `0x00`.
- **Subsystem Device ID:** Matches the VirtIO Device ID (`8` for SCSI).
- **BAR 0:** The Legacy Interface is exposed in the **first I/O region** (BAR 0) of the PCI device.

## 2. Legacy PCI Configuration Space (BAR 0)
The legacy configuration structure is mapped to BAR 0. It is composed of a 20-byte (or 24-byte with MSI-X) header, followed immediately by device-specific configuration space.

**Offset Layout (without MSI-X):**
* `0x00` (32-bit R): Device Features
* `0x04` (32-bit W): Guest Features
* `0x08` (32-bit W): Queue Address (Page Frame Number, physical address / 4096)
* `0x0C` (16-bit R): Queue Size (Maximum size of the queue)
* `0x0E` (16-bit W): Queue Select
* `0x10` (16-bit W): Queue Notify
* `0x12` (8-bit RW): Device Status
* `0x13` (8-bit R): ISR Status

*Note: If MSI-X is enabled, two additional 16-bit fields (`config_msix_vector` and `queue_msix_vector`) exist, shifting the device-specific config by 4 bytes to `offset 0x18`. It is recommended to keep MSI-X disabled for AmigaOS 4.*

## 3. Endianness
- **Common Configuration (0x00 to 0x13):** Little-Endian format. Since AmigaOS 4.1 on PowerPC is Big Endian, we **must byte-swap** 16-bit and 32-bit reads/writes.
- **Device-Specific Configuration (SCSI):** In Legacy mode, the device-specific configuration space is encoded in the **"native endian"** of the guest. Thus, NO byte-swapping should be done for SCSI config writes in legacy mode!
- **VirtQueues (in Memory):** The memory structures (Descriptors, Available Ring, Used Ring) must be strictly **Little-Endian**.

## 4. SCSI Device Configuration (offset `0x14` in BAR 0)
When using the Legacy Interface, transitional drivers must access the device-specific configuration space at an offset immediately following the general headers (Offset `0x14` when MSI-X is disabled).

**Device Specific Config for VirtIO SCSI:**
- `num_queues` (32-bit)
- `seg_max` (32-bit)
- `max_sectors` (32-bit)
- `cmd_per_lun` (32-bit)
- `event_info_size` (32-bit)
- `sense_size` (32-bit)
- `cdb_size` (32-bit)
- `max_channel` (16-bit)
- `max_target` (16-bit)
- `max_lun` (32-bit)

*(Remember: In legacy mode, this struct is native Big Endian!)*

## 5. Initialization Sequence (Verified Working)
1. Discover PCI device (Vendor 0x1AF4, Device 0x1004).
2. Get BAR 0 physical address via `GetResourceRange(0)`.
3. **Use `PCIDevice->OutByte/InByte` methods** — NOT direct pointer dereference (BAR 0 is I/O space).
4. Reset Device: `OutByte(bar0 + 0x12, 0x00)`.
5. Clear ISR: `InByte(bar0 + 0x13)`.
6. Write `ACKNOWLEDGE`: `OutByte(bar0 + 0x12, 0x01)`.
7. Write `ACKNOWLEDGE | DRIVER`: `OutByte(bar0 + 0x12, 0x03)`.
8. Read Device Features: `InLong(bar0 + 0x00)`. Write Guest Features: `OutLong(bar0 + 0x04, accepted)`. *(Legacy: bits 0-31 only, NO FEATURES_OK step)*.
9. Queue Setup for each queue (0: control, 1: event, 2: request):
   - `OutWord(bar0 + 0x0E, queue_index)` — Queue Select.
   - `InWord(bar0 + 0x0C)` — Read Queue Size.
   - Allocate page-aligned memory (over-allocate + manual align).
   - `OutLong(bar0 + 0x08, physical_addr / 4096)` — Write PFN.
10. Write `ACKNOWLEDGE | DRIVER | DRIVER_OK`: `OutByte(bar0 + 0x12, 0x07)`.

## 6. Proven Host Features
QEMU reports `0x79000006`:
- Bit 1: `VIRTIO_SCSI_F_HOTPLUG`
- Bit 2: `VIRTIO_SCSI_F_CHANGE`
- Bits 24, 27, 28, 29, 30: Various reserved/transport features
