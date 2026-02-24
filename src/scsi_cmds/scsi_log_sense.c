#include "virtioscsi.h"
#include "virtioscsi_cmds.h"
#include <scsi/sense_codes.h>
#include <string.h>

/*
 * Handle_SCSI_LogSense: LOG SENSE (0x4D)
 *
 * Returns synthetic S.M.A.R.T. data. VirtIO SCSI has no real SMART layer,
 * so we synthesise page 0x2F (Informational Exceptions) with plausible
 * dummy values. This prevents SMART tools from reporting a parse error.
 *
 * Pages supported:
 *   0x00 — Supported Log Pages list
 *   0x2F — Informational Exceptions (S.M.A.R.T. health page, SPC-4 7.3.14)
 */
void Handle_SCSI_LogSense(struct VirtIOSCSIBase *libBase, struct IOStdReq *req, struct SCSICmd *scsiCmd)
{
    uint8 *cdb = scsiCmd->scsi_Command;
    uint8 page_code = cdb[2] & 0x3F;
    uint16 allocation_length = ((uint16)cdb[7] << 8) | cdb[8];
    uint8 *buffer = (uint8 *)scsiCmd->scsi_Data;

    DPRINTF(libBase->IExec, "[virtioscsi:scsi_log_sense.c] LOG SENSE Page 0x%02X, AllocLen %u\n", (uint32)page_code,
            (uint32)allocation_length);

    if (!buffer || allocation_length == 0) {
        scsiCmd->scsi_Actual = 0;
        Handle_CMD_Success(libBase, req);
        return;
    }

    uint32 transfer_len = 0;
    uint8 temp_buf[256];

    libBase->IUtility->ClearMem(temp_buf, sizeof(temp_buf));

    switch (page_code) {

    case 0x00:
        /*
         * Supported Log Pages list (SPC-4 7.3.2).
         * 4-byte page header + one byte per supported page code.
         */
        temp_buf[0] = 0x00; /* page code */
        temp_buf[1] = 0x00; /* reserved */
        temp_buf[2] = 0x00; /* page length high */
        temp_buf[3] = 0x02; /* page length low: 2 supported pages follow */
        temp_buf[4] = 0x00; /* page 0x00 */
        temp_buf[5] = 0x2F; /* page 0x2F */
        transfer_len = 6;
        break;

    case 0x2F: {
        /*
         * Informational Exceptions page (SPC-4 7.3.14), used by SMART tools.
         * Two parameters:
         *   0x0000 — Informational Exceptions general (ASC/ASCQ = 0x00/0x00 = no error)
         *   0x8000 — Vendor-specific: string identifying this as a VirtIO dummy
         */
        temp_buf[0] = 0x2F; /* page code */
        temp_buf[1] = 0x00; /* reserved */
        /* Page length filled in after computing transfer_len */

        /* Parameter 0x0000: Informational Exceptions */
        temp_buf[4] = 0x00; /* parameter code high */
        temp_buf[5] = 0x00; /* parameter code low */
        temp_buf[6] = 0x03; /* control: binary format, not saveable */
        temp_buf[7] = 0x04; /* parameter length */
        temp_buf[8] = 0x00; /* ASC: no additional sense information */
        temp_buf[9] = 0x00; /* ASCQ */
        /* temp_buf[10..11] reserved, already zeroed */

        /* Parameter 0x8000: Vendor-specific health string */
        temp_buf[12] = 0x80; /* parameter code high */
        temp_buf[13] = 0x00; /* parameter code low */
        temp_buf[14] = 0x03; /* control */
        const char *attr = "VirtIO Dummy OK";
        uint32 attr_len = (uint32)strlen(attr);
        temp_buf[15] = (uint8)attr_len; /* parameter length */
        libBase->IExec->CopyMem((APTR)attr, &temp_buf[16], attr_len);

        transfer_len = 16 + attr_len;
        temp_buf[2] = 0x00;                        /* page length high */
        temp_buf[3] = (uint8)(transfer_len - 4);   /* page length low */
        break;
    }

    default:
        DPRINTF(libBase->IExec, "[virtioscsi:scsi_log_sense.c] Unsupported Log Page 0x%02X\n", (uint32)page_code);
        scsiCmd->scsi_Status = 2; /* CHECK CONDITION */
        req->io_Error = HFERR_BadStatus;
        break;
    }

    if (scsiCmd->scsi_Status == 0 && transfer_len > 0) {
        if (transfer_len > allocation_length)
            transfer_len = allocation_length;
        libBase->IExec->CopyMem(temp_buf, buffer, transfer_len);
        scsiCmd->scsi_Actual = transfer_len;
    }

    if (scsiCmd->scsi_Status == 0)
        Handle_CMD_Success(libBase, req);
}
