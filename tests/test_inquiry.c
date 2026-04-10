#include <devices/newstyle.h>
#include <devices/scsidisk.h>
#include <dos/dos.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/types.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>

#define DEVICE_NAME "virtioscsi.device"

int main()
{
    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    if (!port) {
        printf("Failed to allocate MsgPort\n");
        return 1;
    }

    struct IOStdReq *req = (struct IOStdReq *)IExec->AllocSysObjectTags(
        ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct IOStdReq), ASOIOR_ReplyPort, port, TAG_DONE);

    if (!req) {
        printf("Failed to allocate IORequest\n");
        IExec->FreeSysObject(ASOT_PORT, port);
        return 1;
    }

    // Try to open the device. It likely won't be in DEVS: yet, so we assume it was run from the project root.
    printf("Attempting to open %s Unit 0...\n", DEVICE_NAME);
    BYTE open_err = IExec->OpenDevice("build/" DEVICE_NAME, 0, (struct IORequest *)req, 0);

    // If local build fails, try standard system assignment
    if (open_err) {
        printf("Failed to open local build, trying system location: %s...\n", DEVICE_NAME);
        open_err = IExec->OpenDevice(DEVICE_NAME, 0, (struct IORequest *)req, 0);
    }

    if (open_err) {
        printf("Could not open device! Error code: %d\n", open_err);
        printf("Did you copy virtioscsi.device to DEVS: or are you running from the correct directory?\n");
        IExec->FreeSysObject(ASOT_IOREQUEST, req);
        IExec->FreeSysObject(ASOT_PORT, port);
        return 1;
    }

    printf("Successfully opened %s unit 0!\n\n", DEVICE_NAME);

    // Test 1: NewStyle Device Query
    printf("--- TEST 1: NSCMD_DEVICEQUERY ---\n");
    struct NSDeviceQueryResult ns_query;
    memset(&ns_query, 0, sizeof(ns_query));

    req->io_Command = NSCMD_DEVICEQUERY;
    req->io_Data = &ns_query;
    req->io_Length = sizeof(ns_query);
    req->io_Actual = 0;

    IExec->DoIO((struct IORequest *)req);

    if (req->io_Error == 0) {
        printf("NSCMD_DEVICEQUERY succeeded!\n");
        printf("  Device Type: %u\n", ns_query.DeviceType);
        if (ns_query.DeviceType == NSDEVTYPE_TRACKDISK) {
            printf("  It claims to be a Trackdisk-compatible block device.\n");
        }
    } else {
        printf("NSCMD_DEVICEQUERY failed! Error: %d\n", req->io_Error);
    }
    printf("\n");

    // Test 2: SCSI Inquiry
    printf("--- TEST 2: SCSI INQUIRY ---\n");
    UBYTE inquiry_data[36];
    memset(inquiry_data, 0, sizeof(inquiry_data));

    UBYTE scsi_cmd_inq[6] = {0x12, 0, 0, 0, 36, 0}; // Opcode 0x12 (INQUIRY), allocation length 36

    struct SCSICmd scsi;
    memset(&scsi, 0, sizeof(scsi));
    scsi.scsi_Data = (UWORD *)inquiry_data;
    scsi.scsi_Length = sizeof(inquiry_data);
    scsi.scsi_Command = scsi_cmd_inq;
    scsi.scsi_CmdLength = 6;
    scsi.scsi_Flags = SCSIF_READ;

    req->io_Command = HD_SCSICMD;
    req->io_Data = &scsi;
    req->io_Length = sizeof(struct SCSICmd);
    req->io_Actual = 0;

    IExec->DoIO((struct IORequest *)req);

    if (req->io_Error == 0 && scsi.scsi_Status == 0) {
        printf("SCSI INQUIRY succeeded!\n");

        char vendor[9] = {0};
        char product[17] = {0};
        char revision[5] = {0};

        memcpy(vendor, &inquiry_data[8], 8);
        memcpy(product, &inquiry_data[16], 16);
        memcpy(revision, &inquiry_data[32], 4);

        printf("  Vendor  : '%s'\n", vendor);
        printf("  Product : '%s'\n", product);
        printf("  Revision: '%s'\n", revision);
    } else {
        printf("SCSI INQUIRY failed! io_Error: %d, scsi_Status: %d\n", req->io_Error, scsi.scsi_Status);
    }
    printf("\n");

    // Test 3: SCSI Read Capacity (10)
    printf("--- TEST 3: SCSI READ CAPACITY (10) ---\n");
    UBYTE cap_data[8];
    memset(cap_data, 0, sizeof(cap_data));

    UBYTE scsi_cmd_cap[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    memset(&scsi, 0, sizeof(scsi));
    scsi.scsi_Data = (UWORD *)cap_data;
    scsi.scsi_Length = sizeof(cap_data);
    scsi.scsi_Command = scsi_cmd_cap;
    scsi.scsi_CmdLength = 10;
    scsi.scsi_Flags = SCSIF_READ;

    req->io_Command = HD_SCSICMD;
    req->io_Data = &scsi;
    req->io_Length = sizeof(struct SCSICmd);
    req->io_Actual = 0;

    IExec->DoIO((struct IORequest *)req);

    if (req->io_Error == 0 && scsi.scsi_Status == 0) {
        printf("SCSI READ CAPACITY succeeded!\n");

        uint32 blocks = (cap_data[0] << 24) | (cap_data[1] << 16) | (cap_data[2] << 8) | cap_data[3];
        uint32 block_len = (cap_data[4] << 24) | (cap_data[5] << 16) | (cap_data[6] << 8) | cap_data[7];

        printf("  Reported Capacity:\n");
        printf("  Total Blocks : %lu\n", blocks);
        printf("  Block Length : %lu bytes\n", block_len);

        uint64 total_bytes = ((uint64)blocks + 1) * block_len;
        printf("  Total Drive Size: %llu bytes (~%llu MB)\n", total_bytes, total_bytes / (1024 * 1024));

    } else {
        printf("SCSI READ CAPACITY failed! io_Error: %d, scsi_Status: %d\n", req->io_Error, scsi.scsi_Status);
    }
    printf("\n");

    // Clean up
    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);

    printf("Test program completed.\n");

    return 0;
}
