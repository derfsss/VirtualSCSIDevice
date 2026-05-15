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

    /* Zero the whole struct before filling it.  Callers (SFS 1.290 in
     * particular) sometimes hand us a dirty buffer, and SFS refuses to
     * mount if dg_Reserved (or any other unwritten byte) has non-zero
     * bits -- the behaviour is "abort the mount without ever issuing a
     * read", which is exactly what we were seeing. */
    if (geom && req->io_Length >= sizeof(struct DriveGeometry)) {
        uint8 *p = (uint8 *)geom;
        for (uint32 i = 0; i < sizeof(struct DriveGeometry); i++) p[i] = 0;
    }

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

    /* Best-effort read of block 0; if it has a valid RDSK we'll use the
     * physical CHS the disk declares for itself instead of a hardcoded
     * shape that might fight the downstream partition blocks. */
    (void)ensure_rdb_geometry_cached(libBase, unit);

    if (unit && unit->geometry_valid) {
        uint32 heads, sectors_per_track, cylinders;

        if (unit->rdb_geometry_valid) {
            /* Report whatever the RDB declares.  This keeps our
             * TD_GETGEOMETRY consistent with every PartitionBlock on
             * the disk, which is what filesystem handlers compare
             * against during mount. */
            heads             = unit->rdb_phys_heads;
            sectors_per_track = unit->rdb_phys_sectors;
            cylinders         = unit->rdb_phys_cyls;
        } else {
            /* No RDB -- use "linear" geometry.  1 head × 1 sector per
             * track makes cylinder == LBA, which any partition layout
             * written later by HDToolbox will naturally match (tools
             * re-probe geometry at RDB-creation time). */
            heads             = 1;
            sectors_per_track = 1;
            uint64 c = unit->total_blocks;
            if (c > 0xFFFFFFFFULL) c = 0xFFFFFFFFULL;
            cylinders = (uint32)c;
            if (cylinders == 0) cylinders = 1;
        }

        uint32 cyl_sectors = heads * sectors_per_track;
        uint32 total32 = (unit->total_blocks > 0xFFFFFFFFULL)
                         ? 0xFFFFFFFFUL : (uint32)unit->total_blocks;

        geom->dg_SectorSize = unit->block_size;
        geom->dg_TotalSectors = total32;
        geom->dg_Heads = heads;
        geom->dg_TrackSectors = sectors_per_track;
        geom->dg_CylSectors = cyl_sectors;
        geom->dg_Cylinders = cylinders;
        /*
         * dg_BufMemType is used by `diskboot.kmod` (and any caller that
         * wants AmigaOS-allocated buffers / DOSNode/FSSM/DOSEnvVec memory
         * to live in BPTR-safe RAM) as the AllocMem() flag set.  On OS4
         * with MEMF_VIRTUAL high-memory pools, returning just MEMF_ANY (0)
         * or MEMF_PUBLIC (1) lets the allocator place the FSSM-attached
         * DOSEnvVec at addresses >= 0x80000000 -- when SFS later follows
         * fssm_Environ as a BPTR (`addr >> 2`), the round-trip back to an
         * APTR yields an unmapped page and SFS dies with a DSI on the
         * very first dereference (`lwz r8, 64(r10)` reading dn_DosType
         * at byte 64 of the DOSEnvVec).  **MEMF_LOCAL (bit 8)** restricts
         * the allocation to low-RAM (always BPTR-safe), which is what
         * a1ide.device returns and what every working AmigaOS disk
         * driver's `dg_BufMemType` should include.  MEMF_PUBLIC keeps
         * the buffers shareable across tasks; OR them.
         */
        geom->dg_BufMemType = MEMF_PUBLIC | MEMF_LOCAL; /* 0x101 */
        geom->dg_DeviceType = 0; /* DG_DIRECT_ACCESS */
        geom->dg_Flags = 0;

        DPRINTF(libBase->IExec,
                "[virtioscsi] TD_GETGEOMETRY: SectorSize=%lu TotalSectors=%lu "
                "C=%lu H=%lu S=%lu CylSectors=%lu DevType=%lu Flags=%lu\n",
                geom->dg_SectorSize, geom->dg_TotalSectors, geom->dg_Cylinders, geom->dg_Heads, geom->dg_TrackSectors,
                geom->dg_CylSectors, (uint32)geom->dg_DeviceType, (uint32)geom->dg_Flags);

        /* Raw byte dump of the struct we just filled, so we can verify the
         * caller sees exactly these bytes (catches accidental memory clobber). */
        uint8 *b = (uint8 *)geom;
        DPRINTF(libBase->IExec,
                "[virtioscsi] TD_GETGEOMETRY raw @ %p: "
                "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x "
                "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
                geom,
                b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15],
                b[16], b[17], b[18], b[19], b[20], b[21], b[22], b[23],
                b[24], b[25], b[26], b[27], b[28], b[29], b[30], b[31]);

        req->io_Error = 0;
        req->io_Actual = sizeof(struct DriveGeometry);
    } else {
        /* Fallback if no unit or capacity query failed */
        req->io_Error = HFERR_BadStatus;
    }
}
