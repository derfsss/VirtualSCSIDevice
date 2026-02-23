#include "scsi_cdb_helpers.h"
#include "virtioscsi.h"
#include "virtioscsi_cmds.h"

/*
 * Handle_TD_GetGeometry: TD_GETGEOMETRY (command 4)
 *
 * Returns real disk geometry by issuing READ CAPACITY(10) via VirtIO.
 * Results are cached in the unit struct for subsequent calls.
 */
void Handle_TD_GetGeometry(struct VirtIOSCSIBase *libBase, struct IOStdReq *req)
{
    struct DriveGeometry *geom = (struct DriveGeometry *)req->io_Data;

    if (req->io_Length < sizeof(struct DriveGeometry)) {
        req->io_Error = IOERR_BADLENGTH;
        return;
    }

    struct VirtIOUSCSIDevUnit *unit = (struct VirtIOUSCSIDevUnit *)req->io_Unit;

    int32 rc = ensure_geometry_cached(libBase, unit);
    if (rc != 0) {
        req->io_Actual = 0;
        req->io_Error = (BYTE)rc;
        return;
    }

    if (unit && unit->geometry_valid) {
        uint32 sectors_per_track = 16;
        uint32 heads = 4;

        geom->dg_SectorSize = unit->block_size;
        geom->dg_TotalSectors = unit->total_blocks;
        geom->dg_Heads = heads;
        geom->dg_TrackSectors = sectors_per_track;
        geom->dg_CylSectors = heads * sectors_per_track;
        geom->dg_Cylinders = unit->total_blocks / geom->dg_CylSectors;
        geom->dg_BufMemType = MEMF_PUBLIC;
        geom->dg_DeviceType = 0; /* DG_DIRECT_ACCESS */
        geom->dg_Flags = 0;

        DPRINTF(libBase->IExec,
                "[virtioscsi] TD_GETGEOMETRY: SectorSize=%lu TotalSectors=%lu "
                "C=%lu H=%lu S=%lu CylSectors=%lu DevType=%lu Flags=%lu\n",
                geom->dg_SectorSize, geom->dg_TotalSectors, geom->dg_Cylinders, geom->dg_Heads, geom->dg_TrackSectors,
                geom->dg_CylSectors, (uint32)geom->dg_DeviceType, (uint32)geom->dg_Flags);

        req->io_Error = 0;
        req->io_Actual = sizeof(struct DriveGeometry);
    } else {
        /* Fallback if no unit or capacity query failed */
        req->io_Error = HFERR_BadStatus;
    }
}
