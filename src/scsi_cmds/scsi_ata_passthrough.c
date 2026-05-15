#include "virtioscsi.h"
#include "virtioscsi_cmds.h"
#include <scsi/sense_codes.h>

/*
 * ATA PASS-THROUGH handler (opcodes 0x85 / 0xA1).
 *
 * VirtIO SCSI is not an ATA device -- there is no real ATA layer to query.
 * SMART applications on AmigaOS 4 (e.g. AmigaDiskBench) send:
 *   CDB[0] = 0x85  (ATA PASS-THROUGH 16, SAT spec)
 *   CDB[14] = 0xB0 (ATA SMART command)
 *   CDB[4]  = 0xD0 (SMART Read Data sub-command)
 * and expect a 512-byte response in ATA SMART attribute format.
 *
 * If we return HFERR_BadStatus (unsupported opcode) the tool reports
 * io_Error = 45 and gives up entirely.  By returning a plausible dummy
 * 512-byte SMART data block we satisfy the caller's parsing logic.
 *
 * ATA SMART Read Data format (ATA/ATAPI-8 spec):
 *   Bytes   0-1:  Data structure revision (little-endian, 0x0006)
 *   Bytes   2-361: 30 x 12-byte attribute entries:
 *     [0]     Attribute ID (0 = unused slot)
 *     [1-2]   Status flags (little-endian)
 *     [3]     Current value (1-253, higher is better typically)
 *     [4]     Worst value ever
 *     [5-10]  Raw value (6 bytes, little-endian)
 *     [11]    Reserved
 *   Bytes 362-509: Reserved (zero)
 *   Byte  510:    Offline data collection status
 *   Byte  511:    Data structure checksum (we leave at 0 -- most tools skip)
 *
 * We report a small set of plausible attributes so that health-check tools
 * display meaningful (non-critical) output rather than a parse error.
 */

/* One attribute entry in the SMART Read Data block */
struct SMARTAttr {
    uint8 id;
    uint8 flags_lo;
    uint8 flags_hi;
    uint8 current;
    uint8 worst;
    uint8 raw[6];
    uint8 reserved;
};

static const struct SMARTAttr s_attrs[] = {
    /* ID  flags_lo flags_hi  cur  worst  raw[6]                  rsv */
    {   1, 0x0B,   0x00,     100, 100, {0,0,0,0,0,0},           0 }, /* Raw Read Error Rate     */
    {   9, 0x03,   0x00,     100, 100, {0x01,0x00,0,0,0,0},     0 }, /* Power-On Hours = 1      */
    {  12, 0x03,   0x00,     100, 100, {0x01,0x00,0,0,0,0},     0 }, /* Power Cycle Count = 1   */
    { 194, 0x02,   0x00,     120, 120, {0x1E,0,0,0,0x1E,0},     0 }, /* Temperature = 30°C      */
    { 197, 0x00,   0x00,     100, 100, {0,0,0,0,0,0},           0 }, /* Pending Sector Count = 0*/
    { 198, 0x00,   0x00,     100, 100, {0,0,0,0,0,0},           0 }, /* Uncorrectable = 0       */
};

#define NUM_ATTRS ((uint32)(sizeof(s_attrs) / sizeof(s_attrs[0])))

void Handle_SCSI_ATAPassthrough(struct VirtIOSCSIBase *libBase, struct IOStdReq *req,
                                 struct SCSICmd *scsiCmd)
{
    struct ExecIFace *IExec = libBase->IExec;
    uint8 *cdb = scsiCmd->scsi_Command;
    uint8 opcode = cdb[0];

    /*
     * Extract the ATA command from the CDB.
     * PASS-THROUGH 16: ATA Command in CDB[14]
     * PASS-THROUGH 12: ATA Command in CDB[9]
     */
    uint8 ata_cmd = (opcode == 0x85) ? cdb[14] : cdb[9];
    uint8 ata_feat = (opcode == 0x85) ? cdb[4]  : cdb[3];

    DPRINTF(IExec, "[virtioscsi:scsi_ata_passthrough.c] ATA PT opcode=0x%02X ata_cmd=0x%02X feat=0x%02X\n",
            (uint32)opcode, (uint32)ata_cmd, (uint32)ata_feat);

    /*
     * We only handle ATA SMART (0xB0) commands.  Any other ATA command
     * (IDENTIFY DEVICE 0xEC, etc.) returns ILLEGAL REQUEST.
     */
    if (ata_cmd != 0xB0) {
        DPRINTF(IExec, "[virtioscsi:scsi_ata_passthrough.c] ATA cmd 0x%02X not supported\n", (uint32)ata_cmd);
        scsiCmd->scsi_Status = 2; /* CHECK CONDITION */
        scsiCmd->scsi_Actual = 0;
        req->io_Error = HFERR_BadStatus;
        return;
    }

    uint8 *buf = (uint8 *)scsiCmd->scsi_Data;
    uint32 alloc = scsiCmd->scsi_Length;

    if (!buf || alloc < 512) {
        DPRINTF(IExec, "[virtioscsi:scsi_ata_passthrough.c] Buffer too small (%lu)\n", alloc);
        scsiCmd->scsi_Status = 2;
        scsiCmd->scsi_Actual = 0;
        req->io_Error = HFERR_BadStatus;
        return;
    }

    /* Zero the 512-byte SMART data block */
    uint32 i;
    volatile uint8 *p = (volatile uint8 *)buf;
    for (i = 0; i < 512; i++)
        p[i] = 0;

    /*
     * SMART Read Data / Read Thresholds -- both sub-commands get the
     * same dummy block (threshold = current value for all attributes,
     * so nothing ever looks critical).
     *
     * Bytes 0-1: revision number 0x0006 (little-endian)
     */
    buf[0] = 0x06;
    buf[1] = 0x00;

    /* Write attribute entries starting at offset 2 */
    uint32 off = 2;
    for (i = 0; i < NUM_ATTRS && off + 12 <= 362; i++) {
        const struct SMARTAttr *a = &s_attrs[i];
        buf[off + 0]  = a->id;
        buf[off + 1]  = a->flags_lo;
        buf[off + 2]  = a->flags_hi;
        buf[off + 3]  = a->current;
        buf[off + 4]  = a->worst;
        buf[off + 5]  = a->raw[0];
        buf[off + 6]  = a->raw[1];
        buf[off + 7]  = a->raw[2];
        buf[off + 8]  = a->raw[3];
        buf[off + 9]  = a->raw[4];
        buf[off + 10] = a->raw[5];
        buf[off + 11] = a->reserved;
        off += 12;
    }

    /* Byte 510: offline data collection status (0xC0 = never, no error) */
    buf[510] = 0xC0;
    /* Byte 511: checksum -- 0x00 (not computed; most parsers accept it) */

    scsiCmd->scsi_Actual = 512;
    scsiCmd->scsi_Status = 0; /* GOOD */
    Handle_CMD_Success(libBase, req);

    DPRINTF(IExec, "[virtioscsi:scsi_ata_passthrough.c] Returned 512-byte dummy SMART block (feat=0x%02X)\n",
            (uint32)ata_feat);
}
