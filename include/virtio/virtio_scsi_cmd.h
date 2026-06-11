#ifndef VIRTIO_SCSI_CMD_H
#define VIRTIO_SCSI_CMD_H

#include <exec/types.h>

/*
 * VirtIO SCSI Command Structures (Spec 5.6.6.1)
 *
 * Endianness:
 *   Legacy mode: ALL fields are native guest endian (Big Endian on PPC) --
 *   no byte-swapping anywhere.
 *   Modern (VIRTIO_F_VERSION_1) mode: multi-byte fields are LITTLE-endian
 *   ("this is unlike legacy interface, where fields were guest-endian").
 *   Single-byte fields (status, response, task_attr, ...) and byte arrays
 *   (lun, cdb, sense) need no conversion.  The uint64 `id` is a driver
 *   cookie the device never interprets, so it round-trips unchanged.
 *   Device-written multi-byte fields the driver actually reads (residual)
 *   must go through virtio_scsi_resp_residual() below.
 */

/* Default sizes from device config (spec 5.6.4.2) */
#define VIRTIO_SCSI_CDB_SIZE 32
#define VIRTIO_SCSI_SENSE_SIZE 96

/* Response codes (spec 5.6.6.1.1) */
#define VIRTIO_SCSI_S_OK 0
#define VIRTIO_SCSI_S_OVERRUN 1
#define VIRTIO_SCSI_S_ABORTED 2
#define VIRTIO_SCSI_S_BAD_TARGET 3
#define VIRTIO_SCSI_S_RESET 4
#define VIRTIO_SCSI_S_BUSY 5
#define VIRTIO_SCSI_S_TRANSPORT_FAILURE 6
#define VIRTIO_SCSI_S_TARGET_FAILURE 7
#define VIRTIO_SCSI_S_NEXUS_FAILURE 8
#define VIRTIO_SCSI_S_FAILURE 9

/* SAM-2 LUN address method: single-level LUN format (byte 2 bits 7:6 = 01b) */
#define SAM2_SINGLE_LEVEL_LUN 0x40

/* Task attributes */
#define VIRTIO_SCSI_S_SIMPLE 0
#define VIRTIO_SCSI_S_ORDERED 1
#define VIRTIO_SCSI_S_HEAD 2
#define VIRTIO_SCSI_S_ACA 3

/*
 * Request header (device-readable part of the descriptor chain).
 * Size: 8 + 8 + 1 + 1 + 1 + 32 = 51 bytes
 */
struct virtio_scsi_req_cmd
{
    uint8 lun[8];
    uint64 id;
    uint8 task_attr;
    uint8 prio;
    uint8 crn;
    uint8 cdb[VIRTIO_SCSI_CDB_SIZE];
} __attribute__((packed));

/*
 * Response header (device-writable part of the descriptor chain).
 * Size: 4 + 4 + 2 + 1 + 1 + 96 = 108 bytes
 */
struct virtio_scsi_resp_cmd
{
    uint32 sense_len;
    uint32 residual;
    uint16 status_qualifier;
    uint8 status;   /* SCSI status byte (0x00 = GOOD) */
    uint8 response; /* VirtIO response (VIRTIO_SCSI_S_OK = 0) */
    uint8 sense[VIRTIO_SCSI_SENSE_SIZE];
} __attribute__((packed));

/*
 * Read the device-written residual count in native byte order.
 *
 * `residual` is a uint32 the device writes little-endian in modern mode
 * and guest-endian (BE) in legacy mode.  Reading it raw on the modern
 * path byte-swaps the value: a benign 0 stays 0 (why this went unnoticed
 * -- full transfers have no residual), but any genuine underrun would
 * produce a garbage io_Actual.
 */
static inline uint32 virtio_scsi_resp_residual(BOOL modern,
                                               const struct virtio_scsi_resp_cmd *resp)
{
    uint32 v = resp->residual;
    if (modern) {
        v = ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
            ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
    }
    return v;
}

/*
 * Helper to fill the LUN field in SAM-2 format.
 * Byte 0 = 1, Byte 1 = target, Bytes 2-3 = single-level LUN, rest = 0.
 */
static inline void virtio_scsi_set_lun(uint8 lun[8], uint8 target, uint16 lun_id)
{
    lun[0] = 1;
    lun[1] = target;
    lun[2] = (lun_id >> 8) | SAM2_SINGLE_LEVEL_LUN; /* SAM-2 single-level LUN address method */
    lun[3] = lun_id & 0xFF;
    lun[4] = 0;
    lun[5] = 0;
    lun[6] = 0;
    lun[7] = 0;
}

#endif /* VIRTIO_SCSI_CMD_H */
