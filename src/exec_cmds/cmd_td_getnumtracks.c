#include "virtioscsi.h"
#include "virtioscsi_cmds.h"

void Handle_TD_GetNumTracks(struct VirtIOSCSIBase *libBase, struct IOStdReq *req)
{
    struct VirtIOUSCSIDevUnit *unit = (struct VirtIOUSCSIDevUnit *)req->io_Unit;
    uint32 cylinders = 32768; // Safe default for large hard disks

    if (unit && unit->geometry_valid) {
        cylinders = unit->total_blocks / (4 * 16); // heads * sectors_per_track
    }

    DPRINTF(libBase->IExec, "[virtioscsi] TD_GETNUMTRACKS: Returning %lu for T%lu L%lu (Valid: %d)\n", cylinders,
            unit->target_id, unit->lun_id, unit ? unit->geometry_valid : 0);

    req->io_Error = 0;
    req->io_Actual = cylinders;
}
