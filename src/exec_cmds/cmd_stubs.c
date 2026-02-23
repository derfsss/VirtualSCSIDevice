#include "virtioscsi.h"
#include "virtioscsi_cmds.h"

#include <devices/newstyle.h>

/*
 * Stub command handlers for VirtIO SCSI fixed-media devices.
 *
 * These return constant values because VirtIO disks are always present,
 * always writable, and non-removable. They must exist so AmigaOS
 * utilities and filesystems don't get unexpected errors.
 */

/* TD_CHANGESTATE: Returns 0 = disk is present (non-removable) */
void Handle_TD_ChangeState(struct VirtIOSCSIBase *libBase, struct IOStdReq *req)
{
    req->io_Actual = 0;
    req->io_Error = 0;
}

/* TD_PROTSTATUS: Returns 0 = disk is writable (not write-protected) */
void Handle_TD_ProtStatus(struct VirtIOSCSIBase *libBase, struct IOStdReq *req)
{
    req->io_Actual = 0;
    req->io_Error = 0;
}

/* TD_GETDRIVETYPE: Returns DRIVE_NEWSTYLE to signal 64-bit and NSD support */
void Handle_TD_GetDriveType(struct VirtIOSCSIBase *libBase, struct IOStdReq *req)
{
    struct VirtIOUSCSIDevUnit *unit = (struct VirtIOUSCSIDevUnit *)req->io_Unit;
    DPRINTF(libBase->IExec, "[virtioscsi] TD_GETDRIVETYPE called for T%lu L%lu\n", unit->target_id, unit->lun_id);
    req->io_Error = 0;
    req->io_Actual = DRIVE_NEWSTYLE;
}

/* Generic success handler for commands we don't emulate but must return cleanly */
void Handle_CMD_Success(struct VirtIOSCSIBase *libBase, struct IOStdReq *req)
{
    req->io_Error = 0;
    req->io_Actual = 0;
}
