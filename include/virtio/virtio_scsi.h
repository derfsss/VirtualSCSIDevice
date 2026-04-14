#ifndef VIRTIO_SCSI_H
#define VIRTIO_SCSI_H

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

/*
 * VirtIO SCSI event queue (VQ1) entry — 16 bytes, device-writable.
 * Spec: each event posted by the device describes one asynchronous event
 * on a target/LUN.  All fields little-endian in modern mode; native in
 * legacy.  The device writes exactly one struct per completed buffer.
 */
struct virtio_scsi_event
{
    uint32 event;    /* VIRTIO_SCSI_T_* */
    uint8  lun[8];   /* SAM LUN: lun[0]=1, lun[1]=target_id, lun[2..3]=lun_id, rest=0 */
    uint32 reason;   /* VIRTIO_SCSI_EVT_* */
} __attribute__((packed));

/* Event type codes */
#define VIRTIO_SCSI_T_NO_EVENT          0
#define VIRTIO_SCSI_T_TRANSPORT_RESET   1
#define VIRTIO_SCSI_T_ASYNC_NOTIFY      2
#define VIRTIO_SCSI_T_PARAM_CHANGE      3

/* Event "events lost" marker bit — set by device in the event field when
 * buffers were not available and one or more events were dropped. */
#define VIRTIO_SCSI_T_EVENTS_MISSED     0x80000000

/* Reason codes for VIRTIO_SCSI_T_TRANSPORT_RESET */
#define VIRTIO_SCSI_EVT_RESET_HARD      0  /* full device reset, re-probe all */
#define VIRTIO_SCSI_EVT_RESET_RESCAN    1  /* new target/LUN appeared — probe it */
#define VIRTIO_SCSI_EVT_RESET_REMOVED   2  /* target/LUN removed — drop it */

#endif /* VIRTIO_SCSI_H */
