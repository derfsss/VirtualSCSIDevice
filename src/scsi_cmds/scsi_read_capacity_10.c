#include "virtio/virtio_scsi_io.h"
#include "virtioscsi.h"
#include "virtioscsi_cmds.h"

void Handle_SCSI_ReadCapacity10(struct VirtIOSCSIBase *libBase, struct IOStdReq *req, struct SCSICmd *scsiCmd)
{
    DPRINTF(libBase->IExec, "[virtioscsi:scsi_read_capacity_10.c] SCSI READ CAPACITY (10) requested\n");

    if (scsiCmd->scsi_Length < 8) {
        scsiCmd->scsi_Status = 2;
        req->io_Error = HFERR_BadStatus;
        return;
    }

    uint8 scsi_status = 0;
    uint32 residual = 0;
    struct VirtIOUSCSIDevUnit *unit = (struct VirtIOUSCSIDevUnit *)req->io_Unit;

    int32 rc = VirtIOSCSI_DoIO(libBase, unit, unit->target_id, unit->lun_id, scsiCmd->scsi_Command, scsiCmd->scsi_CmdLength,
                               (uint8 *)scsiCmd->scsi_Data, scsiCmd->scsi_Length, FALSE, &scsi_status, &residual);

    scsiCmd->scsi_Status = scsi_status;
    scsiCmd->scsi_Actual = scsiCmd->scsi_Length - residual;
    req->io_Actual = scsiCmd->scsi_Actual;

    if (rc != 0) {
        req->io_Error = (BYTE)rc;
        return;
    }

    req->io_Error = 0;
}
