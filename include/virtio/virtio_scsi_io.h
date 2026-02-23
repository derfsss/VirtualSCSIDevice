#ifndef VIRTIO_SCSI_IO_H
#define VIRTIO_SCSI_IO_H

#include <exec/types.h>

struct VirtIOSCSIBase;
struct VirtIOUSCSIDevUnit;

/*
 * Execute a SCSI command through the VirtIO request queue.
 * Called from the unit device task — sleeping via Wait/Signal (interrupt path)
 * or polling fallback.
 *
 * unit:               The unit making the request (for per-unit signal tracking)
 * cdb/cdb_len:        SCSI CDB to send
 * data/data_len:      Data buffer (read into or write from)
 * is_write:           TRUE=data goes to device, FALSE=data comes from device
 * scsi_status_out:    Receives the SCSI status byte (0=GOOD)
 * residual_out:       Receives the residual byte count
 *
 * Returns 0 on success, non-zero AmigaOS error code on failure.
 */
int32 VirtIOSCSI_DoIO(struct VirtIOSCSIBase *libBase, struct VirtIOUSCSIDevUnit *unit,
                      uint32 target, uint32 lun, uint8 *cdb, uint32 cdb_len,
                      uint8 *data, uint32 data_len, BOOL is_write,
                      uint8 *scsi_status_out, uint32 *residual_out);

#endif /* VIRTIO_SCSI_IO_H */
