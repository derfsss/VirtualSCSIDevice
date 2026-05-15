#include "virtio/virtio_scsi_io.h"
#include "virtioscsi.h"
#include "virtioscsi_cmds.h"
#include <scsi/sense_codes.h>

/*
 * VPD response helper: zero the output buffer and fill the 4-byte page header.
 * Returns pointer past the header for the caller to fill page-specific data.
 * Checks that scsiCmd->scsi_Length >= needed; returns FALSE if buffer too small.
 */
static BOOL vpd_begin(struct SCSICmd *scsiCmd, uint8 page_code, uint32 needed,
                      struct IOStdReq *req)
{
    if (scsiCmd->scsi_Length < needed || !scsiCmd->scsi_Data) {
        scsiCmd->scsi_Status = 2;
        scsiCmd->scsi_Actual = 0;
        req->io_Actual = 0;
        req->io_Error = HFERR_BadStatus;
        return FALSE;
    }
    uint8 *buf = (uint8 *)scsiCmd->scsi_Data;
    uint32 i;
    for (i = 0; i < needed; i++)
        buf[i] = 0;
    buf[0] = 0x00; /* peripheral qualifier + device type: direct-access */
    buf[1] = page_code;
    /* buf[2] = 0 (reserved) */
    /* buf[3] = page data length (set by caller) */
    return TRUE;
}

static void vpd_finish(struct SCSICmd *scsiCmd, struct IOStdReq *req, uint32 total_len)
{
    scsiCmd->scsi_Status = 0;
    scsiCmd->scsi_Actual = total_len;
    req->io_Actual       = total_len;
    req->io_Error        = 0;
}

static void vpd_illegal_page(struct SCSICmd *scsiCmd, struct IOStdReq *req)
{
    /* ILLEGAL REQUEST / INVALID FIELD IN CDB (ASC 0x24, ASCQ 0x00) */
    scsiCmd->scsi_Status = 2; /* CHECK CONDITION */
    scsiCmd->scsi_Actual = 0;
    req->io_Actual = 0;

    if ((scsiCmd->scsi_Flags & SCSIF_AUTOSENSE) && scsiCmd->scsi_SenseData
            && scsiCmd->scsi_SenseLength >= 18) {
        uint8 *s = (uint8 *)scsiCmd->scsi_SenseData;
        uint32 i;
        for (i = 0; i < 18; i++) s[i] = 0;
        s[0]  = 0x70;           /* fixed format, current error */
        s[2]  = ILLEGAL_REQUEST; /* sense key */
        s[7]  = 10;             /* additional sense length */
        s[12] = 0x24;           /* ASC: invalid field in CDB */
        s[13] = 0x00;           /* ASCQ */
        scsiCmd->scsi_SenseActual = 18;
    }
    req->io_Error = HFERR_BadStatus;
}

void Handle_SCSI_Inquiry(struct VirtIOSCSIBase *libBase, struct IOStdReq *req, struct SCSICmd *scsiCmd)
{
    DPRINTF(libBase->IExec, "[virtioscsi:scsi_inquiry.c] SCSI INQUIRY requested (Length: %lu)\n",
            scsiCmd->scsi_Length);

    if (scsiCmd->scsi_Length < 5) {
        scsiCmd->scsi_Status = 2;
        scsiCmd->scsi_Actual = 0;
        req->io_Actual = 0;
        req->io_Error = HFERR_BadStatus;
        return;
    }

    uint8 *cdb     = scsiCmd->scsi_Command;
    BOOL   evpd    = (scsiCmd->scsi_CmdLength >= 2) && (cdb[1] & 0x01);
    uint8  pgcode  = (scsiCmd->scsi_CmdLength >= 3) ? cdb[2] : 0;

    if (!evpd) {
        /* Standard INQUIRY -- pass through to VirtIO device */
        uint8  scsi_status = 0;
        uint32 residual    = 0;
        struct VirtIOUSCSIDevUnit *unit = (struct VirtIOUSCSIDevUnit *)req->io_Unit;

        int32 rc = VirtIOSCSI_DoIO(libBase, unit, unit->target_id, unit->lun_id,
                                   cdb, scsiCmd->scsi_CmdLength,
                                   (uint8 *)scsiCmd->scsi_Data, scsiCmd->scsi_Length,
                                   FALSE, &scsi_status, &residual);

        scsiCmd->scsi_Status = scsi_status;
        scsiCmd->scsi_Actual = scsiCmd->scsi_Length - residual;
        req->io_Actual = scsiCmd->scsi_Actual;

        if (rc != 0) {
            req->io_Error = (BYTE)rc;
            return;
        }
        req->io_Error = 0;
        return;
    }

    /*
     * EVPD=1: caller wants a Vital Product Data page.
     * We synthesise three pages locally; any other page code returns
     * CHECK CONDITION ILLEGAL REQUEST / INVALID FIELD IN CDB.
     */
    struct VirtIOUSCSIDevUnit *unit = (struct VirtIOUSCSIDevUnit *)req->io_Unit;
    uint8 *buf = (uint8 *)scsiCmd->scsi_Data;

    switch (pgcode) {

    case 0x00: {
        /*
         * Page 0x00 -- Supported VPD Pages List.
         * Lists the three page codes we implement.
         * Format: [devtype][0x00][reserved][page_len][page_codes...]
         */
        if (!vpd_begin(scsiCmd, 0x00, 7, req))
            return;
        buf[3] = 3;    /* page data length: 3 additional bytes */
        buf[4] = 0x00; /* page 0x00 */
        buf[5] = 0x80; /* page 0x80 */
        buf[6] = 0x83; /* page 0x83 */
        vpd_finish(scsiCmd, req, 7);
        DPRINTF(libBase->IExec, "[virtioscsi:scsi_inquiry.c] VPD page 0x00 returned\n");
        break;
    }

    case 0x80: {
        /*
         * Page 0x80 -- Unit Serial Number.
         * Serial: "VIRTIOSCSI-T%lu" formatted with target_id.
         * Format: [devtype][0x80][reserved][serial_len][serial...]
         */
        char serial[24];
        uint32 slen = (uint32)libBase->IUtility->SNPrintf(serial, sizeof(serial),
                                                           "VIRTIOSCSI-T%lu",
                                                           (uint32)unit->target_id);
        uint32 total = 4 + slen;
        if (!vpd_begin(scsiCmd, 0x80, total, req))
            return;
        buf[3] = (uint8)slen;
        uint32 i;
        for (i = 0; i < slen; i++)
            buf[4 + i] = (uint8)serial[i];
        vpd_finish(scsiCmd, req, total);
        DPRINTF(libBase->IExec, "[virtioscsi:scsi_inquiry.c] VPD page 0x80 returned serial '%s'\n", serial);
        break;
    }

    case 0x83: {
        /*
         * Page 0x83 -- Device Identification.
         * One T10 vendor ID designation descriptor:
         *   code set=ASCII(2), identifier type=T10 vendor(1),
         *   id = "QEMU    virtioscsi      T%lu"
         *          ^8 chars  ^16 chars
         * Format: [devtype][0x83][reserved][total_desig_len]
         *           [code_set][id_type][reserved][id_len][id...]
         */
        char id[40];
        uint32 idlen = (uint32)libBase->IUtility->SNPrintf(id, sizeof(id),
                                                            "QEMU    virtioscsi      T%lu",
                                                            (uint32)unit->target_id);
        /* designator: 4-byte header + idlen bytes */
        uint32 desig_len = 4 + idlen;
        /* page: 4-byte page header + designator */
        uint32 total = 4 + desig_len;
        if (!vpd_begin(scsiCmd, 0x83, total, req))
            return;
        buf[3] = (uint8)desig_len; /* page data length */
        /* Designation descriptor */
        buf[4] = 0x02; /* code set: ASCII */
        buf[5] = 0x01; /* identifier type: T10 vendor ID */
        buf[6] = 0x00; /* reserved */
        buf[7] = (uint8)idlen;
        uint32 i;
        for (i = 0; i < idlen; i++)
            buf[8 + i] = (uint8)id[i];
        vpd_finish(scsiCmd, req, total);
        DPRINTF(libBase->IExec, "[virtioscsi:scsi_inquiry.c] VPD page 0x83 returned id '%s'\n", id);
        break;
    }

    default:
        /* Unsupported VPD page -- ILLEGAL REQUEST */
        DPRINTF(libBase->IExec, "[virtioscsi:scsi_inquiry.c] VPD unsupported page 0x%02X\n", pgcode);
        vpd_illegal_page(scsiCmd, req);
        break;
    }
}
