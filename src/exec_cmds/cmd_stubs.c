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

/*
 * TD_GETDRIVETYPE: Returns DRIVE_NEWSTYLE (0x44).
 *
 * DRIVE_NEWSTYLE signals to filesystems and tools that this device supports
 * 64-bit block addressing (TD_READ64/TD_WRITE64) and New-Style Device (NSD)
 * commands (NSCMD_TD_READ64, NSCMD_TD_WRITE64, NSCMD_TD_GETGEOMETRY64).
 * This is the correct value for a VirtIO SCSI disk: it is a fixed-access
 * device with full 64-bit LBA support and no floppy/removable-media behaviour.
 */
void Handle_TD_GetDriveType(struct VirtIOSCSIBase *libBase, struct IOStdReq *req)
{
    struct VirtIOUSCSIDevUnit *unit = (struct VirtIOUSCSIDevUnit *)req->io_Unit;
    DPRINTF(libBase->IExec, "[virtioscsi] TD_GETDRIVETYPE called for T%lu L%lu\n", unit->target_id, unit->lun_id);
    req->io_Error  = 0;
    /* DEBUG: matched to a1ide.device which returns DRIVE3_5 not NSTY —
     * see comment in BeginIO.c TD_GETDRIVETYPE handler. */
    req->io_Actual = DRIVE3_5;
}

/* Generic success handler for commands we don't emulate but must return cleanly */
void Handle_CMD_Success(struct VirtIOSCSIBase *libBase, struct IOStdReq *req)
{
    req->io_Error = 0;
    req->io_Actual = 0;
}
