#include "scsi_cdb_helpers.h"
#include "virtioscsi.h"
#include "virtioscsi_cmds.h"

/*
 * TD_GETNUMTRACKS: report the track (cylinder) count.
 *
 * Runs in the unit task (queued by BeginIO) because the RDB probe in
 * ensure_rdb_geometry_cached() reads block 0 from the disk.  Reports
 * the same shape TD_GETGEOMETRY uses: the disk's own RDB-declared
 * cylinders when present, else the linear heads=1 x spt=1 fallback
 * where cylinder count equals the block count.
 */
void Handle_TD_GetNumTracks(struct VirtIOSCSIBase *libBase, struct IOStdReq *req)
{
    struct VirtIOUSCSIDevUnit *unit = (struct VirtIOUSCSIDevUnit *)req->io_Unit;
    uint32 cylinders = 32768; /* safe default when geometry is unknown */

    if (unit && !unit->geometry_valid)
        (void)ensure_geometry_cached(libBase, unit);

    if (unit && unit->geometry_valid) {
        (void)ensure_rdb_geometry_cached(libBase, unit);

        if (unit->rdb_geometry_valid) {
            cylinders = unit->rdb_phys_cyls;
        } else {
            uint64 c = unit->total_blocks; /* heads=1 x spt=1 -> cyl == LBA */
            cylinders = (c > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (uint32)c;
            if (cylinders == 0) cylinders = 1;
        }
    }

    DPRINTF(libBase->IExec, "[virtioscsi] TD_GETNUMTRACKS: Returning %lu for T%lu L%lu (Valid: %d)\n",
            cylinders,
            unit ? unit->target_id : 99, unit ? unit->lun_id : 99,
            unit ? (int)unit->geometry_valid : 0);

    req->io_Error = 0;
    req->io_Actual = cylinders;
}
