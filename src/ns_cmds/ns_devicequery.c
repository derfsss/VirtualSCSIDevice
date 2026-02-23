#include "cmd_names.h"
#include "virtioscsi.h"
#include "virtioscsi_cmds.h"

void Handle_NS_DeviceQuery(struct VirtIOSCSIBase *libBase, struct IOStdReq *req)
{
    struct NSDeviceQueryResult *res = (struct NSDeviceQueryResult *)req->io_Data;
    uint32 len = req->io_Length;

    DPRINTF(libBase->IExec, "[virtioscsi:ns_devicequery.c] NSCMD_DEVICEQUERY: Length %lu\n", len);

    if (len < sizeof(struct NSDeviceQueryResult)) {
        req->io_Actual = 0;
        req->io_Error = IOERR_BADLENGTH;
        return;
    }

    res->DevQueryFormat = 0;
    res->SizeAvailable = sizeof(struct NSDeviceQueryResult);
    res->DeviceType = NSDEVTYPE_TRACKDISK;
    res->DeviceSubType = 0;
    res->SupportedCommands = (uint16 *)supported_commands;

    req->io_Actual = sizeof(struct NSDeviceQueryResult);
    req->io_Error = 0;
}
