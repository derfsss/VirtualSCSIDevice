#ifndef SCSI_CDB_HELPERS_H
#define SCSI_CDB_HELPERS_H

#include <exec/types.h>

/*
 * Shared SCSI CDB builders and offset helpers.
 *
 * These eliminate duplicated CDB construction across cmd_read.c,
 * cmd_write.c, cmd_td_io64.c, scsi_read_10.c, scsi_write_10.c,
 * and ns_td_io64.c.
 */

/* Build a SCSI READ CAPACITY (16) CDB. Caller provides a 16-byte buffer.
 * Used when READ CAPACITY (10) returns last_lba == 0xFFFFFFFF (disk >= 2TB). */
void make_read_capacity16_cdb(uint8 *cdb);

/* Build a SCSI READ(10) CDB. Caller provides a 10-byte buffer. */
void make_read10_cdb(uint8 *cdb, uint32 lba, uint16 blocks);

/* Build a SCSI WRITE(10) CDB. Caller provides a 10-byte buffer. */
void make_write10_cdb(uint8 *cdb, uint32 lba, uint16 blocks);

/*
 * Build a SCSI READ(16) CDB. Caller provides a 16-byte buffer.
 * Use when LBA > 0xFFFFFFFF (disks larger than ~2TB at 512B blocks).
 */
void make_read16_cdb(uint8 *cdb, uint64 lba, uint32 blocks);

/*
 * Build a SCSI WRITE(16) CDB. Caller provides a 16-byte buffer.
 * Use when LBA > 0xFFFFFFFF (disks larger than ~2TB at 512B blocks).
 */
void make_write16_cdb(uint8 *cdb, uint64 lba, uint32 blocks);

/* Combine upper/lower 32-bit halves into a 64-bit byte offset.
 * Used by TD_READ64/WRITE64 and NSCMD_TD_READ64/WRITE64 handlers. */
static inline uint64 unpack_io64_offset(uint32 io_Actual, uint32 io_Offset)
{
    return ((uint64)io_Actual << 32) | io_Offset;
}

struct VirtIOSCSIBase;
struct VirtIOUSCSIDevUnit;

/* Issue READ CAPACITY(10) if geometry not yet cached in unit struct.
 * Returns 0 on success, non-zero on I/O failure. */
int32 ensure_geometry_cached(struct VirtIOSCSIBase *base, struct VirtIOUSCSIDevUnit *unit);

#endif /* SCSI_CDB_HELPERS_H */
