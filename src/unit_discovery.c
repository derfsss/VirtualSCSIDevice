#include "unit_discovery.h"
#include "virtio/virtio_scsi_io.h"

/*
 * Scan SCSI targets 0-7, LUNs 0-7 via INQUIRY.
 * Allocates unit structs and stores them in devBase->units[].
 *
 * Partition discovery and DOSNode creation are handled by diskboot.kmod
 * (which scans devices listed in diskboot.config).  This driver does NOT
 * call mounter.library directly — that approach was incompatible with
 * resident priority 0 (mounter is initialised later).  Standard AmigaOS
 * disk drivers (a1ide.device, peg2ide.device, etc.) follow the same
 * pattern: discover units, register the device, let diskboot do the rest.
 */
uint32 DiscoverUnits(struct VirtIOSCSIBase *devBase)
{
    struct ExecIFace *iexec = devBase->IExec;

    uint8 *inqData = iexec->AllocVecTags(36, AVT_Type, MEMF_SHARED, AVT_ClearWithValue, 0, TAG_END);
    if (!inqData) {
        DPRINTF(iexec, "[virtioscsi:unit_discovery.c] Failed to allocate INQUIRY buffer\n");
        return 0;
    }

    uint8 cdb[6] = {0x12, 0, 0, 0, 36, 0}; /* INQUIRY */
    uint32 units_found = 0;

    for (uint32 t = 0; t < 8; t++) {
        for (uint32 l = 0; l < 8; l++) {
            if (units_found >= 8)
                break;

            uint8 scsi_status = 0;
            uint32 residual = 0;

            if (VirtIOSCSI_DoIO(devBase, NULL, t, l, cdb, 6, inqData, 36, FALSE, &scsi_status, &residual) == 0 &&
                scsi_status == 0) {
                uint8 qual = inqData[0] >> 5;
                uint8 devType = inqData[0] & 0x1F;

                /* Qualifier 0x03 means LUN not supported at this target address */
                if (qual == 0x03)
                    continue;

                char vendor[9], product[17], revision[5];

                for (int i = 0; i < 8; i++)
                    vendor[i] = inqData[8 + i];
                vendor[8] = '\0';
                for (int i = 0; i < 16; i++)
                    product[i] = inqData[16 + i];
                product[16] = '\0';
                for (int i = 0; i < 4; i++)
                    revision[i] = inqData[32 + i];
                revision[4] = '\0';

                DPRINTF(iexec, "[virtioscsi:unit_discovery.c] Found device at T%lu L%lu, Type 0x%02lX Qual 0x%02lX\n",
                        t, l, (uint32)devType, (uint32)qual);
                DPRINTF(iexec, "[virtioscsi:unit_discovery.c] Vendor='%s' Product='%s' Rev='%s'\n",
                        vendor, product, revision);

                /* Filter: Only accept Direct Access (Disk) or CD-ROM */
                if (devType != 0x00 && devType != 0x05) {
                    DPRINTF(iexec, "[virtioscsi:unit_discovery.c] Skipping non-disk target %lu LUN %lu\n", t, l);
                    continue;
                }

                struct VirtIOUSCSIDevUnit *unit = (struct VirtIOUSCSIDevUnit *)iexec->AllocVecTags(
                    sizeof(struct VirtIOUSCSIDevUnit), AVT_Type, MEMF_SHARED, AVT_ClearWithValue, 0, TAG_END);

                if (unit) {
                    unit->unit_num = units_found;
                    unit->target_id = t;
                    unit->lun_id = l;
                    unit->media_present = TRUE; /* assumed present at discovery */
                    devBase->units[units_found] = unit;
                    DPRINTF(iexec, "[virtioscsi:unit_discovery.c] Registered unit %lu (T%lu L%lu)\n",
                            units_found, t, l);
                    units_found++;
                }
            }
        }
    }

    iexec->FreeVec(inqData);

    if (units_found == 0) {
        DPRINTF(iexec, "[virtioscsi:unit_discovery.c] No drives found during scan.\n");
    }

    return units_found;
}
