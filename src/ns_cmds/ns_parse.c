#include "virtioscsi.h"
#include "virtioscsi_cmds.h"

void Parse_NS_Command(struct VirtIOSCSIBase *libBase, struct IOStdReq *req)
{
    DPRINTF(libBase->IExec, "[virtioscsi] NS Command parsed: 0x%04lX\n", (uint32)req->io_Command);

    switch (req->io_Command) {
    case NSCMD_DEVICEQUERY:
        Handle_NS_DeviceQuery(libBase, req);
        break;

    case NSCMD_TD_SEEK64:
    case NSCMD_ETD_SEEK64:
    case NSCMD_TD_CHANGEUNIT:
    case NSCMD_TD_ADDSTATCALLBACK:
    case NSCMD_TD_REMSTATCALLBACK:
        Handle_CMD_Success(libBase, req);
        break;

    case NSCMD_TD_GETGEOMETRY64:
        Handle_NS_TD_GetGeometry64(libBase, req);
        break;

    case NSCMD_TD_FORMAT64:
    case NSCMD_TD_READ64:
    case NSCMD_TD_WRITE64:
    case NSCMD_ETD_FORMAT64:
    case NSCMD_ETD_READ64:
    case NSCMD_ETD_WRITE64:
        Handle_NS_TD_IO64(libBase, req);
        break;

    default:
        req->io_Error = IOERR_NOCMD;
        break;
    }
}
