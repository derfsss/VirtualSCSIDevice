#include "virtioscsi.h"
#include "virtioscsi_cmds.h"

/*
 * Generic success handler for commands the driver accepts but does not
 * emulate (NSD seek/changeunit/statcallback no-ops, SMART emulation).
 *
 * The TD_CHANGESTATE / TD_PROTSTATUS / TD_GETDRIVETYPE constants are
 * answered inline in BeginIO.c -- they need no unit-task round trip.
 */
void Handle_CMD_Success(struct VirtIOSCSIBase *libBase, struct IOStdReq *req)
{
    (void)libBase;
    req->io_Error = 0;
    req->io_Actual = 0;
}
