#include "scsi_cdb_helpers.h"
#include "virtioscsi.h"
#include "virtioscsi_cmds.h"

void Handle_TD_GetNumTracks(struct VirtIOSCSIBase *libBase, struct IOStdReq *req)
{
    struct VirtIOUSCSIDevUnit *unit = (struct VirtIOUSCSIDevUnit *)req->io_Unit;
    uint32 cylinders = 32768; /* safe default for large disks */

    if (unit && unit->geometry_valid) {
        /* Match whatever shape TD_GETGEOMETRY reports: the disk's own
         * RDB-declared CHS when present, else linear (heads=1, spt=1). */
        (void)ensure_rdb_geometry_cached(libBase, unit);

        if (unit->rdb_geometry_valid) {
            cylinders = unit->rdb_phys_cyls;
        } else {
            uint64 c = unit->total_blocks; /* heads=1 × spt=1 → cyl == LBA */
            cylinders = (c > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (uint32)c;
            if (cylinders == 0) cylinders = 1;
        }
    }

    DPRINTF(libBase->IExec, "[virtioscsi] TD_GETNUMTRACKS: Returning %lu for T%lu L%lu (Valid: %d)\n", cylinders,
            unit->target_id, unit->lun_id, unit ? unit->geometry_valid : 0);

    req->io_Error = 0;
    req->io_Actual = cylinders;
}
