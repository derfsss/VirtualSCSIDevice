#include "scsi_cdb_helpers.h"
#include "virtio/virtio_scsi_io.h"
#include "virtioscsi.h"
#include "virtioscsi_cmds.h"

/*
 * Handle_NS_TD_IO64: NSCMD_TD_READ64 / NSCMD_TD_WRITE64 / NSCMD_TD_FORMAT64
 *
 * 64-bit offset: upper 32 in io_Actual, lower 32 in io_Offset
 * Dispatches to VirtIO via SCSI READ(10)/WRITE(10).
 */
void Handle_NS_TD_IO64(struct VirtIOSCSIBase *libBase, struct IOStdReq *req)
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

    uint64 lba = offset64 / blksz;
    uint32 blocks = length / blksz;
    if (blocks == 0)
        blocks = 1;

    BOOL is_write = (req->io_Command != NSCMD_TD_READ64);

    uint8 scsi_status = 0;
    uint32 residual = 0;
    int32 rc;

    if (lba > 0xFFFFFFFFULL) {
        /* Disk > 2TB: use READ(16)/WRITE(16) with 64-bit LBA */
        uint8 cdb[16];
        if (is_write)
            make_write16_cdb(cdb, lba, blocks);
        else
            make_read16_cdb(cdb, lba, blocks);
        rc = VirtIOSCSI_DoIO(libBase, unit, unit->target_id, unit->lun_id, cdb, 16, data, length, is_write, &scsi_status,
                             &residual);
    } else {
        uint8 cdb[10];
        if (is_write)
            make_write10_cdb(cdb, (uint32)lba, (uint16)blocks);
        else
            make_read10_cdb(cdb, (uint32)lba, (uint16)blocks);
        rc = VirtIOSCSI_DoIO(libBase, unit, unit->target_id, unit->lun_id, cdb, 10, data, length, is_write, &scsi_status,
                             &residual);
    }

    if (rc != 0) { /* rc declared above */
        req->io_Error = (BYTE)rc;
        req->io_Actual = 0;
    } else {
        req->io_Error = 0;
        req->io_Actual = length - residual;
    }
}
