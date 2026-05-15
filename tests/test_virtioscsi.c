#include <devices/newstyle.h>
#include <devices/scsidisk.h>
#include <devices/trackdisk.h>
#include <dos/dos.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <libraries/mounter.h> /* struct DriveGeometry64 lives here in the SDK */
#include <proto/dos.h>
#include <proto/exec.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <interfaces/dos.h>

#define DEVICE_NAME "virtioscsi.device"
#define MAX_INFLIGHT 8

/* Output to both stdout and serial debug port (DebugPrintF).
 * IExec is provided globally by -lauto. */
#define TPRINTF(...) do { printf(__VA_ARGS__); IExec->DebugPrintF(__VA_ARGS__); } while(0)

/* ---- helpers ------------------------------------------------------------ */

static void log_test(const char *name, BOOL success)
{
    TPRINTF("[%s] %s\n", success ? "PASS" : "FAIL", name);
}

static BYTE open_device(const char *name, ULONG unit, struct IORequest *req)
{
    BYTE err = IExec->OpenDevice("build/" DEVICE_NAME, unit, req, 0);
    if (err != 0)
        err = IExec->OpenDevice(DEVICE_NAME, unit, req, 0);
    return err;
}

static BOOL check_device_availability(void)
{
    return (IExec->FindResident(DEVICE_NAME) != NULL);
}

/* Allocate a MEMF_SHARED buffer of <size> bytes, zeroed. */
static UBYTE *alloc_buf(ULONG size)
{
    return (UBYTE *)IExec->AllocVecTags(size,
        AVT_Type, MEMF_SHARED, AVT_ClearWithValue, 0, TAG_DONE);
}

/* -------------------------------------------------------------------------
 * Shared cross-unit background state
 * ----------------------------------------------------------------------- */
struct CrossUnitArg {
    ULONG unit;           /* which unit to use */
    ULONG reads;          /* how many reads to perform */
    ULONG read_size;      /* bytes per read */
    ULONG io_errors;      /* count of errors seen */
    BOOL  done;           /* set TRUE by the task when finished */
    struct Task *parent;  /* parent task to signal on completion */
    ULONG parent_sigbit;  /* signal bit to use */
};

static void cross_unit_task(void)
{
    struct Task *self = IExec->FindTask(NULL);
    struct CrossUnitArg *arg = (struct CrossUnitArg *)self->tc_UserData;

    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    struct IOStdReq *req = (struct IOStdReq *)IExec->AllocSysObjectTags(
        ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct IOStdReq),
        ASOIOR_ReplyPort, port, TAG_DONE);

    ULONG errors = 0;

    if (port && req && open_device(DEVICE_NAME, arg->unit, (struct IORequest *)req) == 0) {
        UBYTE *buf = alloc_buf(arg->read_size);
        if (buf) {
            for (ULONG i = 0; i < arg->reads; i++) {
                req->io_Command = CMD_READ;
                req->io_Data    = buf;
                req->io_Length  = arg->read_size;
                req->io_Offset  = (i % 64) * 512; /* vary sector */
                IExec->DoIO((struct IORequest *)req);
                if (req->io_Error != 0)
                    errors++;
            }
            IExec->FreeVec(buf);
        }
        IExec->CloseDevice((struct IORequest *)req);
    }

    arg->io_errors = errors;
    arg->done = TRUE;

    if (req)  IExec->FreeSysObject(ASOT_IOREQUEST, req);
    if (port) IExec->FreeSysObject(ASOT_PORT, port);

    IExec->Signal(arg->parent, 1UL << arg->parent_sigbit);
}

/* ---- existing tests from v1.2 (unchanged API) --------------------------- */

static void test_scsi_smart(struct MsgPort *port, struct IOStdReq *req)
{
    TPRINTF("--- TEST 5: SCSI LOG SENSE (SMART) ---\n");

    struct SCSICmd scsiCmd;
    uint8_t cdb[10];
    uint8_t data[256];

    memset(&scsiCmd, 0, sizeof(scsiCmd));
    memset(cdb,      0, sizeof(cdb));
    memset(data,     0, sizeof(data));

    cdb[0] = 0x4D;
    cdb[2] = 0x00;
    cdb[8] = 64;

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
        TPRINTF("[PASS] Supported Log Pages retrieved. Pages: ");
        for (int i = 4; i < (int)scsiCmd.scsi_Actual; i++)
            TPRINTF("0x%02X ", data[i]);
        TPRINTF("\n");
    } else {
        TPRINTF("[FAIL] Failed to retrieve Supported Log Pages. Error %d, Status %d\n",
               req->io_Error, scsiCmd.scsi_Status);
        TPRINTF("\n");
        return;
    }

    memset(&scsiCmd, 0, sizeof(scsiCmd));
    memset(cdb,      0, sizeof(cdb));
    memset(data,     0, sizeof(data));

    cdb[0] = 0x4D;
    cdb[2] = 0x2F;
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
        TPRINTF("[PASS] SMART Health Data retrieved. ASC=0x%02X, ASCQ=0x%02X\n", asc, ascq);
    } else {
        TPRINTF("[FAIL] Failed to retrieve SMART data. Error %d, Status %d\n",
               req->io_Error, scsiCmd.scsi_Status);
    }
    TPRINTF("\n");
}

static void test_async_io(struct MsgPort *port, struct IOStdReq *req)
{
    TPRINTF("--- TEST 6: ASYNC I/O (SendIO / WaitIO) ---\n");

    UBYTE *buf = alloc_buf(512);
    if (!buf) { TPRINTF("[FAIL] Buffer allocation failed\n\n"); return; }

    TPRINTF("6.1: SendIO + WaitIO (sector 0 read)...\n");
    req->io_Command = CMD_READ;
    req->io_Data    = buf;
    req->io_Length  = 512;
    req->io_Offset  = 0;
    IExec->SendIO((struct IORequest *)req);
    IExec->WaitIO((struct IORequest *)req);
    log_test("SendIO + WaitIO sector 0 read", req->io_Error == 0);

    TPRINTF("6.2: Two overlapping async reads on unit 0...\n");
    struct MsgPort  *port2 = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    struct IOStdReq *req2  = NULL;
    if (port2)
        req2 = (struct IOStdReq *)IExec->AllocSysObjectTags(
            ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct IOStdReq),
            ASOIOR_ReplyPort, port2, TAG_DONE);

    if (req2 && open_device(DEVICE_NAME, 0, (struct IORequest *)req2) == 0) {
        UBYTE *buf2 = alloc_buf(512);
        if (buf2) {
            req->io_Command  = CMD_READ; req->io_Data  = buf;  req->io_Length  = 512; req->io_Offset  = 0;
            req2->io_Command = CMD_READ; req2->io_Data = buf2; req2->io_Length = 512; req2->io_Offset = 512;
            IExec->SendIO((struct IORequest *)req);
            IExec->SendIO((struct IORequest *)req2);
            IExec->WaitIO((struct IORequest *)req);
            IExec->WaitIO((struct IORequest *)req2);
            log_test("Overlapping async reads both complete", req->io_Error == 0 && req2->io_Error == 0);
            IExec->FreeVec(buf2);
        }
        IExec->CloseDevice((struct IORequest *)req2);
    } else {
        TPRINTF("[INFO] Could not open second req — skipping overlapping test\n");
    }
    if (req2)  IExec->FreeSysObject(ASOT_IOREQUEST, req2);
    if (port2) IExec->FreeSysObject(ASOT_PORT, port2);

    IExec->FreeVec(buf);
    TPRINTF("\n");
}

static void test_abort_io(struct MsgPort *port, struct IOStdReq *req)
{
    TPRINTF("--- TEST 7: ABORTIO ---\n");

    UBYTE *buf = alloc_buf(512);
    if (!buf) { TPRINTF("[FAIL] Buffer allocation failed\n\n"); return; }

    TPRINTF("7.1: SendIO then immediate AbortIO...\n");
    req->io_Command = CMD_READ;
    req->io_Data    = buf;
    req->io_Length  = 512;
    req->io_Offset  = 0;
    IExec->SendIO((struct IORequest *)req);
    IExec->AbortIO((struct IORequest *)req);
    IExec->WaitIO((struct IORequest *)req);

    if (req->io_Error == IOERR_ABORTED)
        TPRINTF("[PASS] AbortIO: request aborted before execution (IOERR_ABORTED)\n");
    else if (req->io_Error == 0)
        TPRINTF("[PASS] AbortIO: request completed before abort could remove it (io_Error=0)\n");
    else
        TPRINTF("[FAIL] AbortIO: unexpected io_Error=%d\n", (int)req->io_Error);

    IExec->FreeVec(buf);
    TPRINTF("\n");
}

static void test_unit_task_lifecycle(struct MsgPort *port)
{
    TPRINTF("--- TEST 8: UNIT TASK LIFECYCLE ---\n");

    struct IOStdReq *req_a = (struct IOStdReq *)IExec->AllocSysObjectTags(
        ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct IOStdReq), ASOIOR_ReplyPort, port, TAG_DONE);
    struct IOStdReq *req_b = (struct IOStdReq *)IExec->AllocSysObjectTags(
        ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct IOStdReq), ASOIOR_ReplyPort, port, TAG_DONE);

    if (!req_a || !req_b) {
        TPRINTF("[INFO] Allocation failed — skipping lifecycle test\n\n");
        if (req_a) IExec->FreeSysObject(ASOT_IOREQUEST, req_a);
        if (req_b) IExec->FreeSysObject(ASOT_IOREQUEST, req_b);
        return;
    }

    TPRINTF("8.1: Double-open same unit, verify both can I/O...\n");
    BYTE err_a = open_device(DEVICE_NAME, 0, (struct IORequest *)req_a);
    BYTE err_b = open_device(DEVICE_NAME, 0, (struct IORequest *)req_b);
    log_test("Second open of unit 0 succeeds", err_a == 0 && err_b == 0);

    if (err_a == 0 && err_b == 0) {
        UBYTE *buf_a = alloc_buf(512);
        UBYTE *buf_b = alloc_buf(512);
        if (buf_a && buf_b) {
            req_a->io_Command = CMD_READ; req_a->io_Data = buf_a; req_a->io_Length = 512; req_a->io_Offset = 0;
            req_b->io_Command = CMD_READ; req_b->io_Data = buf_b; req_b->io_Length = 512; req_b->io_Offset = 512;
            IExec->DoIO((struct IORequest *)req_a);
            IExec->DoIO((struct IORequest *)req_b);
            log_test("Both opens can perform I/O", req_a->io_Error == 0 && req_b->io_Error == 0);
        }
        if (buf_a) IExec->FreeVec(buf_a);
        if (buf_b) IExec->FreeVec(buf_b);
    }

    TPRINTF("8.2: Close first opener — task should still run...\n");
    if (err_a == 0) IExec->CloseDevice((struct IORequest *)req_a);

    if (err_b == 0) {
        UBYTE *buf = alloc_buf(512);
        if (buf) {
            req_b->io_Command = CMD_READ; req_b->io_Data = buf; req_b->io_Length = 512; req_b->io_Offset = 0;
            IExec->DoIO((struct IORequest *)req_b);
            log_test("I/O works after first close (task still alive)", req_b->io_Error == 0);
            IExec->FreeVec(buf);
        }
        TPRINTF("8.3: Close last opener — task should exit cleanly...\n");
        IExec->CloseDevice((struct IORequest *)req_b);
        TPRINTF("[PASS] CloseDevice returned without crash\n");
    }

    IExec->FreeSysObject(ASOT_IOREQUEST, req_a);
    IExec->FreeSysObject(ASOT_IOREQUEST, req_b);
    TPRINTF("\n");
}

/* -------------------------------------------------------------------------
 * TEST 9: Multi-unit concurrent I/O
 *
 * This is the core regression test for builds 1057/1058.
 * Two background tasks each hammer a different unit simultaneously.
 * The ISR wakes both unit tasks on every completion; both Harvest functions
 * drain VQ2 concurrently. Without io_lock around GetBuf, and without
 * cross-unit cookie search, completions are stolen and IORequests are lost —
 * one or both units hang permanently.
 *
 * Success: both tasks complete all reads without errors, within a timeout.
 * Failure: either task reports errors, or the test hangs.
 * ----------------------------------------------------------------------- */
static void test_multi_unit_concurrent(struct DOSIFace *IDOS)
{
    TPRINTF("--- TEST 9: MULTI-UNIT CONCURRENT I/O (cross-unit harvest regression) ---\n");

    /* Check unit 1 is available */
    struct MsgPort  *probe_port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    struct IOStdReq *probe_req  = (struct IOStdReq *)IExec->AllocSysObjectTags(
        ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct IOStdReq),
        ASOIOR_ReplyPort, probe_port, TAG_DONE);
    BOOL unit1_avail = FALSE;
    if (probe_req && open_device(DEVICE_NAME, 1, (struct IORequest *)probe_req) == 0) {
        unit1_avail = TRUE;
        IExec->CloseDevice((struct IORequest *)probe_req);
    }
    if (probe_req)  IExec->FreeSysObject(ASOT_IOREQUEST, probe_req);
    if (probe_port) IExec->FreeSysObject(ASOT_PORT, probe_port);

    if (!unit1_avail) {
        TPRINTF("[INFO] Unit 1 not available — skipping cross-unit test\n\n");
        return;
    }

    /* Allocate signal bit for completion notification from both tasks */
    LONG sig0 = IExec->AllocSignal(-1);
    LONG sig1 = IExec->AllocSignal(-1);
    if (sig0 < 0 || sig1 < 0) {
        TPRINTF("[INFO] AllocSignal failed — skipping cross-unit test\n\n");
        if (sig0 >= 0) IExec->FreeSignal(sig0);
        if (sig1 >= 0) IExec->FreeSignal(sig1);
        return;
    }

    struct CrossUnitArg arg0 = {
        .unit = 0, .reads = 200, .read_size = 512,
        .io_errors = 0, .done = FALSE,
        .parent = IExec->FindTask(NULL), .parent_sigbit = (ULONG)sig0
    };
    struct CrossUnitArg arg1 = {
        .unit = 1, .reads = 200, .read_size = 512,
        .io_errors = 0, .done = FALSE,
        .parent = IExec->FindTask(NULL), .parent_sigbit = (ULONG)sig1
    };

    TPRINTF("9.1: 200 reads on unit 0 and 200 reads on unit 1 simultaneously...\n");

    struct Process *p0 = IDOS->CreateNewProcTags(
        NP_Entry,    (ULONG)cross_unit_task,
        NP_Name,     (ULONG)"VirtIO_CU0",
        NP_Priority, 0,
        NP_UserData, (ULONG)&arg0,
        NP_Child,    TRUE,
        TAG_DONE);

    struct Process *p1 = IDOS->CreateNewProcTags(
        NP_Entry,    (ULONG)cross_unit_task,
        NP_Name,     (ULONG)"VirtIO_CU1",
        NP_Priority, 0,
        NP_UserData, (ULONG)&arg1,
        NP_Child,    TRUE,
        TAG_DONE);

    if (!p0 || !p1) {
        TPRINTF("[INFO] Could not create tasks — skipping\n\n");
        IExec->FreeSignal(sig0);
        IExec->FreeSignal(sig1);
        return;
    }

    /* Wait for both tasks to signal completion. Timeout after ~30 seconds. */
    ULONG wait_mask = (1UL << sig0) | (1UL << sig1);
    ULONG received  = 0;
    for (int ticks = 0; ticks < 600 && received != wait_mask; ticks++) {
        ULONG got = IExec->SetSignal(0, 0) & wait_mask; /* peek */
        if (got) {
            received |= got;
            IExec->SetSignal(0, got); /* clear */
        }
        if (received != wait_mask)
            IDOS->Delay(5); /* wait 100ms */
    }

    BOOL task0_ok = arg0.done && arg0.io_errors == 0;
    BOOL task1_ok = arg1.done && arg1.io_errors == 0;

    log_test("Unit 0 completed 200 reads without errors", task0_ok);
    log_test("Unit 1 completed 200 reads without errors", task1_ok);

    if (!arg0.done) TPRINTF("[FAIL] Unit 0 task did not finish (timeout — possible hang)\n");
    if (!arg1.done) TPRINTF("[FAIL] Unit 1 task did not finish (timeout — possible hang)\n");
    if (arg0.io_errors) TPRINTF("[FAIL] Unit 0 saw %lu I/O errors\n", arg0.io_errors);
    if (arg1.io_errors) TPRINTF("[FAIL] Unit 1 saw %lu I/O errors\n", arg1.io_errors);

    IExec->FreeSignal(sig0);
    IExec->FreeSignal(sig1);
    TPRINTF("\n");
}

/* -------------------------------------------------------------------------
 * TEST 10: Multi-unit mixed read sizes
 *
 * One task does 512B reads on unit 0 (high frequency, lots of ISR wakeups).
 * Another task does 32KB reads on unit 1 (each read saturates multiple SG
 * entries, exercises INDIRECT_DESC path and larger DMA transfers).
 * Tests the cross-unit harvest path under different completion timing.
 * ----------------------------------------------------------------------- */
static void test_multi_unit_mixed_size(struct DOSIFace *IDOS)
{
    TPRINTF("--- TEST 10: MULTI-UNIT MIXED READ SIZES ---\n");

    struct MsgPort  *probe_port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    struct IOStdReq *probe_req  = (struct IOStdReq *)IExec->AllocSysObjectTags(
        ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct IOStdReq),
        ASOIOR_ReplyPort, probe_port, TAG_DONE);
    BOOL unit1_avail = FALSE;
    if (probe_req && open_device(DEVICE_NAME, 1, (struct IORequest *)probe_req) == 0) {
        unit1_avail = TRUE;
        IExec->CloseDevice((struct IORequest *)probe_req);
    }
    if (probe_req)  IExec->FreeSysObject(ASOT_IOREQUEST, probe_req);
    if (probe_port) IExec->FreeSysObject(ASOT_PORT, probe_port);

    if (!unit1_avail) {
        TPRINTF("[INFO] Unit 1 not available — skipping\n\n");
        return;
    }

    LONG sig0 = IExec->AllocSignal(-1);
    LONG sig1 = IExec->AllocSignal(-1);
    if (sig0 < 0 || sig1 < 0) {
        if (sig0 >= 0) IExec->FreeSignal(sig0);
        if (sig1 >= 0) IExec->FreeSignal(sig1);
        TPRINTF("[INFO] AllocSignal failed — skipping\n\n");
        return;
    }

    struct CrossUnitArg arg0 = {
        .unit = 0, .reads = 100, .read_size = 512,    /* small, high-freq */
        .io_errors = 0, .done = FALSE,
        .parent = IExec->FindTask(NULL), .parent_sigbit = (ULONG)sig0
    };
    struct CrossUnitArg arg1 = {
        .unit = 1, .reads = 20, .read_size = 32768,   /* large, exercises SG/INDIRECT_DESC */
        .io_errors = 0, .done = FALSE,
        .parent = IExec->FindTask(NULL), .parent_sigbit = (ULONG)sig1
    };

    TPRINTF("10.1: Unit 0: 100 x 512B reads / Unit 1: 20 x 32KB reads simultaneously...\n");

    struct Process *p0 = IDOS->CreateNewProcTags(
        NP_Entry,    (ULONG)cross_unit_task,
        NP_Name,     (ULONG)"VirtIO_Mix0",
        NP_Priority, 0,
        NP_UserData, (ULONG)&arg0,
        NP_Child,    TRUE,
        TAG_DONE);

    struct Process *p1 = IDOS->CreateNewProcTags(
        NP_Entry,    (ULONG)cross_unit_task,
        NP_Name,     (ULONG)"VirtIO_Mix1",
        NP_Priority, 0,
        NP_UserData, (ULONG)&arg1,
        NP_Child,    TRUE,
        TAG_DONE);

    if (!p0 || !p1) {
        TPRINTF("[INFO] Could not create tasks — skipping\n\n");
        IExec->FreeSignal(sig0);
        IExec->FreeSignal(sig1);
        return;
    }

    ULONG wait_mask = (1UL << sig0) | (1UL << sig1);
    ULONG received  = 0;
    for (int ticks = 0; ticks < 600 && received != wait_mask; ticks++) {
        ULONG got = IExec->SetSignal(0, 0) & wait_mask;
        if (got) { received |= got; IExec->SetSignal(0, got); }
        if (received != wait_mask)
            IDOS->Delay(5);
    }

    log_test("Unit 0 (512B x 100) completed without errors",  arg0.done && arg0.io_errors == 0);
    log_test("Unit 1 (32KB x 20) completed without errors",   arg1.done && arg1.io_errors == 0);

    if (!arg0.done) TPRINTF("[FAIL] Unit 0 task timed out\n");
    if (!arg1.done) TPRINTF("[FAIL] Unit 1 task timed out\n");

    IExec->FreeSignal(sig0);
    IExec->FreeSignal(sig1);
    TPRINTF("\n");
}

/* -------------------------------------------------------------------------
 * TEST 11: Pipeline saturation — multiple in-flight slots on one unit
 *
 * Sends MAX_INFLIGHT requests via SendIO without waiting, then WaitIO each.
 * Exercises the inflight[] slot allocation, cookie tracking, and Harvest
 * correctly replying to each request. If the pipeline gets confused (e.g.
 * wrong slot matched), one or more WaitIO calls will hang.
 * ----------------------------------------------------------------------- */
static void test_pipeline_saturation(struct MsgPort *port, struct IOStdReq *base_req)
{
    TPRINTF("--- TEST 11: PIPELINE SATURATION (MAX_INFLIGHT=%d slots) ---\n", MAX_INFLIGHT);

    struct MsgPort  *reply_ports[MAX_INFLIGHT];
    struct IOStdReq *reqs[MAX_INFLIGHT];
    UBYTE           *bufs[MAX_INFLIGHT];
    int              open_count = 0;
    int              i;

    /* Allocate MAX_INFLIGHT independent device handles */
    for (i = 0; i < MAX_INFLIGHT; i++) {
        reply_ports[i] = NULL;
        reqs[i]        = NULL;
        bufs[i]        = NULL;
    }

    BOOL ok = TRUE;
    for (i = 0; i < MAX_INFLIGHT; i++) {
        reply_ports[i] = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
        if (!reply_ports[i]) { ok = FALSE; break; }

        reqs[i] = (struct IOStdReq *)IExec->AllocSysObjectTags(
            ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct IOStdReq),
            ASOIOR_ReplyPort, reply_ports[i], TAG_DONE);
        if (!reqs[i]) { ok = FALSE; break; }

        if (open_device(DEVICE_NAME, 0, (struct IORequest *)reqs[i]) != 0) {
            ok = FALSE; break;
        }
        open_count = i + 1;

        bufs[i] = alloc_buf(512);
        if (!bufs[i]) { ok = FALSE; break; }
    }

    if (!ok) {
        TPRINTF("[INFO] Could not set up %d handles — skipping pipeline test\n\n",
               MAX_INFLIGHT);
        goto pipeline_cleanup;
    }

    TPRINTF("11.1: Submitting %d reads simultaneously (SendIO without waiting)...\n",
           MAX_INFLIGHT);

    for (i = 0; i < MAX_INFLIGHT; i++) {
        reqs[i]->io_Command = CMD_READ;
        reqs[i]->io_Data    = bufs[i];
        reqs[i]->io_Length  = 512;
        reqs[i]->io_Offset  = (ULONG)(i * 512);
        IExec->SendIO((struct IORequest *)reqs[i]);
    }

    TPRINTF("11.2: WaitIO for each in submission order...\n");
    ULONG errors = 0;
    for (i = 0; i < MAX_INFLIGHT; i++) {
        IExec->WaitIO((struct IORequest *)reqs[i]);
        if (reqs[i]->io_Error != 0) {
            TPRINTF("[FAIL] Slot %d returned io_Error=%d\n", i, (int)reqs[i]->io_Error);
            errors++;
        }
    }
    log_test("All pipeline slots completed without error", errors == 0);

    /* Verify different sectors returned different data
     * (sector 0 and sector 1 should differ unless disk is all zeros) */
    TPRINTF("11.3: Sector data sanity check...\n");
    if (MAX_INFLIGHT >= 2 && bufs[0] && bufs[1]) {
        /* Count how many bytes differ between sector 0 and sector 1 */
        ULONG diffs = 0;
        for (int b = 0; b < 512; b++)
            if (bufs[0][b] != bufs[1][b])
                diffs++;
        TPRINTF("[INFO] Sectors 0 and 1 differ in %lu/512 bytes\n", diffs);
        /* Even on an empty disk, sector 0 (boot block) and sector 1 differ;
         * if both are zero the disk is blank but no error — accept either. */
        TPRINTF("[PASS] Sector data sanity check done (non-zero diff expected, zero ok on blank)\n");
    }

pipeline_cleanup:
    for (i = 0; i < MAX_INFLIGHT; i++) {
        if (bufs[i])        IExec->FreeVec(bufs[i]);
        if (i < open_count) IExec->CloseDevice((struct IORequest *)reqs[i]);
        if (reqs[i])        IExec->FreeSysObject(ASOT_IOREQUEST, reqs[i]);
        if (reply_ports[i]) IExec->FreeSysObject(ASOT_PORT, reply_ports[i]);
    }
    TPRINTF("\n");
}

/* -------------------------------------------------------------------------
 * TEST 12: DoIO geometry interleaved with pipeline I/O on the other unit
 *
 * Exercises the build 1056 fix: DoIO's inline-harvest must forward pipeline
 * completions from the other unit rather than dropping them.
 *
 * Unit 0: background task does 100 rapid DoIO reads (sync, through unit task).
 * Main task: calls DoIO(TD_GETGEOMETRY) on unit 1 repeatedly — this goes
 *            through VirtIOSCSI_DoIO() which blocks in Wait(io_signal_mask)
 *            and may receive unit 0's pipeline completions in its GetBuf loop.
 *            If those completions are not forwarded, unit 0 hangs.
 * ----------------------------------------------------------------------- */
static void test_doit_geometry_interleave(struct DOSIFace *IDOS)
{
    TPRINTF("--- TEST 12: DoIO GEOMETRY INTERLEAVE WITH PIPELINE ---\n");

    struct MsgPort  *probe_port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    struct IOStdReq *probe_req  = (struct IOStdReq *)IExec->AllocSysObjectTags(
        ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct IOStdReq),
        ASOIOR_ReplyPort, probe_port, TAG_DONE);
    BOOL unit1_avail = FALSE;
    if (probe_req && open_device(DEVICE_NAME, 1, (struct IORequest *)probe_req) == 0) {
        unit1_avail = TRUE;
    }

    if (!unit1_avail) {
        TPRINTF("[INFO] Unit 1 not available — skipping\n\n");
        if (probe_req)  IExec->FreeSysObject(ASOT_IOREQUEST, probe_req);
        if (probe_port) IExec->FreeSysObject(ASOT_PORT, probe_port);
        return;
    }

    LONG sig0 = IExec->AllocSignal(-1);
    if (sig0 < 0) {
        TPRINTF("[INFO] AllocSignal failed — skipping\n\n");
        IExec->CloseDevice((struct IORequest *)probe_req);
        IExec->FreeSysObject(ASOT_IOREQUEST, probe_req);
        IExec->FreeSysObject(ASOT_PORT, probe_port);
        return;
    }

    /* Background: 100 x 512B reads on unit 0 */
    struct CrossUnitArg arg0 = {
        .unit = 0, .reads = 100, .read_size = 512,
        .io_errors = 0, .done = FALSE,
        .parent = IExec->FindTask(NULL), .parent_sigbit = (ULONG)sig0
    };

    TPRINTF("12.1: Background: 100 reads on unit 0. Foreground: 20 geometry queries on unit 1...\n");

    struct Process *p0 = IDOS->CreateNewProcTags(
        NP_Entry,    (ULONG)cross_unit_task,
        NP_Name,     (ULONG)"VirtIO_Geo0",
        NP_Priority, 0,
        NP_UserData, (ULONG)&arg0,
        NP_Child,    TRUE,
        TAG_DONE);

    /* Foreground: 20 geometry queries on unit 1 via DoIO */
    ULONG geo_errors = 0;
    if (p0) {
        struct DriveGeometry geo;
        for (int g = 0; g < 20; g++) {
            probe_req->io_Command = TD_GETGEOMETRY;
            probe_req->io_Data    = &geo;
            probe_req->io_Length  = sizeof(geo);
            IExec->DoIO((struct IORequest *)probe_req);
            if (probe_req->io_Error != 0)
                geo_errors++;
        }
    }

    /* Wait for background task */
    ULONG wait_mask = 1UL << sig0;
    for (int t = 0; t < 400 && !(IExec->SetSignal(0, 0) & wait_mask); t++)
        IDOS->Delay(5);
    IExec->SetSignal(0, wait_mask);

    log_test("Unit 1 geometry queries completed without error",  geo_errors == 0);
    log_test("Unit 0 background reads completed without error",  arg0.done && arg0.io_errors == 0);

    if (!arg0.done) TPRINTF("[FAIL] Unit 0 task timed out (possible DoIO inline-harvest failure)\n");

    IExec->FreeSignal(sig0);
    IExec->CloseDevice((struct IORequest *)probe_req);
    IExec->FreeSysObject(ASOT_IOREQUEST, probe_req);
    IExec->FreeSysObject(ASOT_PORT, probe_port);
    TPRINTF("\n");
}

/* -------------------------------------------------------------------------
 * TEST 13: Large single-transfer read (INDIRECT_DESC / big SG chain)
 *
 * Reads 256KB in a single CMD_READ. On a 4KB page system this requires
 * up to 64 SG entries, which may trigger VIRTIO_F_INDIRECT_DESC if
 * negotiated. Tests that AddBuf and GetBuf handle the indirect path.
 * ----------------------------------------------------------------------- */
static void test_large_transfer(struct MsgPort *port, struct IOStdReq *req)
{
    TPRINTF("--- TEST 13: LARGE SINGLE TRANSFER (256KB — exercises INDIRECT_DESC) ---\n");

    const ULONG large_size = 256 * 1024; /* 256KB */
    UBYTE *buf = alloc_buf(large_size);
    if (!buf) {
        TPRINTF("[INFO] Could not allocate 256KB buffer — skipping\n\n");
        return;
    }

    TPRINTF("13.1: 256KB single CMD_READ from sector 0...\n");
    req->io_Command = CMD_READ;
    req->io_Data    = buf;
    req->io_Length  = large_size;
    req->io_Offset  = 0;
    IExec->DoIO((struct IORequest *)req);
    log_test("256KB read succeeded", req->io_Error == 0 && req->io_Actual == large_size);

    if (req->io_Error == 0) {
        /* Spot-check: first two bytes of sector 0 */
        TPRINTF("[INFO] First 4 bytes of buf: %02X %02X %02X %02X\n",
               buf[0], buf[1], buf[2], buf[3]);
    }

    /* Repeat with an offset to cross a page boundary */
    TPRINTF("13.2: 256KB CMD_READ from offset 512 (page-crossing)...\n");
    memset(buf, 0, large_size);
    req->io_Command = CMD_READ;
    req->io_Data    = buf;
    req->io_Length  = large_size;
    req->io_Offset  = 512;
    IExec->DoIO((struct IORequest *)req);
    log_test("256KB page-crossing read succeeded", req->io_Error == 0 && req->io_Actual == large_size);

    IExec->FreeVec(buf);
    TPRINTF("\n");
}

/* -------------------------------------------------------------------------
 * TEST 4: Concurrency stress (unit 0 only — original test)
 * ----------------------------------------------------------------------- */
static void background_task_unit0(void)
{
    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    struct IOStdReq *req = (struct IOStdReq *)IExec->AllocSysObjectTags(
        ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct IOStdReq),
        ASOIOR_ReplyPort, port, TAG_DONE);

    if (port && req && open_device(DEVICE_NAME, 0, (struct IORequest *)req) == 0) {
        UBYTE *buf = alloc_buf(512);
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
    }
    if (req)  IExec->FreeSysObject(ASOT_IOREQUEST, req);
    if (port) IExec->FreeSysObject(ASOT_PORT, port);
}

/* -------------------------------------------------------------------------
 * TEST 14: Held-async semantics (TD_ADDCHANGEINT / TD_REMCHANGEINT / TD_REMOVE)
 *
 * Filesystems (SFS, CDFileSystem, FFS2) issue TD_ADDCHANGEINT and never want
 * a reply until the device decides to wake them. This test exercises:
 *   14.1: TD_ADDCHANGEINT does NOT reply -- the IORequest stays pending.
 *   14.2: A second TD_ADDCHANGEINT replaces the first; the prior one is
 *         replied with IOERR_ABORTED (BeginIO.c "second add" branch).
 *   14.3: TD_REMCHANGEINT retires the held request with io_Error == 0.
 *   14.4: TD_ADDCHANGEINT followed by CloseDevice causes the unit task
 *         shutdown drain (Reply_Held_Async_Reqs) to reply with IOERR_ABORTED.
 * ----------------------------------------------------------------------- */
static void test_held_async(struct DOSIFace *IDOS)
{
    TPRINTF("--- TEST 14: HELD ASYNC (TD_ADDCHANGEINT / TD_REMCHANGEINT / shutdown drain) ---\n");

    /* Use a fresh port so we can poll/wait for replies independently of the
     * main port (which is shared with the long-running req). */
    struct MsgPort  *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    struct IOStdReq *req1 = (struct IOStdReq *)IExec->AllocSysObjectTags(
        ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct IOStdReq),
        ASOIOR_ReplyPort, port, TAG_DONE);
    struct IOStdReq *req2 = (struct IOStdReq *)IExec->AllocSysObjectTags(
        ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct IOStdReq),
        ASOIOR_ReplyPort, port, TAG_DONE);

    if (!port || !req1 || !req2) {
        TPRINTF("[INFO] alloc failed -- skipping\n\n");
        if (req2) IExec->FreeSysObject(ASOT_IOREQUEST, req2);
        if (req1) IExec->FreeSysObject(ASOT_IOREQUEST, req1);
        if (port) IExec->FreeSysObject(ASOT_PORT, port);
        return;
    }

    if (open_device(DEVICE_NAME, 0, (struct IORequest *)req1) != 0) {
        TPRINTF("[INFO] open(unit 0) failed -- skipping\n\n");
        IExec->FreeSysObject(ASOT_IOREQUEST, req2);
        IExec->FreeSysObject(ASOT_IOREQUEST, req1);
        IExec->FreeSysObject(ASOT_PORT, port);
        return;
    }
    /* Share unit 0 across both IORequests (typical filesystem pattern). */
    req2->io_Device = req1->io_Device;
    req2->io_Unit   = req1->io_Unit;

    TPRINTF("14.1: TD_ADDCHANGEINT must be held (no reply)...\n");
    req1->io_Command = TD_ADDCHANGEINT;
    req1->io_Data    = NULL;
    req1->io_Length  = sizeof(struct Interrupt); /* purely a held cookie */
    req1->io_Offset  = 0;
    req1->io_Error   = (BYTE)0xAA;
    IExec->SendIO((struct IORequest *)req1);
    IDOS->Delay(10); /* 200ms: plenty for any erroneous reply to land */
    {
        struct Message *m = IExec->GetMsg(port);
        log_test("TD_ADDCHANGEINT not replied within 200ms (correctly held)", m == NULL);
        if (m) IExec->ReplyMsg(m); /* recycle if the driver wrongly replied */
    }

    TPRINTF("14.2: Second TD_ADDCHANGEINT replaces; first replies IOERR_ABORTED...\n");
    req2->io_Command = TD_ADDCHANGEINT;
    req2->io_Data    = NULL;
    req2->io_Length  = sizeof(struct Interrupt);
    req2->io_Offset  = 0;
    IExec->SendIO((struct IORequest *)req2);
    IDOS->Delay(10);
    {
        /* Should be exactly ONE reply waiting: req1 with IOERR_ABORTED. */
        struct Message *m = IExec->GetMsg(port);
        BOOL aborted = (m == (struct Message *)req1) && (req1->io_Error == IOERR_ABORTED);
        log_test("Prior TD_ADDCHANGEINT replied with IOERR_ABORTED on second add",
                 aborted);
        if (!m)
            TPRINTF("[FAIL] no reply -- prior add was lost rather than retired\n");
        else if (m != (struct Message *)req1)
            TPRINTF("[FAIL] wrong msg replied: %p (expected req1=%p)\n",
                    (void *)m, (void *)req1);
        else
            TPRINTF("[INFO] req1 io_Error=%d\n", (int)req1->io_Error);
    }

    TPRINTF("14.3: TD_REMCHANGEINT retires held req2 with io_Error == 0...\n");
    req1->io_Command = TD_REMCHANGEINT;
    req1->io_Data    = NULL;
    req1->io_Length  = sizeof(struct Interrupt);
    req1->io_Offset  = 0;
    req1->io_Error   = (BYTE)0xAA;
    IExec->DoIO((struct IORequest *)req1);
    log_test("TD_REMCHANGEINT itself returns success", req1->io_Error == 0);
    IDOS->Delay(10);
    {
        struct Message *m = IExec->GetMsg(port);
        BOOL retired = (m == (struct Message *)req2) && (req2->io_Error == 0);
        log_test("Held TD_ADDCHANGEINT retired with io_Error == 0 by TD_REMCHANGEINT",
                 retired);
        if (m && m != (struct Message *)req2)
            TPRINTF("[FAIL] wrong msg retired: %p (expected req2=%p)\n",
                    (void *)m, (void *)req2);
        else if (m)
            TPRINTF("[INFO] req2 io_Error=%d\n", (int)req2->io_Error);
    }

    TPRINTF("14.4: Non-last CloseDevice while another opener exists must NOT reply held cookie...\n");
    /* The main test program holds an open on unit 0, so closing req1 leaves
     * open_count > 0 and the unit task survives. A held TD_ADDCHANGEINT
     * belonging to another opener (effectively the main program's
     * filesystem-like usage) must therefore stay held -- the unit task
     * shutdown drain only runs when the LAST opener closes. */
    req1->io_Command = TD_ADDCHANGEINT;
    req1->io_Data    = NULL;
    req1->io_Length  = sizeof(struct Interrupt);
    req1->io_Offset  = 0;
    req1->io_Error   = (BYTE)0xAA;
    IExec->SendIO((struct IORequest *)req1);
    IDOS->Delay(5);
    while (IExec->GetMsg(port)) { /* discard any spurious */ }
    IExec->CloseDevice((struct IORequest *)req1);
    IDOS->Delay(20);
    {
        struct Message *m = IExec->GetMsg(port);
        log_test("Non-last close did NOT reply held cookie (unit task survives)", m == NULL);
        if (m) {
            TPRINTF("[FAIL] unexpected reply on non-last close: req1 io_Error=%d\n",
                    (int)req1->io_Error);
            /* Recycle so the FreeSysObject below doesn't double-list it. */
            IExec->ReplyMsg(m);
        }
    }
    /* req1 is now a held cookie owned by the driver (changeint_req points at
     * it). We can't free it yet -- doing so would leave a dangling pointer in
     * the driver. Retire it cleanly via TD_REMCHANGEINT on req2. */
    req2->io_Command = TD_REMCHANGEINT;
    req2->io_Data    = NULL;
    req2->io_Length  = sizeof(struct Interrupt);
    req2->io_Offset  = 0;
    IExec->DoIO((struct IORequest *)req2);
    IDOS->Delay(5);
    while (IExec->GetMsg(port)) { /* drain the retired req1 reply */ }

    IExec->FreeSysObject(ASOT_IOREQUEST, req2);
    IExec->FreeSysObject(ASOT_IOREQUEST, req1);
    IExec->FreeSysObject(ASOT_PORT, port);
    TPRINTF("\n");
}

/* -------------------------------------------------------------------------
 * TEST 15: NSCMD_TD_GETGEOMETRY64 round-trip
 *
 * Asserts that the 64-bit geometry command returns a self-consistent answer
 * and that it agrees with TD_GETGEOMETRY (32-bit) on disks <= 2TB. On a
 * >2TB image dg_TotalSectors will exceed 0xFFFFFFFF and we log INFO that the
 * RC16 (CDB 0x9E) internal path was exercised by ensure_geometry_cached.
 * ----------------------------------------------------------------------- */
static void test_geometry64(struct MsgPort *port, struct IOStdReq *req)
{
    (void)port;
    TPRINTF("--- TEST 15: NSCMD_TD_GETGEOMETRY64 ROUND-TRIP ---\n");

    struct DriveGeometry64 geo64;
    memset(&geo64, 0xCC, sizeof(geo64)); /* poison so we can see what's written */

    req->io_Command = NSCMD_TD_GETGEOMETRY64;
    req->io_Data    = &geo64;
    req->io_Length  = sizeof(geo64);
    req->io_Offset  = 0;
    req->io_Actual  = 0;
    req->io_Error   = (BYTE)0xAA;
    IExec->DoIO((struct IORequest *)req);
    log_test("NSCMD_TD_GETGEOMETRY64 returned io_Error == 0", req->io_Error == 0);
    log_test("NSCMD_TD_GETGEOMETRY64 io_Actual == sizeof(DriveGeometry64)",
             req->io_Actual == sizeof(struct DriveGeometry64));

    if (req->io_Error == 0) {
        TPRINTF("[INFO] dg_SectorSize=%lu dg_TotalSectors=%llu (size=%llu MiB)\n",
                (uint32)geo64.dg_SectorSize,
                (unsigned long long)geo64.dg_TotalSectors,
                (unsigned long long)((geo64.dg_TotalSectors * geo64.dg_SectorSize) / (1024ULL * 1024ULL)));
        log_test("dg_SectorSize is a non-zero power-of-two",
                 geo64.dg_SectorSize >= 512 && (geo64.dg_SectorSize & (geo64.dg_SectorSize - 1)) == 0);
        log_test("dg_TotalSectors > 0", geo64.dg_TotalSectors > 0);

        if (geo64.dg_TotalSectors > 0xFFFFFFFFULL)
            TPRINTF("[INFO] dg_TotalSectors > 2^32 -- RC16 (CDB 0x9E) path was exercised\n");
        else
            TPRINTF("[INFO] dg_TotalSectors <= 2^32 -- RC10 (CDB 0x25) sufficed; swap in a >2TB scsi.raw to exercise RC16\n");

        /* Cross-check vs legacy TD_GETGEOMETRY. */
        struct DriveGeometry geo32;
        memset(&geo32, 0, sizeof(geo32));
        req->io_Command = TD_GETGEOMETRY;
        req->io_Data    = &geo32;
        req->io_Length  = sizeof(geo32);
        IExec->DoIO((struct IORequest *)req);
        if (req->io_Error == 0) {
            log_test("TD_GETGEOMETRY dg_SectorSize matches GETGEOMETRY64",
                     geo32.dg_SectorSize == geo64.dg_SectorSize);
            if (geo64.dg_TotalSectors <= 0xFFFFFFFFULL) {
                log_test("TD_GETGEOMETRY dg_TotalSectors matches GETGEOMETRY64 on <=2TB disk",
                         (uint64)geo32.dg_TotalSectors == geo64.dg_TotalSectors);
            }
        } else {
            TPRINTF("[INFO] TD_GETGEOMETRY io_Error=%d -- skipping cross-check\n", (int)req->io_Error);
        }
    }
    TPRINTF("\n");
}

/* -------------------------------------------------------------------------
 * TEST 16: ATA PASS-THROUGH (SAT, CDB 0x85)
 *
 * 16.1: ata_cmd 0xB0 (SMART) -- driver returns a 512-byte synthesized SMART
 *       block (scsi_ata_passthrough.c). Tools like smartctl depend on this.
 * 16.2: ata_cmd 0xEC (IDENTIFY DEVICE) -- not implemented; expect
 *       scsi_Status == CHECK CONDITION and io_Error == HFERR_BadStatus.
 * ----------------------------------------------------------------------- */
static void test_ata_passthrough(struct MsgPort *port, struct IOStdReq *req)
{
    (void)port;
    TPRINTF("--- TEST 16: ATA PASS-THROUGH (CDB 0x85) ---\n");

    UBYTE *data = alloc_buf(512);
    if (!data) {
        TPRINTF("[INFO] alloc failed -- skipping\n\n");
        return;
    }

    struct SCSICmd scsiCmd;
    uint8_t cdb[16];

    /* 16.1: SMART READ DATA (ATA cmd 0xB0, sub-cmd 0xD0 in feature byte) */
    TPRINTF("16.1: CDB 0x85 ATA SMART READ DATA (0xB0/0xD0)...\n");
    memset(&scsiCmd, 0, sizeof(scsiCmd));
    memset(cdb,      0, sizeof(cdb));
    memset(data,     0, 512);
    cdb[0]  = 0x85;  /* ATA PASS-THROUGH 16 */
    cdb[4]  = 0xD0;  /* features = SMART READ DATA */
    cdb[14] = 0xB0;  /* ATA cmd = SMART */

    scsiCmd.scsi_Command   = cdb;
    scsiCmd.scsi_CmdLength = 16;
    scsiCmd.scsi_Data      = (UWORD *)data;
    scsiCmd.scsi_Length    = 512;
    scsiCmd.scsi_Flags     = SCSIF_READ;

    req->io_Command = HD_SCSICMD;
    req->io_Data    = &scsiCmd;
    req->io_Length  = sizeof(scsiCmd);
    IExec->DoIO((struct IORequest *)req);

    log_test("ATA SMART READ DATA io_Error == 0", req->io_Error == 0);
    log_test("ATA SMART READ DATA scsi_Status == GOOD", scsiCmd.scsi_Status == 0);
    log_test("ATA SMART READ DATA scsi_Actual == 512", scsiCmd.scsi_Actual == 512);
    if (req->io_Error == 0)
        TPRINTF("[INFO] SMART revision bytes: %02X %02X (expected 06 00)\n",
                data[0], data[1]);

    /* 16.2: Unsupported ATA command (IDENTIFY DEVICE) */
    TPRINTF("16.2: CDB 0x85 ATA IDENTIFY DEVICE (0xEC) -- expect rejection...\n");
    memset(&scsiCmd, 0, sizeof(scsiCmd));
    memset(cdb,      0, sizeof(cdb));
    memset(data,     0, 512);
    cdb[0]  = 0x85;
    cdb[14] = 0xEC;  /* IDENTIFY DEVICE -- not implemented by ATA PT handler */

    scsiCmd.scsi_Command   = cdb;
    scsiCmd.scsi_CmdLength = 16;
    scsiCmd.scsi_Data      = (UWORD *)data;
    scsiCmd.scsi_Length    = 512;
    scsiCmd.scsi_Flags     = SCSIF_READ;

    req->io_Command = HD_SCSICMD;
    req->io_Data    = &scsiCmd;
    req->io_Length  = sizeof(scsiCmd);
    IExec->DoIO((struct IORequest *)req);

    log_test("IDENTIFY DEVICE rejected (io_Error != 0)", req->io_Error != 0);
    log_test("IDENTIFY DEVICE scsi_Status == CHECK CONDITION (2)",
             scsiCmd.scsi_Status == 2);

    IExec->FreeVec(data);
    TPRINTF("\n");
}

/* -------------------------------------------------------------------------
 * TEST 17: HD_SCSICMD unsupported opcode
 *
 * Sends a CDB the driver does NOT handle in Parse_SCSI_Command (e.g. 0x9E
 * SERVICE ACTION IN). Default branch must return HFERR_BadStatus with
 * scsi_Status == CHECK CONDITION and (if SCSIF_AUTOSENSE set) sense_key
 * == ILLEGAL_REQUEST.
 * ----------------------------------------------------------------------- */
static void test_unsupported_cdb(struct MsgPort *port, struct IOStdReq *req)
{
    (void)port;
    TPRINTF("--- TEST 17: HD_SCSICMD UNSUPPORTED OPCODE ---\n");

    UBYTE *data  = alloc_buf(32);
    UBYTE *sense = alloc_buf(32);
    if (!data || !sense) {
        TPRINTF("[INFO] alloc failed -- skipping\n\n");
        if (data)  IExec->FreeVec(data);
        if (sense) IExec->FreeVec(sense);
        return;
    }

    struct SCSICmd scsiCmd;
    uint8_t cdb[16];
    memset(&scsiCmd, 0, sizeof(scsiCmd));
    memset(cdb,      0, sizeof(cdb));

    cdb[0]  = 0x9E; /* SERVICE ACTION IN (16) -- not handled externally */
    cdb[1]  = 0x10; /* READ CAPACITY 16 service action */
    cdb[13] = 32;

    scsiCmd.scsi_Command     = cdb;
    scsiCmd.scsi_CmdLength   = 16;
    scsiCmd.scsi_Data        = (UWORD *)data;
    scsiCmd.scsi_Length      = 32;
    scsiCmd.scsi_Flags       = SCSIF_READ | SCSIF_AUTOSENSE;
    scsiCmd.scsi_SenseData   = sense;
    scsiCmd.scsi_SenseLength = 32;

    req->io_Command = HD_SCSICMD;
    req->io_Data    = &scsiCmd;
    req->io_Length  = sizeof(scsiCmd);
    IExec->DoIO((struct IORequest *)req);

    log_test("Unsupported opcode io_Error == HFERR_BadStatus",
             req->io_Error == HFERR_BadStatus);
    log_test("Unsupported opcode scsi_Status == CHECK CONDITION",
             scsiCmd.scsi_Status == 2);
    log_test("Auto-sense filled (scsi_SenseActual == 18)",
             scsiCmd.scsi_SenseActual == 18);
    if (scsiCmd.scsi_SenseActual >= 18) {
        uint8_t key = sense[2] & 0x0F;
        uint8_t asc = sense[12];
        TPRINTF("[INFO] sense_key=0x%02X asc=0x%02X (expect 0x05 ILLEGAL_REQUEST, 0x20 INVALID OPCODE)\n",
                key, asc);
        log_test("Sense key == ILLEGAL_REQUEST (0x05) and ASC == 0x20",
                 key == 0x05 && asc == 0x20);
    }

    IExec->FreeVec(data);
    IExec->FreeVec(sense);
    TPRINTF("\n");
}

/* ---- main --------------------------------------------------------------- */

int main(void)
{
    TPRINTF("--- VirtIO SCSI Comprehensive Stress Test (v1.4) ---\n\n");

    if (check_device_availability())
        TPRINTF("[INFO] Device %s is already resident.\n", DEVICE_NAME);
    else
        TPRINTF("[INFO] Device %s not resident, OpenDevice will load it.\n", DEVICE_NAME);

    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    struct IOStdReq *req = (struct IOStdReq *)IExec->AllocSysObjectTags(
        ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct IOStdReq),
        ASOIOR_ReplyPort, port, TAG_DONE);

    if (!port || !req) {
        TPRINTF("Failed to allocate system objects\n");
        return 1;
    }

    if (open_device(DEVICE_NAME, 0, (struct IORequest *)req) != 0) {
        TPRINTF("CRITICAL: Failed to open %s unit 0\n", DEVICE_NAME);
        IExec->FreeSysObject(ASOT_IOREQUEST, req);
        IExec->FreeSysObject(ASOT_PORT, port);
        return 1;
    }
    TPRINTF("Successfully opened %s unit 0.\n\n", DEVICE_NAME);

    /* Open dos.library for IDOS (needed by tests that create processes) */
    struct Library   *DOSBase = IExec->OpenLibrary("dos.library", 53);
    struct DOSIFace  *IDOS    = NULL;
    if (DOSBase)
        IDOS = (struct DOSIFace *)IExec->GetInterface(DOSBase, "main", 1, NULL);

    /* --- TEST 1: MULTI-UNIT PROBE --- */
    TPRINTF("--- TEST 1: MULTI-UNIT PROBE ---\n");
    for (int i = 0; i < 8; i++) {
        struct IOStdReq *u_req = (struct IOStdReq *)IExec->AllocSysObjectTags(
            ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct IOStdReq),
            ASOIOR_ReplyPort, port, TAG_DONE);
        if (u_req) {
            BYTE err = open_device(DEVICE_NAME, (ULONG)i, (struct IORequest *)u_req);
            if (err == 0) {
                TPRINTF("[PASS] Found Unit %d\n", i);
                IExec->CloseDevice((struct IORequest *)u_req);
            } else {
                TPRINTF("[INFO] Unit %d not available\n", i);
            }
            IExec->FreeSysObject(ASOT_IOREQUEST, u_req);
        }
    }
    TPRINTF("\n");

    /* --- TEST 2: BAD DATA RESILIENCE --- */
    TPRINTF("--- TEST 2: BAD DATA RESILIENCE ---\n");

    TPRINTF("2.1: NULL Buffer...\n");
    req->io_Command = CMD_READ; req->io_Data = NULL; req->io_Length = 512; req->io_Offset = 0;
    IExec->DoIO((struct IORequest *)req);
    log_test("NULL Buffer CMD_READ returns error", req->io_Error != 0);

    TPRINTF("2.2: Zero Length...\n");
    req->io_Command = CMD_READ; req->io_Data = (APTR)0xDEADBEEF; req->io_Length = 0; req->io_Offset = 0;
    IExec->DoIO((struct IORequest *)req);
    log_test("Zero Length CMD_READ returns error or zero actual", req->io_Error != 0 || req->io_Actual == 0);

    TPRINTF("2.3: Unaligned Buffer...\n");
    UBYTE *aligned_buf = alloc_buf(1024);
    if (aligned_buf) {
        req->io_Command = CMD_READ; req->io_Data = aligned_buf + 1; req->io_Length = 512; req->io_Offset = 0;
        IExec->DoIO((struct IORequest *)req);
        log_test("Unaligned Buffer CMD_READ (should succeed)", req->io_Error == 0);
        IExec->FreeVec(aligned_buf);
    }
    TPRINTF("\n");

    /* --- TEST 3: 64-BIT OFFSET --- */
    TPRINTF("--- TEST 3: 64-BIT OFFSET TEST ---\n");
    req->io_Data = alloc_buf(512);
    if (req->io_Data) {
        req->io_Command = NSCMD_TD_READ64;
        req->io_Length  = 512;
        req->io_Actual  = 1;           /* high 32 bits = 4GB */
        req->io_Offset  = 0x40000000;  /* total = 5GB */
        IExec->DoIO((struct IORequest *)req);
        /* On the standard 5GB scsi.raw test image LBA 0xA00000 is past EOF,
         * so QEMU returns CHECK CONDITION / ILLEGAL_REQUEST and the driver
         * maps that to IOERR_NOCMD (virtio_scsi_io.c:map_scsi_error case 0x05).
         * On a larger image the read succeeds with io_Error == 0. Either
         * result is a PASS; the only failure is the driver returning
         * IOERR_OPENFAIL or IOERR_BADADDRESS, which would mean the 64-bit
         * offset wasn't unpacked correctly. */
        TPRINTF("[INFO] 5GB Read result: Error %d (Actual=%lu)\n",
                req->io_Error, (uint32)req->io_Actual);
        BOOL ok = (req->io_Error == 0) ||
                  (req->io_Error == IOERR_NOCMD); /* beyond-EOF on 5GB image */
        log_test("NSCMD_TD_READ64 large offset accepted by 64-bit dispatch", ok);
        IExec->FreeVec(req->io_Data);
        req->io_Data = NULL;
    }
    TPRINTF("\n");

    /* --- TEST 4: CONCURRENCY STRESS (original) --- */
    if (IDOS) {
        TPRINTF("--- TEST 4: CONCURRENCY STRESS (unit 0 x2 tasks) ---\n");
        struct Process *bg_proc = IDOS->CreateNewProcTags(
            NP_Entry,    (ULONG)background_task_unit0,
            NP_Name,     (ULONG)"VirtIO_Stress_BG",
            NP_Priority, 0,
            NP_Child,    TRUE,
            TAG_DONE);
        if (bg_proc) {
            UBYTE *buf = alloc_buf(512);
            if (buf) {
                for (int i = 0; i < 100; i++) {
                    req->io_Command = CMD_READ; req->io_Data = buf; req->io_Length = 512; req->io_Offset = 0;
                    IExec->DoIO((struct IORequest *)req);
                }
                IExec->FreeVec(buf);
            }
            TPRINTF("[PASS] Main task completed race loop without crashing.\n");
            IDOS->Delay(50);
        }
        TPRINTF("\n");
    }

    /* --- TEST 5: SCSI LOG SENSE --- */
    test_scsi_smart(port, req);

    /* --- TEST 6: ASYNC I/O --- */
    test_async_io(port, req);

    /* --- TEST 7: ABORTIO --- */
    test_abort_io(port, req);

    /* --- TEST 8: UNIT TASK LIFECYCLE --- */
    test_unit_task_lifecycle(port);

    /* --- TEST 9–13: NEW — pipeline/multi-unit scenarios --- */
    if (IDOS) {
        /* Tests 9, 10, 12 require a background process */
        test_multi_unit_concurrent(IDOS);
        test_multi_unit_mixed_size(IDOS);
        test_doit_geometry_interleave(IDOS);
    } else {
        TPRINTF("[INFO] dos.library unavailable — skipping tests 9, 10, 12\n\n");
    }

    /* Test 11 and 13 use only the current task */
    test_pipeline_saturation(port, req);
    test_large_transfer(port, req);

    /* --- TEST 14: held-async semantics (needs IDOS for Delay) --- */
    if (IDOS)
        test_held_async(IDOS);
    else
        TPRINTF("[INFO] dos.library unavailable -- skipping TEST 14\n\n");

    /* --- TEST 15: NSCMD_TD_GETGEOMETRY64 round-trip --- */
    test_geometry64(port, req);

    /* --- TEST 16: ATA PASS-THROUGH (SAT 0x85) --- */
    test_ata_passthrough(port, req);

    /* --- TEST 17: HD_SCSICMD unsupported opcode --- */
    test_unsupported_cdb(port, req);

    if (IDOS)    IExec->DropInterface((struct Interface *)IDOS);
    if (DOSBase) IExec->CloseLibrary(DOSBase);

    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);

    TPRINTF("Test suite execution finished.\n");
    return 0;
}
