# VirtualSCSIDevice Development Roadmap

## Context
The `virtioscsi.device` v1.3 (Build 1033) is a working, production-ready AmigaOS 4.1 FE VirtIO SCSI device driver. It builds, boots systems, and passes stress testing. The driver now features interrupt-driven I/O and a full async device-task architecture. This roadmap tracks completed and future work.

---

## Phase 1: CDB Helpers & Block Size Fix
**Priority: High | Risk: Low | Effort: Small**

### Problem
- READ/WRITE(10) CDB construction was copy-pasted across 6 files
- 64-bit offset unpacking was duplicated in 2 files
- Geometry caching logic was duplicated in 2 files
- All read/write handlers hardcoded 512-byte block size instead of using `unit->block_size` — breaks on 4K sector disks

### Changes

**New file: `src/scsi_cdb_helpers.c`** + **`include/scsi_cdb_helpers.h`**
- `void make_read10_cdb(uint8 *cdb, uint32 lba, uint16 blocks)` — builds a 10-byte READ CDB
- `void make_write10_cdb(uint8 *cdb, uint32 lba, uint16 blocks)` — builds a 10-byte WRITE CDB
- `uint64 unpack_io64_offset(uint32 io_Actual, uint32 io_Offset)` — inline helper combining high/low 32-bit halves
- `int32 ensure_geometry_cached(struct VirtIOSCSIBase *base, struct VirtIOUSCSIDevUnit *unit)` — issues READ CAPACITY(10) if `!unit->geometry_valid`, caches result

**Modified files:**
- `src/exec_cmds/cmd_read.c` — uses `make_read10_cdb()`, uses `unit->block_size` with fallback to 512
- `src/exec_cmds/cmd_write.c` — uses `make_write10_cdb()`, uses `unit->block_size` with fallback to 512
- `src/exec_cmds/cmd_td_io64.c` — uses helpers, uses `unit->block_size`
- `src/ns_cmds/ns_td_io64.c` — uses helpers, uses `unit->block_size`
- `src/exec_cmds/cmd_td_getgeometry.c` — uses `ensure_geometry_cached()`
- `src/ns_cmds/ns_td_getgeometry64.c` — uses `ensure_geometry_cached()`

---

## Phase 2: Consolidate Thin Stub Files
**Priority: Medium | Risk: Low | Effort: Small**

### Problem
Several files contained a single `return 0` statement. Two SCSI files were identical except for one opcode byte. One header was empty.

### Changes

**Merged stubs → `src/exec_cmds/cmd_stubs.c`:**
- `Handle_TD_ChangeState()`, `Handle_TD_ProtStatus()`, `Handle_TD_GetDriveType()`, `Handle_CMD_Success()`
- Deleted: `cmd_td_changestate.c`, `cmd_td_protstatus.c`, `cmd_td_getdrivetype.c`, `cmd_success.c`

**Merged SCSI R/W → `src/scsi_cmds/scsi_rw_10.c`:**
- Shared `handle_scsi_rw10()` static helper with `is_write` parameter
- `Handle_SCSI_Read10()` and `Handle_SCSI_Write10()` are thin wrappers
- Deleted: `scsi_read_10.c`, `scsi_write_10.c`

**Deleted `include/device.h`** — empty file, not included anywhere

---

## Phase 3: Split Init.c — Extract Unit Discovery
**Priority: Medium | Risk: Low | Effort: Small-Medium**

### Problem
`Init.c` (188 lines) mixed library setup, PCI discovery, VirtIO init, INQUIRY scanning, unit allocation, and mounter announcement in one function.

### Changes

**New file: `src/unit_discovery.c`** + **`include/unit_discovery.h`**
- `uint32 DiscoverUnits(struct VirtIOSCSIBase *devBase)` — INQUIRY scan loop (targets 0-7, LUNs 0-7), unit struct allocation, mounter.library announcement
- Self-contained: opens/closes mounter.library internally, allocates/frees INQUIRY buffer

**Simplified `src/Init.c`:**
- Library/interface setup, PCI discovery, VirtIO init, device registration
- Single call to `DiscoverUnits(devBase)` replaces ~100 lines of inline code
- Removed includes for `mounter.h` and `virtio_scsi_io.h` (no longer needed)

---

## Phase 4: BeginIO Dispatch Cleanup
**Priority: Medium | Risk: Low | Effort: Small**

### Problem
- `GetCommandName()` was a 72-line string lookup embedded in BeginIO.c
- The `supported_commands[]` array in `ns_devicequery.c` duplicated the command list

### Changes

**New file: `src/cmd_names.c`** + **`include/cmd_names.h`**
- `const char *GetCommandName(uint32 cmd)` — data-driven lookup table
- `const uint16 supported_commands[]` — single source of truth, zero-terminated

**Modified files:**
- `src/BeginIO.c` — removed 72-line inline function, includes `cmd_names.h`
- `src/ns_cmds/ns_devicequery.c` — removed local `supported_commands[]`, references shared table via `cmd_names.h`

---

## Phase 5: Interrupt-Driven I/O
**Priority: High | Risk: Medium | Effort: Medium**

### Problem
`VirtIOSCSI_DoIO()` busy-waits in a tight polling loop (5M iterations), burning 100% CPU during every I/O operation. The `VRING_AVAIL_F_NO_INTERRUPT` flag is set, so the device never fires interrupts.

### Changes

**New file: `src/virtio/virtio_irq.c`** + **`include/virtio/virtio_irq.h`**
- `VirtIOSCSI_InterruptHandler()` — reads ISR register (acknowledges interrupt), calls `IExec->Signal()` on waiting task
- Returns non-zero (claims interrupt) or zero (shared IRQ, not ours) to the exec interrupt chain
- `InstallVirtIOInterrupt(base)` — maps PCI IRQ via `pciDevice->MapInterrupt()`, installs via `IExec->AddIntServer()`
- `RemoveVirtIOInterrupt(base)` — removes handler via `IExec->RemIntServer()` during expunge

**Modified files:**
- `include/virtioscsi.h` — added `struct Interrupt irq_handler`, `irq_number`, `irq_installed`, `io_wait_task`, `io_signal_mask` to `VirtIOSCSIBase`; added `#include <exec/interrupts.h>`
- `src/virtio/virtqueue.c` — cleared `VRING_AVAIL_F_NO_INTERRUPT` flag in `VirtQueue_Allocate()` to enable device interrupts
- `src/virtio/virtio_scsi_io.c` — replaced 5M-iteration polling loop with `IExec->AllocSignal()` + `IExec->Wait()` + `IExec->FreeSignal()`. Polling fallback retained if interrupts unavailable or `AllocSignal` fails.
- `src/Init.c` — call `InstallVirtIOInterrupt()` after VirtIO init (non-fatal: logs warning and continues in polling mode on failure)
- `src/Expunge.c` — call `RemoveVirtIOInterrupt()` before `CleanupVirtIOSCSI()` to prevent handler firing on freed resources

### Key Design Decisions
- Use legacy PCI INTx — `pciDevice->MapInterrupt()` returns the vector, `AddIntServer()` installs on the shared chain
- Per-call `AllocSignal(-1)` in the calling task — signal bits are task-local; the semaphore ensures only one waiter at a time
- Pre-sleep `GetBuf()` check eliminates the race between `Kick()` and `Wait()` — device may have already completed
- `io_wait_task = NULL` before `FreeSignal()` prevents the handler signalling a task that has moved on
- Retry loop (50 × `Wait()`) handles spurious wakeups from ISR bit 1 (config change) with a practical timeout

---

## Phase 6: Async I/O
**Priority: Medium | Risk: Medium | Effort: Medium**

### Changes

**New files: `src/unit_task.c`** + **`include/unit_task.h`**
- Per-unit exec task owning a `struct MsgPort` for IORequest queuing
- `UnitTask_Entry()` — task entry point; allocates message port, signals parent when ready, runs event loop `Wait(io_port_mask | SIGBREAKF_CTRL_C)`, drains queue with `IOERR_ABORTED` on shutdown
- `UnitTask_Start()` — creates task via `CreateTaskTags()`, sets `tc_UserData` under `Forbid()` to pass stack-allocated start message, waits for port-ready signal
- `UnitTask_Shutdown()` — sets `task_shutdown = TRUE`, signals `SIGBREAKF_CTRL_C`, busy-waits for `unit->task` to become NULL
- `UnitTask_Dispatch()` — switch statement routing all I/O commands to existing handlers

**Modified: `src/BeginIO.c`**
- Inline quick commands (no-ops, change notification, `NSCMD_DEVICEQUERY`) — handled without unit task
- Slow I/O commands (`CMD_READ`, `CMD_WRITE`, `HD_SCSICMD`, 64-bit variants, etc.) — `PutMsg` to `unit->io_port`, clear `IOF_QUICK`, return immediately
- Real `AbortIO` implementation: `Forbid()/Permit()` removes request from message port queue if still pending

**Modified: `include/virtioscsi.h`**
- Added per-unit fields: `task`, `io_port`, `io_port_mask`, `task_shutdown`, `io_wait_task`, `io_signal_mask`, `io_cookie`
- Removed `io_wait_task`/`io_signal_mask` from `VirtIOSCSIBase` (moved to unit)

**Modified: `src/virtio/virtio_scsi_io.c`**
- `VirtIOSCSI_DoIO()` takes `struct VirtIOUSCSIDevUnit *unit` parameter (NULL for discovery)
- Semaphore (`io_lock`) now covers only `AddBuf+Kick` window — tasks can sleep concurrently
- Interrupt path uses `unit->io_wait_task`/`io_signal_mask`/`io_cookie` (per-unit, no contention)

**Modified: `src/virtio/virtio_irq.c`**
- ISR scans all 8 units and signals any with `io_wait_task` set (per-unit fields, no global)

**Modified: `src/Open.c`, `src/Close.c`, `src/Expunge.c`**
- `Open`: `UnitTask_Start()` on first open; `Close`: `UnitTask_Shutdown()` on last close; `Expunge`: `UnitTask_Shutdown()` for all units before freeing

### Key Design Decisions
- One exec task per unit (not one per request) — simple lifetime management, no task pool needed
- `Forbid()` + `tc_UserData` write before `Permit()` — safe parameter passing to new task
- Lock scope narrowed to submit window — multiple units can have in-flight VirtIO requests simultaneously
- `NULL` unit in `DoIO` during discovery bypasses interrupt path, falls back to polling

---

## Phase 7: Performance Optimisations
**Priority: High | Risk: Low | Effort: Medium**

### Problem
- Per-request `AllocVecTags` + `FreeVec` for req_cmd/resp_cmd (2 allocs + 2 frees per I/O)
- Per-request `StartDMA`/`GetDMAList`/`EndDMA` for req and resp buffers (~10-15us per request)
- `CachePreDMA` (deprecated since AmigaOS 4.1) used in `virtio_init.c`
- `VIRTIO_F_EVENT_IDX` not negotiated — redundant PCI `QUEUE_NOTIFY` writes under load
- GCC 11 `-ftree-loop-distribute-patterns` at `-O2` replaces fill loops with `memset()` (newlib, not linked in device drivers)

### Changes

**`src/virtio/virtio_init.c`:** Replace deprecated `CachePreDMA` with `StartDMA`+`GetDMAList`; negotiate `VIRTIO_F_EVENT_IDX` (bit 29).

**`include/virtio/virtqueue.h` + `src/virtio/virtqueue.c`:** Added `dma_phys/dma_entries` (live vring DMA mapping, freed in `VirtQueue_Free`); added `use_event_idx/last_kick_avail_idx`; `VirtQueue_Kick` conditionally suppresses `QUEUE_NOTIFY` using `avail_event` threshold.

**`include/virtioscsi.h`:** Added pre-alloc fields to unit struct (`req_buf`, `resp_buf`, DMA lists/entry counts); added `<exec/memory.h>` and `"virtio/virtio_scsi_cmd.h"` includes.

**`src/unit_task.c`:** `preallocate_unit_dma()` / `free_unit_dma()` — allocate `MEMF_SHARED` req/resp buffers with permanent DMA mappings at unit open; free at close. Unit task never allocates on the I/O path.

**`src/virtio/virtio_scsi_io.c`:** Normal path uses pre-allocated unit buffers and cached DMA lists — zero per-request alloc or DMA setup for req/resp. Discovery path (unit==NULL) falls back to temporary allocation. `resp_buf` zeroed via `zero_shared()` (volatile uint32 word loop, safe for non-cacheable `MEMF_SHARED`). Only user data buffer uses per-call `prepare_dma`/`cleanup_dma`.

**`Makefile`:** Added `-fno-tree-loop-distribute-patterns` — prevents GCC 11 from replacing fill loops with `memset()` calls at `-O2`.

### Key Design Decisions
- `volatile uint32` loop for `MEMF_SHARED` zeroing: `ClearMem`/`SetMem` must not be used on non-cacheable memory (exception handler fallback is slow per autodoc).
- `-fno-tree-loop-distribute-patterns` is targeted: disables only the loop-to-libcall transform.
- `VIRTIO_F_EVENT_IDX` event index lives at `used->ring[num]` (past end of used ring entries).

---

## Phase 8: MSI-X Support
**Priority: Lower | Risk: Medium | Effort: Medium**

### Changes
- Upgrade from legacy INTx to MSI-X per-queue interrupt vectors
- Separate interrupt handlers for control, event, and request queues
- Reduces interrupt overhead on shared interrupt lines

---

## Phase 9: Event Queue / Hot-Plug
**Priority: Lower | Risk: Medium | Effort: Large**

### Changes
- Poll or interrupt on `eventq` (queue 1) for VirtIO SCSI transport events
- Handle target added/removed events
- Call `DenounceDevice()` when targets disappear
- Signal held `TD_ADDCHANGEINT` / `TD_REMOVE` requests

---

## Phase 10: v1.5 — Modern VirtIO 1.0 Support
**Priority: High | Risk: Medium | Effort: Medium**

### Context

v1.4 (build 1070) uses VirtIO legacy PCI transport: I/O BAR (device ID 0x1004).
Modern VirtIO (device ID 0x1048, `VIRTIO_F_VERSION_1`) uses MMIO BARs with PCI
capability chains and a LE vring layout.

### Platform Compatibility

Investigation (Feb 2026) confirmed:
- **Pegasos2 (MV64361)**: Full transparent MMIO bridge → Modern VirtIO works
- **AmigaOne (Articia S)**: Floating buffer bridge, no transparent CPU↔PCI memory window → Modern MMIO returns 0, cannot be worked around in software

**MMIO access method** (Pegasos2): `stwbrx`/`lwbrx` inline assembly with `mbar`
barrier. `InWord`/`InLong` silently return 0 for memory BARs on AmigaOS 4.1 FE.
Only `InByte`/`OutByte` work; multi-byte access requires byte-assembly helpers.

### Status

| Build | Item | Status |
|-------|------|--------|
| 1071 | `test_modern.c` probe program — fully validates Modern VirtIO init on Pegasos2 | **Complete** (2026-02-25) |
| 1072 | Auto-detection in driver (`DetectModernVirtIO`), log-only | Pending |
| 1073 | Modern init sequence + LE vring wrappers + ISR dispatch | Pending |
| 1074 | Guards, cleanup, final version bump, release | Pending |

### Key Design Decisions (already validated)
- `test_modern.c` uses `stwbrx`/`lwbrx` macros — these are the canonical MMIO instructions
- Feature negotiation confirmed: driver accepts full offered set (0x30000006/0x00000001)
- Status handshake confirmed: 0x00→0x03→0x0B→0x0F works end-to-end on QEMU Pegasos2
- `VIRTIO_SCSI_F_INOUT` not offered by QEMU — **this does not affect normal disk I/O**. READ(10)/WRITE(10)/READ CAPACITY/INQUIRY are all unidirectional; INOUT is only needed for bidirectional SCSI commands that have simultaneous data-in AND data-out phases (rare, non-disk). The earlier concern was a misreading of VirtIO spec 5.6.6.1.1.
- `test_modern` scans for any `1AF4:xxxx` — may find RNG (0x1044) or other device first; driver init must use `FDT_DeviceID, 0x1048`
- For AmigaOne: continue using legacy device (0x1004) + I/O port interface

### Files
See `docs/v1.5_implementation_plan.md` for the detailed build plan.

---

## Progress Tracking

| Phase | Description | Status | Date |
|-------|-------------|--------|------|
| 1 | CDB helpers & block size fix | Complete | 2026-02-22 |
| 2 | Consolidate thin stubs | Complete | 2026-02-22 |
| 3 | Split Init.c / unit discovery | Complete | 2026-02-22 |
| 4 | BeginIO dispatch cleanup | Complete | 2026-02-22 |
| 5 | Interrupt-driven I/O | Complete | 2026-02-23 |
| 6 | Async I/O | Complete | 2026-02-23 |
| 7 | Performance optimisations | Complete | 2026-02-23 |
| 8 | MSI-X support | Pending | |
| 9 | Event queue / hot-plug | Pending | |
| 10 | Modern VirtIO 1.0 (v1.5) | In Progress | 2026-02-25 |
