#include "scsi_cdb_helpers.h"
#include "virtio/virtio_scsi_io.h"
#include "virtioscsi.h"
#include "virtioscsi_cmds.h"

/*
 * Handle_TD_IO64: TD_READ64 (24) / TD_WRITE64 (25)
 *
 * Legacy 64-bit offset commands. Same encoding as NSCMD variants:
 * io_Actual = upper 32 bits, io_Offset = lower 32 bits.
 */
void Handle_TD_IO64(struct VirtIOSCSIBase *libBase, struct IOStdReq *req)
{
    uint64 offset64 = unpack_io64_offset(req->io_Actual, req->io_Offset);
    uint32 length = req->io_Length;
    UBYTE *data = (UBYTE *)req->io_Data;

    if (!data || length == 0) {
        req->io_Error = IOERR_BADADDRESS;
        return;
    }

    struct VirtIOUSCSIDevUnit *unit = (struct VirtIOUSCSIDevUnit *)req->io_Unit;
    uint32 blksz = (unit && unit->geometry_valid && unit->block_size) ? unit->block_size : 512;

    uint32 lba = (uint32)(offset64 / blksz);
    uint32 blocks = length / blksz;
    if (blocks == 0)
        blocks = 1;

    BOOL is_write = (req->io_Command == TD_WRITE64 || req->io_Command == (TD_FORMAT64));

    uint8 cdb[10];
    if (is_write)
        make_write10_cdb(cdb, lba, (uint16)blocks);
    else
        make_read10_cdb(cdb, lba, (uint16)blocks);

    uint8 scsi_status = 0;
    uint32 residual = 0;

    int32 rc = VirtIOSCSI_DoIO(libBase, unit, unit->target_id, unit->lun_id, cdb, 10, data, length, is_write, &scsi_status,
                               &residual);

    if (rc != 0) {
        req->io_Error = (BYTE)rc;
        req->io_Actual = 0;
    } else {
        req->io_Error = 0;
        req->io_Actual = length - residual;
    }
}
