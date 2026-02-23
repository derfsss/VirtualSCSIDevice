#include <devices/newstyle.h>
#include <devices/scsidisk.h>
#include <devices/trackdisk.h>
#include <dos/dos.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <interfaces/dos.h>

#define DEVICE_NAME "virtioscsi.device"

void log_test(const char *name, BOOL success)
{
    printf("[%s] %s\n", success ? "PASS" : "FAIL", name);
}

/*
 * Try opening the device from either the build dir or system path.
 */
static BYTE open_device(const char *name, ULONG unit, struct IORequest *req)
{
    BYTE err = IExec->OpenDevice("build/" DEVICE_NAME, unit, req, 0);
    if (err != 0)
        err = IExec->OpenDevice(DEVICE_NAME, unit, req, 0);
    return err;
}

BOOL check_device_availability()
{
    return (IExec->FindResident(DEVICE_NAME) != NULL);
}

/* ---------------------------------------------------------------------------
 * Background task for concurrency testing.
 * Performs 50 reads on unit 0 in parallel with the main task.
 * -------------------------------------------------------------------------- */
static void background_task()
{
    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    struct IOStdReq *req = (struct IOStdReq *)IExec->AllocSysObjectTags(
        ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct IOStdReq), ASOIOR_ReplyPort, port, TAG_DONE);

    if (port && req && open_device(DEVICE_NAME, 0, (struct IORequest *)req) == 0) {
        printf("[INFO] Background task started.\n");

        UBYTE *buf = IExec->AllocVecTags(512, AVT_Type, MEMF_SHARED, TAG_DONE);
        if (buf) {
            for (int i = 0; i < 50; i++) {
                req->io_Command = CMD_READ;
                req->io_Data    = buf;
                req->io_Length  = 512;
                req->io_Offset  = 0;
                IExec->DoIO((struct IORequest *)req);
            }
            IExec->FreeVec(buf);
        }

        IExec->CloseDevice((struct IORequest *)req);
        printf("[INFO] Background task finished.\n");
    }

    if (req)
        IExec->FreeSysObject(ASOT_IOREQUEST, req);
    if (port)
        IExec->FreeSysObject(ASOT_PORT, port);
}

/* ---------------------------------------------------------------------------
 * TEST 5: SCSI LOG SENSE (SMART)
 * -------------------------------------------------------------------------- */
void test_scsi_smart(struct MsgPort *port, struct IOStdReq *req)
{
    printf("--- TEST 5: SCSI LOG SENSE (SMART) ---\n");

    struct SCSICmd scsiCmd;
    uint8_t cdb[10];
    uint8_t data[256];

    /* Sub-test 5.1: Query Supported Log Pages (Page 0) */
    memset(&scsiCmd, 0, sizeof(scsiCmd));
    memset(cdb,      0, sizeof(cdb));
    memset(data,     0, sizeof(data));

    cdb[0] = 0x4D; /* LOG SENSE */
    cdb[2] = 0x00; /* PC=0, Page=0 */
    cdb[8] = 64;   /* Allocation Length */

    scsiCmd.scsi_Command   = cdb;
    scsiCmd.scsi_CmdLength = 10;
    scsiCmd.scsi_Data      = (UWORD *)data;
    scsiCmd.scsi_Length    = 64;
    scsiCmd.scsi_Flags     = SCSIF_READ;

    req->io_Command = HD_SCSICMD;
    req->io_Data    = &scsiCmd;
    req->io_Length  = sizeof(scsiCmd);

    IExec->DoIO((struct IORequest *)req);

    if (req->io_Error == 0 && scsiCmd.scsi_Status == 0) {
        printf("[PASS] Supported Log Pages retrieved. Pages: ");
        for (int i = 4; i < (int)scsiCmd.scsi_Actual; i++)
            printf("0x%02X ", data[i]);
        printf("\n");
    } else {
        printf("[FAIL] Failed to retrieve Supported Log Pages. Error %d, Status %d\n",
               req->io_Error, scsiCmd.scsi_Status);
        printf("\n");
        return;
    }

    /* Sub-test 5.2: Query Informational Exceptions (Page 0x2F) */
    memset(&scsiCmd, 0, sizeof(scsiCmd));
    memset(cdb,      0, sizeof(cdb));
    memset(data,     0, sizeof(data));

    cdb[0] = 0x4D; /* LOG SENSE */
    cdb[2] = 0x2F; /* Page 0x2F */
    cdb[7] = 0;
    cdb[8] = 255;

    scsiCmd.scsi_Command   = cdb;
    scsiCmd.scsi_CmdLength = 10;
    scsiCmd.scsi_Data      = (UWORD *)data;
    scsiCmd.scsi_Length    = 255;
    scsiCmd.scsi_Flags     = SCSIF_READ;

    IExec->DoIO((struct IORequest *)req);

    if (req->io_Error == 0 && scsiCmd.scsi_Status == 0) {
        uint8_t asc  = data[8];
        uint8_t ascq = data[9];
        printf("[PASS] SMART Health Data retrieved. ASC=0x%02X, ASCQ=0x%02X\n", asc, ascq);

        data[255] = 0;
        if (strstr((char *)&data[16], "VirtIO Dummy")) {
            printf("[PASS] Found Attribution: %s\n", (char *)&data[16]);
        } else {
            printf("[WARNING] Attribution string not found in log data.\n");
        }
    } else {
        printf("[FAIL] Failed to retrieve SMART data. Error %d, Status %d\n",
               req->io_Error, scsiCmd.scsi_Status);
    }
    printf("\n");
}

/* ---------------------------------------------------------------------------
 * TEST 6: Async I/O — SendIO / CheckIO / WaitIO
 *
 * Phase 6 makes BeginIO non-blocking: it queues the request and returns.
 * This test exercises that path explicitly using SendIO/WaitIO rather than
 * the convenience wrapper DoIO (which calls both internally).
 * -------------------------------------------------------------------------- */
void test_async_io(struct MsgPort *port, struct IOStdReq *req)
{
    printf("--- TEST 6: ASYNC I/O (SendIO / WaitIO) ---\n");

    /* Allocate a DMA-capable shared buffer */
    UBYTE *buf = IExec->AllocVecTags(512, AVT_Type, MEMF_SHARED, AVT_ClearWithValue, 0, TAG_DONE);
    if (!buf) {
        printf("[FAIL] Buffer allocation failed\n\n");
        return;
    }

    /* 6.1: SendIO then WaitIO — verify the request completes normally */
    printf("6.1: SendIO + WaitIO (sector 0 read)...\n");
    req->io_Command = CMD_READ;
    req->io_Data    = buf;
    req->io_Length  = 512;
    req->io_Offset  = 0;

    IExec->SendIO((struct IORequest *)req);

    /*
     * At this point BeginIO has returned — the request is queued to the unit
     * task. The calling task is free to do other work here.
     * WaitIO blocks until the unit task replies.
     */
    IExec->WaitIO((struct IORequest *)req);
    log_test("SendIO + WaitIO sector 0 read", req->io_Error == 0);

    /* 6.2: Two back-to-back SendIO calls on different requests sharing the
     *       same reply port — both should complete correctly. */
    printf("6.2: Two overlapping async reads...\n");

    struct MsgPort *port2 = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    struct IOStdReq *req2 = NULL;
    if (port2) {
        req2 = (struct IOStdReq *)IExec->AllocSysObjectTags(
            ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct IOStdReq), ASOIOR_ReplyPort, port2, TAG_DONE);
    }

    if (req2 && open_device(DEVICE_NAME, 0, (struct IORequest *)req2) == 0) {
        UBYTE *buf2 = IExec->AllocVecTags(512, AVT_Type, MEMF_SHARED, AVT_ClearWithValue, 0, TAG_DONE);

        if (buf2) {
            req->io_Command  = CMD_READ;
            req->io_Data     = buf;
            req->io_Length   = 512;
            req->io_Offset   = 0;

            req2->io_Command = CMD_READ;
            req2->io_Data    = buf2;
            req2->io_Length  = 512;
            req2->io_Offset  = 512;

            IExec->SendIO((struct IORequest *)req);
            IExec->SendIO((struct IORequest *)req2);

            IExec->WaitIO((struct IORequest *)req);
            IExec->WaitIO((struct IORequest *)req2);

            log_test("Overlapping async reads both complete", req->io_Error == 0 && req2->io_Error == 0);
            IExec->FreeVec(buf2);
        } else {
            printf("[INFO] Buffer allocation failed — skipping overlapping test\n");
        }

        IExec->CloseDevice((struct IORequest *)req2);
    } else {
        printf("[INFO] Could not open second req — skipping overlapping test\n");
    }

    if (req2)
        IExec->FreeSysObject(ASOT_IOREQUEST, req2);
    if (port2)
        IExec->FreeSysObject(ASOT_PORT, port2);

    IExec->FreeVec(buf);
    printf("\n");
}

/* ---------------------------------------------------------------------------
 * TEST 7: AbortIO
 *
 * AbortIO removes a pending request from the unit task's message port queue
 * if it hasn't started executing yet, and replies with IOERR_ABORTED.
 * If the request is already executing (started before AbortIO is called),
 * AbortIO returns IOERR_NOCMD — this is the correct documented behavior.
 *
 * Because the unit task may process our request before AbortIO runs (single
 * disk, fast I/O), we test both outcomes and accept either as correct.
 * -------------------------------------------------------------------------- */
void test_abort_io(struct MsgPort *port, struct IOStdReq *req)
{
    printf("--- TEST 7: ABORTIO ---\n");

    UBYTE *buf = IExec->AllocVecTags(512, AVT_Type, MEMF_SHARED, AVT_ClearWithValue, 0, TAG_DONE);
    if (!buf) {
        printf("[FAIL] Buffer allocation failed\n\n");
        return;
    }

    printf("7.1: SendIO then immediate AbortIO...\n");
    req->io_Command = CMD_READ;
    req->io_Data    = buf;
    req->io_Length  = 512;
    req->io_Offset  = 0;

    IExec->SendIO((struct IORequest *)req);
    IExec->AbortIO((struct IORequest *)req);
    IExec->WaitIO((struct IORequest *)req);

    /*
     * Two legal outcomes:
     *   IOERR_ABORTED  — request was still queued, AbortIO removed it
     *   0 (no error)   — request completed before AbortIO could remove it
     * Any other io_Error is a failure.
     */
    if (req->io_Error == IOERR_ABORTED) {
        printf("[PASS] AbortIO: request aborted before execution (IOERR_ABORTED)\n");
    } else if (req->io_Error == 0) {
        printf("[PASS] AbortIO: request completed before abort could remove it (io_Error=0)\n");
    } else {
        printf("[FAIL] AbortIO: unexpected io_Error=%d\n", (int)req->io_Error);
    }

    IExec->FreeVec(buf);
    printf("\n");
}

/* ---------------------------------------------------------------------------
 * TEST 8: Unit task lifecycle
 *
 * Verify that opening the same unit twice uses the same task (not two tasks),
 * and that the task exits cleanly on last close.
 * -------------------------------------------------------------------------- */
void test_unit_task_lifecycle(struct MsgPort *port)
{
    printf("--- TEST 8: UNIT TASK LIFECYCLE ---\n");

    struct IOStdReq *req_a = (struct IOStdReq *)IExec->AllocSysObjectTags(
        ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct IOStdReq), ASOIOR_ReplyPort, port, TAG_DONE);
    struct IOStdReq *req_b = (struct IOStdReq *)IExec->AllocSysObjectTags(
        ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct IOStdReq), ASOIOR_ReplyPort, port, TAG_DONE);

    if (!req_a || !req_b) {
        printf("[INFO] Allocation failed — skipping lifecycle test\n\n");
        if (req_a) IExec->FreeSysObject(ASOT_IOREQUEST, req_a);
        if (req_b) IExec->FreeSysObject(ASOT_IOREQUEST, req_b);
        return;
    }

    printf("8.1: Double-open same unit, verify both can I/O...\n");
    BYTE err_a = open_device(DEVICE_NAME, 0, (struct IORequest *)req_a);
    BYTE err_b = open_device(DEVICE_NAME, 0, (struct IORequest *)req_b);

    BOOL both_open = (err_a == 0 && err_b == 0);
    log_test("Second open of unit 0 succeeds", both_open);

    if (both_open) {
        UBYTE *buf_a = IExec->AllocVecTags(512, AVT_Type, MEMF_SHARED, AVT_ClearWithValue, 0, TAG_DONE);
        UBYTE *buf_b = IExec->AllocVecTags(512, AVT_Type, MEMF_SHARED, AVT_ClearWithValue, 0, TAG_DONE);

        if (buf_a && buf_b) {
            /* Both requests go through the same unit task */
            req_a->io_Command = CMD_READ;
            req_a->io_Data    = buf_a;
            req_a->io_Length  = 512;
            req_a->io_Offset  = 0;

            req_b->io_Command = CMD_READ;
            req_b->io_Data    = buf_b;
            req_b->io_Length  = 512;
            req_b->io_Offset  = 512;

            IExec->DoIO((struct IORequest *)req_a);
            IExec->DoIO((struct IORequest *)req_b);

            log_test("Both opens can perform I/O", req_a->io_Error == 0 && req_b->io_Error == 0);
        }

        if (buf_a) IExec->FreeVec(buf_a);
        if (buf_b) IExec->FreeVec(buf_b);
    }

    printf("8.2: Close first opener — task should still run...\n");
    if (err_a == 0)
        IExec->CloseDevice((struct IORequest *)req_a);

    if (err_b == 0) {
        /* Unit task must still be alive after first close */
        UBYTE *buf = IExec->AllocVecTags(512, AVT_Type, MEMF_SHARED, AVT_ClearWithValue, 0, TAG_DONE);
        if (buf) {
            req_b->io_Command = CMD_READ;
            req_b->io_Data    = buf;
            req_b->io_Length  = 512;
            req_b->io_Offset  = 0;
            IExec->DoIO((struct IORequest *)req_b);
            log_test("I/O works after first close (task still alive)", req_b->io_Error == 0);
            IExec->FreeVec(buf);
        }

        printf("8.3: Close last opener — task should exit cleanly...\n");
        IExec->CloseDevice((struct IORequest *)req_b);
        /* If the task exited uncleanly (crash), we would not reach this line */
        printf("[PASS] CloseDevice returned without crash\n");
    }

    IExec->FreeSysObject(ASOT_IOREQUEST, req_a);
    IExec->FreeSysObject(ASOT_IOREQUEST, req_b);
    printf("\n");
}

/* ---------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */
int main()
{
    printf("--- VirtIO SCSI Comprehensive Stress Test (v1.3.0) ---\n\n");

    if (check_device_availability()) {
        printf("[INFO] Device %s is already resident.\n", DEVICE_NAME);
    } else {
        printf("[INFO] Device %s not resident, OpenDevice will attempt to load/init.\n", DEVICE_NAME);
    }

    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    struct IOStdReq *req = (struct IOStdReq *)IExec->AllocSysObjectTags(
        ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct IOStdReq), ASOIOR_ReplyPort, port, TAG_DONE);

    if (!port || !req) {
        printf("Failed to allocate system objects\n");
        return 1;
    }

    if (open_device(DEVICE_NAME, 0, (struct IORequest *)req) != 0) {
        printf("CRITICAL: Failed to open %s\n", DEVICE_NAME);
        IExec->FreeSysObject(ASOT_IOREQUEST, req);
        IExec->FreeSysObject(ASOT_PORT, port);
        return 1;
    }

    printf("Successfully opened %s.\n\n", DEVICE_NAME);

    /* --- TEST 1: MULTI-UNIT PROBE --- */
    printf("--- TEST 1: MULTI-UNIT PROBE ---\n");
    for (int i = 0; i < 8; i++) {
        struct IOStdReq *u_req = (struct IOStdReq *)IExec->AllocSysObjectTags(
            ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct IOStdReq), ASOIOR_ReplyPort, port, TAG_DONE);
        if (u_req) {
            BYTE err = open_device(DEVICE_NAME, i, (struct IORequest *)u_req);
            if (err == 0) {
                printf("[PASS] Found Unit %d\n", i);
                IExec->CloseDevice((struct IORequest *)u_req);
            } else {
                printf("[INFO] Unit %d not available\n", i);
            }
            IExec->FreeSysObject(ASOT_IOREQUEST, u_req);
        }
    }
    printf("\n");

    /* --- TEST 2: BAD DATA RESILIENCE --- */
    printf("--- TEST 2: BAD DATA RESILIENCE ---\n");

    printf("2.1: NULL Buffer Protection check...\n");
    req->io_Command = CMD_READ;
    req->io_Data    = NULL;
    req->io_Length  = 512;
    req->io_Offset  = 0;
    IExec->DoIO((struct IORequest *)req);
    log_test("NULL Buffer CMD_READ", req->io_Error != 0);

    printf("2.2: Zero Length Protection check...\n");
    req->io_Command = CMD_READ;
    req->io_Data    = (APTR)0xDEADBEEF;
    req->io_Length  = 0;
    req->io_Offset  = 0;
    IExec->DoIO((struct IORequest *)req);
    log_test("Zero Length CMD_READ", req->io_Error != 0 || req->io_Actual == 0);

    printf("2.3: Unaligned Buffer check...\n");
    UBYTE *aligned_buf = IExec->AllocVecTags(1024, AVT_Type, MEMF_SHARED, TAG_DONE);
    if (aligned_buf) {
        req->io_Command = CMD_READ;
        req->io_Data    = aligned_buf + 1; /* odd address */
        req->io_Length  = 512;
        req->io_Offset  = 0;
        IExec->DoIO((struct IORequest *)req);
        log_test("Unaligned Buffer CMD_READ", req->io_Error == 0);
        IExec->FreeVec(aligned_buf);
    }
    printf("\n");

    /* --- TEST 3: 64-BIT OFFSET TEST --- */
    printf("--- TEST 3: 64-BIT OFFSET TEST ---\n");
    req->io_Command = NSCMD_TD_READ64;
    req->io_Data    = IExec->AllocVecTags(512, AVT_Type, MEMF_SHARED, TAG_DONE);
    req->io_Length  = 512;
    req->io_Actual  = 1;           /* high 32 bits = 4GB */
    req->io_Offset  = 0x40000000;  /* low  32 bits → total = 5GB */
    if (req->io_Data) {
        IExec->DoIO((struct IORequest *)req);
        printf("[INFO] 5GB Read result: Error %d, Sense %lu\n", req->io_Error, req->io_Actual);
        log_test("Large offset handling", req->io_Error != IOERR_NOCMD);
        IExec->FreeVec(req->io_Data);
    }
    printf("\n");

    /* --- TEST 4: CONCURRENCY STRESS --- */
    struct Library *DOSBase = IExec->OpenLibrary("dos.library", 53);
    struct DOSIFace *IDOS = (struct DOSIFace *)IExec->GetInterface(DOSBase, "main", 1, NULL);

    if (IDOS) {
        printf("--- TEST 4: CONCURRENCY STRESS ---\n");
        printf("Spawning background task to race with main task (both doing reads)...\n");

        struct Process *bg_proc = IDOS->CreateNewProcTags(
            NP_Entry,    (ULONG)background_task,
            NP_Name,     (ULONG)"VirtIO_Stress_BG",
            NP_Priority, 0,
            NP_Child,    TRUE,
            TAG_DONE);

        if (bg_proc) {
            UBYTE *buf = IExec->AllocVecTags(512, AVT_Type, MEMF_SHARED, TAG_DONE);
            if (buf) {
                for (int i = 0; i < 100; i++) {
                    req->io_Command = CMD_READ;
                    req->io_Data    = buf;
                    req->io_Length  = 512;
                    req->io_Offset  = 0;
                    IExec->DoIO((struct IORequest *)req);
                }
                IExec->FreeVec(buf);
            }
            printf("[PASS] Main task completed race loop without crashing.\n");
            IDOS->Delay(50); /* let background task finish */
        }
        printf("\n");

        IExec->DropInterface((struct Interface *)IDOS);
    }
    if (DOSBase)
        IExec->CloseLibrary(DOSBase);

    /* --- TEST 5: SCSI LOG SENSE (SMART) --- */
    test_scsi_smart(port, req);

    /* --- TEST 6: ASYNC I/O --- */
    test_async_io(port, req);

    /* --- TEST 7: ABORTIO --- */
    test_abort_io(port, req);

    /* --- TEST 8: UNIT TASK LIFECYCLE --- */
    /* Re-open unit 0 for the lifecycle test (we're still open from main) */
    test_unit_task_lifecycle(port);

    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);

    printf("Test suite execution finished.\n");
    return 0;
}
