#ifndef VIRTIO_SCSI_IO_H
#define VIRTIO_SCSI_IO_H

#include <exec/io.h>
#include <exec/types.h>

struct VirtIOSCSIBase;
struct VirtIOUSCSIDevUnit;

/*
 * Execute a SCSI command through the VirtIO request queue.
 * Synchronous: submits one request and blocks (interrupt or polling) until done.
 * Used for SCSI pass-through (HD_SCSICMD), discovery, and geometry queries.
 *
 * unit:               The unit making the request (NULL during discovery)
 * scsi_status_out:    Receives the SCSI status byte (0=GOOD)
 * residual_out:       Receives the residual byte count
 *
 * Returns 0 on success, non-zero AmigaOS error code on failure.
 */
int32 VirtIOSCSI_DoIO(struct VirtIOSCSIBase *libBase, struct VirtIOUSCSIDevUnit *unit,
                      uint32 target, uint32 lun, uint8 *cdb, uint32 cdb_len,
                      uint8 *data, uint32 data_len, BOOL is_write,
                      uint8 *scsi_status_out, uint32 *residual_out);

/*
 * Submit a block I/O request into a pipeline inflight slot (non-blocking).
 * Finds a free inflight slot, DMA-maps the user buffer, builds the descriptor
 * chain, calls AddBuf+Kick, and returns immediately. The slot is marked
 * occupied; VirtIOSCSI_Harvest() will ReplyMsg when the device completes it.
 *
 * The ioreq must have io_Unit, io_Data, io_Length, io_Offset set.
 * is_write: TRUE = CMD_WRITE path, FALSE = CMD_READ path.
 * cdb/cdb_len: the SCSI CDB to send.
 *
 * Returns 0 on success (slot acquired and submitted), -1 if no free slot
 * (caller should queue the ioreq for retry), or a positive error code on
 * hard failure (DMA setup, AddBuf failure — set io_Error and ReplyMsg).
 */
int32 VirtIOSCSI_Submit(struct VirtIOSCSIBase *libBase, struct VirtIOUSCSIDevUnit *unit,
                        struct IOStdReq *ioreq, uint8 *cdb, uint32 cdb_len, BOOL is_write);

/*
 * Harvest completed VirtIO responses from the used ring.
 * For each completion, finds the matching inflight slot, sets io_Error/io_Actual,
 * cleans up DMA, clears the slot, and calls ReplyMsg.
 * Called by the unit task after waking from the ISR signal.
 */
void VirtIOSCSI_Harvest(struct VirtIOSCSIBase *libBase, struct VirtIOUSCSIDevUnit *unit);

#endif /* VIRTIO_SCSI_IO_H */
