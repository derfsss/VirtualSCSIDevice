#include "scsi_cdb_helpers.h"
#include "virtioscsi.h"
#include "virtioscsi_cmds.h"

/*
 * Handle_NS_TD_GetGeometry64: Returns 64-bit geometry
 * Uses cached geometry from unit struct (populated by ensure_geometry_cached).
 */
void Handle_NS_TD_GetGeometry64(struct VirtIOSCSIBase *libBase, struct IOStdReq *req)
{
    struct DriveGeometry64 *geom = (struct DriveGeometry64 *)req->io_Data;

    if (req->io_Length < sizeof(struct DriveGeometry64)) {
        DPRINTF(libBase->IExec, "[virtioscsi:ns_td_getgeometry64.c] BADLENGTH len=%lu\n",
                (uint32)req->io_Length);
        req->io_Error = IOERR_BADLENGTH;
        return;
    }

    struct VirtIOUSCSIDevUnit *unit = (struct VirtIOUSCSIDevUnit *)req->io_Unit;

    int32 rc = ensure_geometry_cached(libBase, unit);
    if (rc != 0) {
        DPRINTF(libBase->IExec, "[virtioscsi:ns_td_getgeometry64.c] geometry cache failed rc=%ld\n",
                (long)rc);
        req->io_Actual = 0;
        req->io_Error = (BYTE)rc;
        return;
    }

    if (unit && unit->geometry_valid) {
        geom->dg_SectorSize = (uint32)unit->block_size;
        geom->dg_Reserved1 = 0;
        geom->dg_TotalSectors = (uint64)unit->total_blocks;
        geom->dg_BufMemTags = NULL;
        geom->dg_DeviceType = 0; /* DG_DIRECT_ACCESS */
        geom->dg_Flags = 0;      /* Fixed media */
        geom->dg_Reserved2 = 0;

        req->io_Actual = sizeof(struct DriveGeometry64);
        req->io_Error = 0;
    } else {
        req->io_Actual = 0;
        req->io_Error = HFERR_BadStatus;
    }
}
