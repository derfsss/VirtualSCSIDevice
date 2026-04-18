#include "scsi_cdb_helpers.h"
#include "virtioscsi.h"
#include "virtio/virtio_scsi_io.h"

void make_read_capacity16_cdb(uint8 *cdb)
{
    uint8 i;
    for (i = 0; i < 16; i++) cdb[i] = 0;
    cdb[0]  = 0x9E; /* SERVICE ACTION IN (16) */
    cdb[1]  = 0x10; /* READ CAPACITY (16) service action */
    cdb[13] = 32;   /* allocation length = 32 bytes (big-endian bytes 10-13) */
}

void make_read10_cdb(uint8 *cdb, uint32 lba, uint16 blocks)
{
    cdb[0] = 0x28; /* READ(10) opcode */
    cdb[1] = 0;
    cdb[2] = (lba >> 24) & 0xFF;
    cdb[3] = (lba >> 16) & 0xFF;
    cdb[4] = (lba >> 8) & 0xFF;
    cdb[5] = lba & 0xFF;
    cdb[6] = 0;                     /* Group number */
    cdb[7] = (blocks >> 8) & 0xFF;  /* Transfer length MSB */
    cdb[8] = blocks & 0xFF;         /* Transfer length LSB */
    cdb[9] = 0;                     /* Control */
}

void make_write10_cdb(uint8 *cdb, uint32 lba, uint16 blocks)
{
    cdb[0] = 0x2A; /* WRITE(10) opcode */
    cdb[1] = 0;
    cdb[2] = (lba >> 24) & 0xFF;
    cdb[3] = (lba >> 16) & 0xFF;
    cdb[4] = (lba >> 8) & 0xFF;
    cdb[5] = lba & 0xFF;
    cdb[6] = 0;
    cdb[7] = (blocks >> 8) & 0xFF;
    cdb[8] = blocks & 0xFF;
    cdb[9] = 0;
}

void make_read16_cdb(uint8 *cdb, uint64 lba, uint32 blocks)
{
    cdb[0]  = 0x88; /* READ(16) opcode */
    cdb[1]  = 0;
    cdb[2]  = (lba >> 56) & 0xFF;
    cdb[3]  = (lba >> 48) & 0xFF;
    cdb[4]  = (lba >> 40) & 0xFF;
    cdb[5]  = (lba >> 32) & 0xFF;
    cdb[6]  = (lba >> 24) & 0xFF;
    cdb[7]  = (lba >> 16) & 0xFF;
    cdb[8]  = (lba >> 8)  & 0xFF;
    cdb[9]  = lba         & 0xFF;
    cdb[10] = (blocks >> 24) & 0xFF;
    cdb[11] = (blocks >> 16) & 0xFF;
    cdb[12] = (blocks >> 8)  & 0xFF;
    cdb[13] = blocks          & 0xFF;
    cdb[14] = 0; /* Group number */
    cdb[15] = 0; /* Control */
}

void make_write16_cdb(uint8 *cdb, uint64 lba, uint32 blocks)
{
    cdb[0]  = 0x8A; /* WRITE(16) opcode */
    cdb[1]  = 0;
    cdb[2]  = (lba >> 56) & 0xFF;
    cdb[3]  = (lba >> 48) & 0xFF;
    cdb[4]  = (lba >> 40) & 0xFF;
    cdb[5]  = (lba >> 32) & 0xFF;
    cdb[6]  = (lba >> 24) & 0xFF;
    cdb[7]  = (lba >> 16) & 0xFF;
    cdb[8]  = (lba >> 8)  & 0xFF;
    cdb[9]  = lba         & 0xFF;
    cdb[10] = (blocks >> 24) & 0xFF;
    cdb[11] = (blocks >> 16) & 0xFF;
    cdb[12] = (blocks >> 8)  & 0xFF;
    cdb[13] = blocks          & 0xFF;
    cdb[14] = 0;
    cdb[15] = 0;
}

int32 ensure_geometry_cached(struct VirtIOSCSIBase *base, struct VirtIOUSCSIDevUnit *unit)
{
    if (!unit || unit->geometry_valid) {
        DPRINTF(base->IExec, "[virtioscsi:scsi_cdb_helpers.c] ensure_geometry_cached: unit=%p valid=%d (skip)\n",
                (void *)unit, unit ? (int)unit->geometry_valid : -1);
        return 0;
    }
    DPRINTF(base->IExec, "[virtioscsi:scsi_cdb_helpers.c] ensure_geometry_cached: issuing READ CAPACITY T%lu L%lu\n",
            unit->target_id, unit->lun_id);

    /*
     * Step 1: READ CAPACITY (10) — 8-byte response, 32-bit last LBA.
     * If the returned last LBA is 0xFFFFFFFF the disk is >= 2TB and we must
     * follow up with READ CAPACITY (16) to get the true 64-bit last LBA.
     */
    uint8 cap10[8] = {0};
    uint8 cdb10[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}; /* READ CAPACITY (10) */
    uint8 scsi_status = 0;
    uint32 residual = 0;

    int32 rc = VirtIOSCSI_DoIO(base, unit, unit->target_id, unit->lun_id,
                               cdb10, 10, cap10, 8, FALSE,
                               &scsi_status, &residual);
    if (rc != 0)
        return rc;
    if (scsi_status != 0)
        return HFERR_BadStatus;

    uint32 last_lba10 = ((uint32)cap10[0] << 24) | ((uint32)cap10[1] << 16) |
                        ((uint32)cap10[2] << 8)  |  (uint32)cap10[3];

    if (last_lba10 == 0xFFFFFFFF) {
        /*
         * Step 2: READ CAPACITY (16) — 32-byte response, 64-bit last LBA.
         * Disk is >= 2TB; use the 64-bit result.
         */
        DPRINTF(base->IExec, "[virtioscsi:scsi_cdb_helpers.c] RC10 returned 0xFFFFFFFF — issuing RC16\n");

        uint8 cap16[32] = {0};
        uint8 cdb16[16];
        make_read_capacity16_cdb(cdb16);
        scsi_status = 0;
        residual = 0;

        rc = VirtIOSCSI_DoIO(base, unit, unit->target_id, unit->lun_id,
                             cdb16, 16, cap16, 32, FALSE,
                             &scsi_status, &residual);
        if (rc != 0)
            return rc;
        if (scsi_status != 0)
            return HFERR_BadStatus;

        /* Bytes 0-7: last LBA (big-endian uint64) */
        uint64 last_lba64 = ((uint64)cap16[0] << 56) | ((uint64)cap16[1] << 48) |
                            ((uint64)cap16[2] << 40) | ((uint64)cap16[3] << 32) |
                            ((uint64)cap16[4] << 24) | ((uint64)cap16[5] << 16) |
                            ((uint64)cap16[6] << 8)  |  (uint64)cap16[7];
        /* Bytes 8-11: block size (big-endian uint32) */
        unit->block_size   = ((uint32)cap16[8] << 24) | ((uint32)cap16[9] << 16) |
                             ((uint32)cap16[10] << 8) |  (uint32)cap16[11];
        unit->total_blocks = last_lba64 + 1;

        DPRINTF(base->IExec, "[virtioscsi:scsi_cdb_helpers.c] RC16 Geometry: %llu blocks x %lu bytes\n",
                (unsigned long long)unit->total_blocks, unit->block_size);
    } else {
        /* READ CAPACITY (10) result is valid */
        unit->block_size   = ((uint32)cap10[4] << 24) | ((uint32)cap10[5] << 16) |
                             ((uint32)cap10[6] << 8)  |  (uint32)cap10[7];
        unit->total_blocks = (uint64)last_lba10 + 1;

        DPRINTF(base->IExec, "[virtioscsi:scsi_cdb_helpers.c] RC10 Geometry: %lu blocks x %lu bytes = %lu MB\n",
                (uint32)unit->total_blocks, unit->block_size,
                (uint32)((unit->total_blocks * unit->block_size) / (1024 * 1024)));
    }

    unit->geometry_valid = TRUE;
    return 0;
}

/*
 * Read block 0 of the unit, look for the "RDSK" signature, and extract
 * the physical CHS values the disk itself declares.  These are what
 * HDToolbox (or whatever tool initialised the RDB) wrote in; reporting
 * them back via TD_GETGEOMETRY keeps our geometry consistent with every
 * partition block downstream, rather than fighting it with made-up
 * constants.
 *
 * Block 0 layout (struct RigidDiskBlock):
 *   0x00 "RDSK"            4 bytes
 *   0x04 SummedLongs       uint32
 *   0x08 ChkSum            int32  (checksum: sum of SummedLongs longs == 0)
 *   0x0C HostID            uint32
 *   0x10 BlockBytes        uint32
 *   ...
 *   0x40 Cylinders         uint32
 *   0x44 Sectors           uint32  (= BlocksPerTrack)
 *   0x48 Heads             uint32  (= Surfaces)
 *
 * A missing RDSK signature or a checksum mismatch is NOT an error — it
 * just means "no RDB on this disk, use the fallback linear geometry".
 */
int32 ensure_rdb_geometry_cached(struct VirtIOSCSIBase *base, struct VirtIOUSCSIDevUnit *unit)
{
    struct ExecIFace *IExec = base->IExec;

    if (!unit || unit->rdb_geometry_checked)
        return 0;

    /* We need block_size before we can request "one block".  If the caller
     * hasn't called ensure_geometry_cached yet, do it now. */
    if (!unit->geometry_valid) {
        int32 rc = ensure_geometry_cached(base, unit);
        if (rc != 0)
            return rc;
    }

    uint32 bs = unit->block_size ? unit->block_size : 512;

    /* DMA-friendly buffer for the block-0 read. */
    uint8 *buf = IExec->AllocVecTags(bs, AVT_Type, MEMF_SHARED,
                                     AVT_ClearWithValue, 0, TAG_END);
    if (!buf) {
        DPRINTF(IExec, "[virtioscsi:scsi_cdb_helpers.c] RDB probe: AllocVec failed\n");
        unit->rdb_geometry_checked = TRUE;
        return 0; /* non-fatal */
    }

    uint8 cdb[10];
    make_read10_cdb(cdb, 0, 1); /* LBA 0, 1 block */

    uint8 scsi_status = 0;
    uint32 residual = 0;
    int32 rc = VirtIOSCSI_DoIO(base, unit, unit->target_id, unit->lun_id,
                               cdb, 10, buf, bs, FALSE,
                               &scsi_status, &residual);
    unit->rdb_geometry_checked = TRUE;

    if (rc != 0 || scsi_status != 0) {
        DPRINTF(IExec, "[virtioscsi:scsi_cdb_helpers.c] RDB probe: block-0 read failed rc=%ld status=0x%02x\n",
                (long)rc, (uint32)scsi_status);
        IExec->FreeVec(buf);
        return 0; /* non-fatal */
    }

    /* RDSK signature check */
    if (buf[0] != 'R' || buf[1] != 'D' || buf[2] != 'S' || buf[3] != 'K') {
        DPRINTF(IExec, "[virtioscsi:scsi_cdb_helpers.c] RDB probe: no RDSK signature at block 0 "
                "(got 0x%02x%02x%02x%02x) — using fallback geometry\n",
                buf[0], buf[1], buf[2], buf[3]);
        IExec->FreeVec(buf);
        return 0; /* non-fatal */
    }

    /* Extract the physical CHS (big-endian uint32s at offsets 0x40/0x44/0x48). */
    uint32 cyls    = ((uint32)buf[0x40] << 24) | ((uint32)buf[0x41] << 16) |
                     ((uint32)buf[0x42] << 8)  |  (uint32)buf[0x43];
    uint32 sectors = ((uint32)buf[0x44] << 24) | ((uint32)buf[0x45] << 16) |
                     ((uint32)buf[0x46] << 8)  |  (uint32)buf[0x47];
    uint32 heads   = ((uint32)buf[0x48] << 24) | ((uint32)buf[0x49] << 16) |
                     ((uint32)buf[0x4A] << 8)  |  (uint32)buf[0x4B];

    /* Sanity: values must be non-zero and factor into something <= total_blocks. */
    if (cyls == 0 || sectors == 0 || heads == 0 ||
        ((uint64)cyls * sectors * heads) > unit->total_blocks) {
        DPRINTF(IExec, "[virtioscsi:scsi_cdb_helpers.c] RDB probe: implausible CHS %lu/%lu/%lu (total %llu) — ignoring\n",
                cyls, heads, sectors, (unsigned long long)unit->total_blocks);
        IExec->FreeVec(buf);
        return 0;
    }

    unit->rdb_phys_cyls      = cyls;
    unit->rdb_phys_sectors   = sectors;
    unit->rdb_phys_heads     = heads;
    unit->rdb_geometry_valid = TRUE;

    DPRINTF(IExec, "[virtioscsi:scsi_cdb_helpers.c] RDB header CHS: C=%lu H=%lu S=%lu (= %lu blocks)\n",
            cyls, heads, sectors, cyls * heads * sectors);

    /*
     * Now also follow the PartitionList and read the FIRST partition's
     * `de_Surfaces` and `de_BlocksPerTrack`.  These are the values that
     * filesystem handlers (SFS) expect TD_GETGEOMETRY's dg_Heads/
     * dg_TrackSectors to match — because partition LBAs are computed as
     * `cyl * de_Surfaces * de_BlocksPerTrack`, and if our reported shape
     * differs from what's in the PartitionBlock, SFS gets a different
     * effective LBA when it cross-references the two.  IDE drivers like
     * a1ide.device avoid this trap because the disk's actual physical
     * CHS *is* what HDToolbox used to define the partition — so the
     * RDB header CHS, the PartitionBlock CHS, and TD_GETGEOMETRY all
     * agree.  On a virtual disk like ours, MediaToolbox can pick a
     * different shape from what we synthesised at partition-creation
     * time, leaving us with a mismatch unless we explicitly track the
     * partition's choice.
     *
     * Layout: rdb_PartitionList at byte 0x1C of the RDB header gives the
     * block number of the first PartitionBlock.  In the PartitionBlock,
     * the DOSEnvVec starts at byte 128; de_Surfaces is at +12 and
     * de_BlocksPerTrack is at +20 within that env.
     */
    uint32 part_block = ((uint32)buf[0x1C] << 24) | ((uint32)buf[0x1D] << 16) |
                        ((uint32)buf[0x1E] << 8)  |  (uint32)buf[0x1F];
    if (part_block != 0xFFFFFFFFU && part_block > 0 && part_block < unit->total_blocks) {
        uint8 *pbuf = IExec->AllocVecTags(bs, AVT_Type, MEMF_SHARED,
                                          AVT_ClearWithValue, 0, TAG_END);
        if (pbuf) {
            uint8 cdb2[10];
            make_read10_cdb(cdb2, part_block, 1);
            uint8 ss = 0;
            uint32 res = 0;
            int32 prc = VirtIOSCSI_DoIO(base, unit, unit->target_id, unit->lun_id,
                                        cdb2, 10, pbuf, bs, FALSE, &ss, &res);
            if (prc == 0 && ss == 0 &&
                pbuf[0] == 'P' && pbuf[1] == 'A' && pbuf[2] == 'R' && pbuf[3] == 'T') {
                /* DOSEnvVec is at offset 128, de_Surfaces at +12, de_BlocksPerTrack at +20 */
                uint32 surfaces = ((uint32)pbuf[128 + 12] << 24) |
                                  ((uint32)pbuf[128 + 13] << 16) |
                                  ((uint32)pbuf[128 + 14] << 8)  |
                                   (uint32)pbuf[128 + 15];
                uint32 blkpt    = ((uint32)pbuf[128 + 20] << 24) |
                                  ((uint32)pbuf[128 + 21] << 16) |
                                  ((uint32)pbuf[128 + 22] << 8)  |
                                   (uint32)pbuf[128 + 23];
                if (surfaces > 0 && blkpt > 0 &&
                    surfaces * blkpt == sectors * heads /* same total/cyl */ ) {
                    DPRINTF(IExec, "[virtioscsi:scsi_cdb_helpers.c] PartitionBlock CHS shape: "
                            "Surfaces=%lu BlocksPerTrack=%lu (overrides RDB header's H=%lu S=%lu — "
                            "same product, different shape)\n",
                            surfaces, blkpt, heads, sectors);
                    unit->rdb_phys_heads   = surfaces;
                    unit->rdb_phys_sectors = blkpt;
                }
            }
            IExec->FreeVec(pbuf);
        }
    }

    IExec->FreeVec(buf);
    return 0;
}
