#include "virtio/virtio_scsi_io.h"
#include "virtioscsi.h"
#include "virtioscsi_cmds.h"

void Handle_SCSI_TestUnitReady(struct VirtIOSCSIBase *libBase, struct IOStdReq *req, struct SCSICmd *scsiCmd)
{
    DPRINTF(libBase->IExec, "[virtioscsi:scsi_test_unit_ready.c] SCSI TEST UNIT READY\n");

    uint8 scsi_status = 0;
    uint32 residual = 0;
    struct VirtIOUSCSIDevUnit *unit = (struct VirtIOUSCSIDevUnit *)req->io_Unit;

    int32 rc = VirtIOSCSI_DoIO(libBase, unit, unit->target_id, unit->lun_id, scsiCmd->scsi_Command, scsiCmd->scsi_CmdLength,
                               NULL, 0, FALSE, &scsi_status, &residual);

    scsiCmd->scsi_Status = scsi_status;
    scsiCmd->scsi_Actual = 0;
    req->io_Actual = 0;

    if (rc != 0) {
        req->io_Error = (BYTE)rc;
        return;
    }

    req->io_Error = 0;
}
