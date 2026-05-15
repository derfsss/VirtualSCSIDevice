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

    if (unit && unit->geometry_valid) {
        /*
         * Synthesize a LOGICAL CHS that multiplies to total_blocks exactly,
         * matching the scheme sii3112ide.device uses.  Media's "Logical
         * size" panel computes disk size as dg_Cylinders * dg_CylSectors *
         * dg_SectorSize; any rounding loss there produces the "Total
         * sectors: -NNN" artifact and a slightly-undersized total disk
         * size in the UI.  Using a power-of-2 cyl_sectors divisor of
         * total_blocks guarantees no loss.
         *
         * We deliberately do NOT use the RDB-declared physical CHS here
         * (which Media reads from disk separately for its "Physical data"
         * panel).  Filesystems do their own LBA math via PartitionBlock's
         * de_Surfaces / de_BlocksPerTrack, so TD_GETGEOMETRY's CHS only
         * needs to encode the size correctly -- it doesn't need to match
         * RDB.  sii3112ide proves this works in practice.
         */
        uint64 t = unit->total_blocks;
        uint32 cyl_sectors = 1;
        /* Walk the largest power-of-2 factor of total_blocks, capped at 256
         * so dg_Cylinders stays large enough to look cylinder-like in tools
         * that key off it.  For an 8 TiB disk (2^34 blocks) this lands at
         * cyl_sectors=256, cylinders=2^26.  sii3112 uses 32 -- either works. */
        while ((t & 1) == 0 && cyl_sectors < 256) {
            t >>= 1;
            cyl_sectors <<= 1;
        }
        uint32 cylinders = (t > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (uint32)t;
        if (cylinders == 0) cylinders = 1;

        /* dg_TotalSectors is uint32 in struct DriveGeometry.  When the
         * underlying disk is >2 TiB the true block count cannot fit, so
         * we have a choice: wrap (= 0 for an 8 TiB disk: 2^34 mod 2^32 = 0)
         * or clamp at 0xFFFFFFFF.  Empirically diskboot.kmod (53.11 /
         * 2014) treats TotalSectors=0 as "size unknown" and skips
         * the unit entirely, so NO partitions get a DOSNode -- not even
         * partitions that lie wholly within the 32-bit LBA range.  Clamp
         * at 0xFFFFFFFF (= "I have at least 2 TiB") so the legacy boot
         * scan trusts the unit and processes the RDB.  Callers that need
         * the real >2 TiB count must use NSCMD_TD_GETGEOMETRY64 +
         * struct DriveGeometry64. */
        geom->dg_SectorSize   = unit->block_size;
        geom->dg_TotalSectors = (unit->total_blocks > 0xFFFFFFFFULL)
                                 ? 0xFFFFFFFFUL
                                 : (uint32)unit->total_blocks;
        geom->dg_Cylinders    = cylinders;
        geom->dg_CylSectors   = cyl_sectors;
        /* dg_Heads + dg_TrackSectors are decorative on a block-addressed
         * device; keep them consistent with dg_CylSectors so callers that
         * cross-check H*TS == CS don't trip.  H=cyl_sectors / TS=1 is the
         * simplest pair that satisfies this. */
        geom->dg_Heads        = cyl_sectors;
        geom->dg_TrackSectors = 1;
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
                "[virtioscsi] TD_GETGEOMETRY reply T%lu L%lu: SectorSize=%lu TotalSectors=%lu "
                "C=%lu CylSectors=%lu H=%lu S=%lu\n",
                unit->target_id, unit->lun_id,
                geom->dg_SectorSize, geom->dg_TotalSectors,
                geom->dg_Cylinders, geom->dg_CylSectors,
                geom->dg_Heads, geom->dg_TrackSectors);

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
