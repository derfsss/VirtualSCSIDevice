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
        DPRINTF(libBase->IExec, "[virtioscsi:cmd_td_getgeometry.c] BADLENGTH len=%lu\n",
                (uint32)req->io_Length);
        req->io_Error = IOERR_BADLENGTH;
        return;
    }

    struct VirtIOUSCSIDevUnit *unit = (struct VirtIOUSCSIDevUnit *)req->io_Unit;

    int32 rc = ensure_geometry_cached(libBase, unit);
    if (rc != 0) {
        DPRINTF(libBase->IExec, "[virtioscsi:cmd_td_getgeometry.c] geometry cache failed rc=%ld\n",
                (long)rc);
        req->io_Actual = 0;
        req->io_Error = (BYTE)rc;
        return;
    }

    if (unit && unit->geometry_valid) {
        uint32 sectors_per_track = 16;
        uint32 heads = 4;

        /* dg_TotalSectors and dg_Cylinders are uint32 — clamp for >2TB disks */
        uint32 cyl_sectors = heads * sectors_per_track;
        uint32 total32 = (unit->total_blocks > 0xFFFFFFFFULL)
                         ? 0xFFFFFFFFUL : (uint32)unit->total_blocks;
        uint32 cylinders = (uint32)(unit->total_blocks / cyl_sectors);
        if (cylinders == 0) cylinders = 1;

        geom->dg_SectorSize = unit->block_size;
        geom->dg_TotalSectors = total32;
        geom->dg_Heads = heads;
        geom->dg_TrackSectors = sectors_per_track;
        geom->dg_CylSectors = cyl_sectors;
        geom->dg_Cylinders = cylinders;
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
