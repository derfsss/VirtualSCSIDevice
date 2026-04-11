#include "virtioscsi.h"
#include "virtioscsi_cmds.h"
#include <scsi/sense_codes.h>

/*
 * Fill a minimal 18-byte fixed-format sense buffer if the caller
 * requested SCSIF_AUTOSENSE and the command ended in CHECK CONDITION.
 */
static void FillAutoSense(struct SCSICmd *scsiCmd, uint8 sense_key, uint8 asc, uint8 ascq)
{
    scsiCmd->scsi_SenseActual = 0;

    if (!(scsiCmd->scsi_Flags & SCSIF_AUTOSENSE))
        return;
    if (!scsiCmd->scsi_SenseData || scsiCmd->scsi_SenseLength < 18)
        return;

    uint8 *sense = (uint8 *)scsiCmd->scsi_SenseData;

    /* Zero the buffer first */
    int i;
    for (i = 0; i < 18; i++)
        sense[i] = 0;

    sense[0] = 0x70;      /* Fixed format, current error */
    sense[2] = sense_key; /* Sense Key */
    sense[7] = 10;        /* Additional sense length (18 - 8) */
    sense[12] = asc;      /* Additional Sense Code */
    sense[13] = ascq;     /* Additional Sense Code Qualifier */

    scsiCmd->scsi_SenseActual = 18;
}

void Parse_SCSI_Command(struct VirtIOSCSIBase *libBase, struct IOStdReq *req)
{
    if (!req->io_Data || req->io_Length < sizeof(struct SCSICmd)) {
        DPRINTF(libBase->IExec, "[virtioscsi:scsi_parse.c] HD_SCSICMD: BADLENGTH data=%p len=%lu need=%lu\n",
                (void *)req->io_Data, (uint32)req->io_Length, (uint32)sizeof(struct SCSICmd));
        req->io_Error = IOERR_BADLENGTH;
        return;
    }

    struct SCSICmd *scsiCmd = (struct SCSICmd *)req->io_Data;

    if (!scsiCmd->scsi_Command || scsiCmd->scsi_CmdLength == 0) {
        DPRINTF(libBase->IExec, "[virtioscsi:scsi_parse.c] HD_SCSICMD: bad SCSICmd (cmd=%p cmdlen=%lu)\n",
                (void *)scsiCmd->scsi_Command, (uint32)scsiCmd->scsi_CmdLength);
        req->io_Error = HFERR_BadStatus;
        return;
    }

    scsiCmd->scsi_Status = 0;
    scsiCmd->scsi_CmdActual = scsiCmd->scsi_CmdLength;
    scsiCmd->scsi_SenseActual = 0;

    uint8 opcode = scsiCmd->scsi_Command[0];

    DPRINTF(libBase->IExec, "[virtioscsi:scsi_parse.c] SCSI Command parsed: opcode 0x%02lX\n", (uint32)opcode);

    switch (opcode) {
    case 0x00: /* TEST UNIT READY */
        Handle_SCSI_TestUnitReady(libBase, req, scsiCmd);
        break;

    case 0x12: /* INQUIRY */
        Handle_SCSI_Inquiry(libBase, req, scsiCmd);
        break;

    case 0x25: /* READ CAPACITY (10) */
        Handle_SCSI_ReadCapacity10(libBase, req, scsiCmd);
        break;

    case 0x28: /* READ (10) */
        Handle_SCSI_Read10(libBase, req, scsiCmd);
        break;

    case 0x2A: /* WRITE (10) */
        Handle_SCSI_Write10(libBase, req, scsiCmd);
        break;

    case SCSI_LOG_SENSE: /* LOG SENSE (0x4D) — S.M.A.R.T. */
        Handle_SCSI_LogSense(libBase, req, scsiCmd);
        break;

    case 0x85: /* ATA PASS-THROUGH (16) — SAT, used by SMART tools */
    case 0xA1: /* ATA PASS-THROUGH (12) — older tool fallback */
        Handle_SCSI_ATAPassthrough(libBase, req, scsiCmd);
        break;

    default:
        /* Unsupported opcode — CHECK CONDITION / INVALID COMMAND OPERATION CODE */
        DPRINTF(libBase->IExec, "[virtioscsi:scsi_parse.c] SCSI unsupported opcode 0x%02lX\n", (uint32)opcode);
        scsiCmd->scsi_Status = 2; /* CHECK CONDITION */
        FillAutoSense(scsiCmd, ILLEGAL_REQUEST, 0x20, 0x00); /* ASC 0x20: INVALID COMMAND OPERATION CODE */
        req->io_Error = HFERR_BadStatus;
        break;
    }
}
