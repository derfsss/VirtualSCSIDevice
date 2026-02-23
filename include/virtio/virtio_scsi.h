#ifndef VIRTIO_VIRTIO_SCSI_H
#define VIRTIO_VIRTIO_SCSI_H

#include <exec/types.h>

/* VirtIO v1.0 Legacy PCI Register Offsets (relative to BAR0 Base I/O) */
#define VIRTIO_PCI_HOST_FEATURES 0x00
#define VIRTIO_PCI_GUEST_FEATURES 0x04
#define VIRTIO_PCI_QUEUE_PFN 0x08
#define VIRTIO_PCI_QUEUE_NUM 0x0C
#define VIRTIO_PCI_QUEUE_SEL 0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY 0x10
#define VIRTIO_PCI_STATUS 0x12
#define VIRTIO_PCI_ISR 0x13

/* VirtIO SCSI Device Specific Configuration (Legacy PCI, no MSI-X) */
#define VIRTIO_SCSI_CONFIG_OFFSET 0x14

/* VirtIO Device Status Bits */
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 64
#define VIRTIO_STATUS_FAILED 128

/* VirtIO Feature Bits */
#define VIRTIO_F_VERSION_1 32

/* VirtIO SCSI Specific feature bits (offsets 0-31) */
#define VIRTIO_SCSI_F_INOUT 0
#define VIRTIO_SCSI_F_HOTPLUG 1
#define VIRTIO_SCSI_F_CHANGE 2
#define VIRTIO_SCSI_F_T10_PI 3

#endif /* VIRTIO_VIRTIO_SCSI_H */
