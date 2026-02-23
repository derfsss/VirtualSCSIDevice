#include "scsi_cdb_helpers.h"
#include "virtio/virtio_scsi_io.h"
#include "virtioscsi.h"
#include "virtioscsi_cmds.h"

/*
 * Handle_CMD_Write: Exec CMD_WRITE (command 3)
 *
 * io_Offset = byte offset on disk
 * io_Length = number of bytes to write
 * io_Data   = source buffer (caller-allocated)
 *
 * We build a SCSI WRITE(10) CDB and dispatch via VirtIO.
 */
void Handle_CMD_Write(struct VirtIOSCSIBase *libBase, struct IOStdReq *ioreq)
{
    uint32 offset = ioreq->io_Offset;
    uint32 length = ioreq->io_Length;
    UBYTE *data = (UBYTE *)ioreq->io_Data;

    if (!data || length == 0) {
        ioreq->io_Error = IOERR_BADADDRESS;
        return;
    }

    struct VirtIOUSCSIDevUnit *unit = (struct VirtIOUSCSIDevUnit *)ioreq->io_Unit;
    uint32 blksz = (unit && unit->geometry_valid && unit->block_size) ? unit->block_size : 512;

    uint32 lba = offset / blksz;
    uint32 blocks = length / blksz;
    if (blocks == 0)
        blocks = 1;

    DPRINTF(libBase->IExec, "[virtioscsi:cmd_write.c] CMD_WRITE: offset=%lu lba=%lu blocks=%lu len=%lu\n", offset, lba,
            blocks, length);

    uint8 cdb[10];
    make_write10_cdb(cdb, lba, (uint16)blocks);

    uint8 scsi_status = 0;
    uint32 residual = 0;

    int32 rc =
        VirtIOSCSI_DoIO(libBase, unit, unit->target_id, unit->lun_id, cdb, 10, data, length, TRUE, &scsi_status, &residual);

    if (rc != 0) {
        ioreq->io_Error = (BYTE)rc;
        ioreq->io_Actual = 0;
    } else {
        ioreq->io_Error = 0;
        ioreq->io_Actual = length - residual;
    }
}
