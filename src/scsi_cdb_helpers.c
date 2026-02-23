#include "scsi_cdb_helpers.h"
#include "virtioscsi.h"
#include "virtio/virtio_scsi_io.h"

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
    if (!unit || unit->geometry_valid)
        return 0;

    uint8 cap_data[8] = {0};
    uint8 cdb[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}; /* READ CAPACITY(10) */
    uint8 scsi_status = 0;
    uint32 residual = 0;

    int32 rc = VirtIOSCSI_DoIO(base, unit, unit->target_id, unit->lun_id,
                               cdb, 10, cap_data, 8, FALSE,
                               &scsi_status, &residual);

    if (rc != 0)
        return rc;

    if (scsi_status != 0)
        return HFERR_BadStatus;

    unit->total_blocks = ((uint32)cap_data[0] << 24) | ((uint32)cap_data[1] << 16) |
                         ((uint32)cap_data[2] << 8) | (uint32)cap_data[3];
    unit->block_size = ((uint32)cap_data[4] << 24) | ((uint32)cap_data[5] << 16) |
                       ((uint32)cap_data[6] << 8) | (uint32)cap_data[7];

    /* READ CAPACITY returns last LBA, total blocks = last_lba + 1 */
    unit->total_blocks += 1;
    unit->geometry_valid = TRUE;

    DPRINTF(base->IExec, "[virtioscsi:scsi_cdb_helpers.c] Geometry: %lu blocks x %lu bytes = %lu MB\n",
            unit->total_blocks, unit->block_size,
            (uint32)(((uint64)unit->total_blocks * unit->block_size) / (1024 * 1024)));

    return 0;
}
