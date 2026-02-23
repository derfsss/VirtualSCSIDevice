#include "virtioscsi.h"
#include "virtioscsi_cmds.h"

/*
 * Handle_CMD_Update: Exec CMD_UPDATE (command 4)
 *
 * On a QEMU VirtIO SCSI backend, SYNCHRONIZE CACHE(10) can time out
 * because the host OS handles disk caching — there is no physical disk
 * write cache to flush. Issuing it causes a VirtIO request timeout which
 * triggers cascading failures in SFS and other filesystems.
 *
 * We return success immediately. This is safe because:
 * 1. QEMU writes go directly to the backing disk image file
 * 2. The host OS provides the actual write-through/caching layer
 * 3. VirtIO CMD_WRITE completions already mean data reached the backend
 */
void Handle_CMD_Update(struct VirtIOSCSIBase *libBase, struct IOStdReq *ioreq)
{
    DPRINTF(libBase->IExec, "[virtioscsi] CMD_UPDATE: no-op (QEMU handles caching)\n");
    ioreq->io_Error = 0;
}
