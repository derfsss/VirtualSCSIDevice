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
