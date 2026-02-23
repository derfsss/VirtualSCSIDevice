# VirtIO Modern PCI Interface Specification

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
