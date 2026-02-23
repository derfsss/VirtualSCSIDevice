#include "scsi_cdb_helpers.h"
#include "virtio/virtio_scsi_io.h"
#include "virtioscsi.h"
#include "virtioscsi_cmds.h"

/*
 * Handle_CMD_Read: Exec CMD_READ (command 2)
 *
 * io_Offset = byte offset on disk
 * io_Length = number of bytes to read
 * io_Data   = destination buffer (caller-allocated)
 *
 * We build a SCSI READ(10) CDB and dispatch via VirtIO.
 */
void Handle_CMD_Read(struct VirtIOSCSIBase *libBase, struct IOStdReq *ioreq)
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

    DPRINTF(libBase->IExec, "[virtioscsi:cmd_read.c] CMD_READ: offset=%lu lba=%lu blocks=%lu len=%lu\n", offset, lba,
            blocks, length);

    uint8 cdb[10];
    make_read10_cdb(cdb, lba, (uint16)blocks);

    uint8 scsi_status = 0;
    uint32 residual = 0;

    int32 rc =
        VirtIOSCSI_DoIO(libBase, unit, unit->target_id, unit->lun_id, cdb, 10, data, length, FALSE, &scsi_status, &residual);

    if (rc != 0) {
        ioreq->io_Error = (BYTE)rc;
        ioreq->io_Actual = 0;
    } else {
        ioreq->io_Error = 0;
        ioreq->io_Actual = length - residual;

        /* Diagnostic: dump first 16 bytes of read data */
        if (length >= 16) {
            DPRINTF(libBase->IExec,
                    "[virtioscsi] DATA[0..15]: "
                    "%02lX %02lX %02lX %02lX %02lX %02lX %02lX %02lX "
                    "%02lX %02lX %02lX %02lX %02lX %02lX %02lX %02lX\n",
                    (uint32)data[0], (uint32)data[1], (uint32)data[2], (uint32)data[3], (uint32)data[4],
                    (uint32)data[5], (uint32)data[6], (uint32)data[7], (uint32)data[8], (uint32)data[9],
                    (uint32)data[10], (uint32)data[11], (uint32)data[12], (uint32)data[13], (uint32)data[14],
                    (uint32)data[15]);
        }
    }
}
