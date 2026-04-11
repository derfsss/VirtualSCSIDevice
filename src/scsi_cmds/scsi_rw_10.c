#include "virtio/virtio_scsi_io.h"
#include "virtioscsi.h"
#include "virtioscsi_cmds.h"

/*
 * Shared SCSI READ(10) / WRITE(10) direct command handler.
 * These are HD_SCSICMD pass-through — the CDB is already built by the caller.
 */
static void handle_scsi_rw10(struct VirtIOSCSIBase *libBase, struct IOStdReq *req,
                             struct SCSICmd *scsiCmd, BOOL is_write)
{
    UBYTE *cmd = (UBYTE *)scsiCmd->scsi_Command;
    uint32 lba = (cmd[2] << 24) | (cmd[3] << 16) | (cmd[4] << 8) | cmd[5];
    uint16 transfer_blocks = (cmd[7] << 8) | cmd[8];

    DPRINTF(libBase->IExec, "[virtioscsi:scsi_rw_10.c] SCSI %s (10) - LBA: %lu, Blocks: %u\n",
            is_write ? "WRITE" : "READ", lba, transfer_blocks);

    uint8 scsi_status = 0;
    uint32 residual = 0;
    struct VirtIOUSCSIDevUnit *unit = (struct VirtIOUSCSIDevUnit *)req->io_Unit;

    int32 rc = VirtIOSCSI_DoIO(libBase, unit, unit->target_id, unit->lun_id,
                               scsiCmd->scsi_Command, scsiCmd->scsi_CmdLength,
                               (uint8 *)scsiCmd->scsi_Data, scsiCmd->scsi_Length,
                               is_write, &scsi_status, &residual);

    scsiCmd->scsi_Status = scsi_status;
    scsiCmd->scsi_Actual = scsiCmd->scsi_Length - residual;
    req->io_Actual = scsiCmd->scsi_Actual;

    if (rc != 0) {
        req->io_Error = (BYTE)rc;
        return;
    }

    req->io_Error = 0;
}

void Handle_SCSI_Read10(struct VirtIOSCSIBase *libBase, struct IOStdReq *req, struct SCSICmd *scsiCmd)
{
    handle_scsi_rw10(libBase, req, scsiCmd, FALSE);
}

void Handle_SCSI_Write10(struct VirtIOSCSIBase *libBase, struct IOStdReq *req, struct SCSICmd *scsiCmd)
{
    handle_scsi_rw10(libBase, req, scsiCmd, TRUE);
}
