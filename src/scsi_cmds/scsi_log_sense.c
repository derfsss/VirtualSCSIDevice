#include "virtioscsi.h"
#include "virtioscsi_cmds.h"
#include <scsi/sense_codes.h>

/*
 * LOG SENSE handler for dummy S.M.A.R.T. data.
 */
void Handle_SCSI_LogSense(struct VirtIOSCSIBase *libBase, struct IOStdReq *req, struct SCSICmd *scsiCmd)
{
    uint8 *cdb = scsiCmd->scsi_Command;
    uint8 page_code = cdb[2] & 0x3F;
    uint16 allocation_length = (cdb[7] << 8) | cdb[8];
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

    /* Use native ClearMem for efficiency on stack buffer */
    libBase->IUtility->ClearMem(temp_buf, sizeof(temp_buf));

    switch (page_code) {
    case 0x00:              // Supported Log Pages
        temp_buf[0] = 0x00; // Page Code
        temp_buf[1] = 0x00; // Reserved
        temp_buf[2] = 0x00; // Page Length (High)
        temp_buf[3] = 0x02; // Page Length (Low): 2 bytes follow
        temp_buf[4] = 0x00; // Page 0x00
        temp_buf[5] = 0x2F; // Page 0x2F
        transfer_len = 6;
        break;

    case 0x2F:              // Informational Exceptions (S.M.A.R.T.)
        temp_buf[0] = 0x2F; // Page Code
        temp_buf[1] = 0x00; // Reserved
        // Page Length: 8 bytes for param 0 + 16 bytes for param 0x8000
        temp_buf[2] = 0x00;
        temp_buf[3] = 24;

        // Parameter 0x0000: Informational Exceptions
        temp_buf[4] = 0x00;  // Param Code High
        temp_buf[5] = 0x00;  // Param Code Low
        temp_buf[6] = 0x03;  // Control (Binary, Not saving, etc)
        temp_buf[7] = 0x04;  // Length
        temp_buf[8] = 0x00;  // ASC
        temp_buf[9] = 0x00;  // ASCQ
        temp_buf[10] = 0x00; // Reserved
        temp_buf[11] = 0x00; // Reserved

        // Parameter 0x8000: Vendor Specific Attribution (VirtIO Dummy)
        temp_buf[12] = 0x80; // Param Code High
        temp_buf[13] = 0x00; // Param Code Low
        temp_buf[14] = 0x03; // Control
        const char *attr = "VirtIO Dummy OK";
        uint32 attr_len = libBase->IUtility->Strlen(attr);

        temp_buf[15] = (uint8)attr_len;
        libBase->IExec->CopyMem((APTR)attr, &temp_buf[16], attr_len);

        transfer_len = 16 + attr_len;
        // Fix up page length
        temp_buf[3] = (uint8)(transfer_len - 4);
        break;

    default:
        DPRINTF(libBase->IExec, "[virtioscsi:scsi_log_sense.c] Unsupported Log Page 0x%02X\n", (uint32)page_code);
        scsiCmd->scsi_Status = 2; // CHECK CONDITION
        req->io_Error = HFERR_BadStatus;
        break;
    }

    if (scsiCmd->scsi_Status == 0 && transfer_len > 0) {
        if (transfer_len > allocation_length)
            transfer_len = allocation_length;

        libBase->IExec->CopyMem(temp_buf, buffer, transfer_len);
        scsiCmd->scsi_Actual = transfer_len;
    }

    if (scsiCmd->scsi_Status == 0) {
        Handle_CMD_Success(libBase, req);
    }
}
