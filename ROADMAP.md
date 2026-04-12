# VirtualSCSIDevice Development Roadmap

## Context
The `virtioscsi.device` v1.6 is a working, production-ready AmigaOS 4.1 FE VirtIO SCSI device driver. It builds, boots systems, and passes stress testing. The driver features interrupt-driven I/O, a full async device-task architecture, and dual VirtIO transport (Legacy + Modern). This roadmap tracks completed and future work.

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

## Phase 10: Modern VirtIO 1.0 Support
**Priority: High | Risk: Medium | Effort: Medium**

### Context

Modern VirtIO (device ID 0x1048, `VIRTIO_F_VERSION_1`) uses MMIO BARs with PCI
capability chains and a LE vring layout. Legacy VirtIO (device ID 0x1004) uses
I/O port BAR with native-endian vring.

### Platform Compatibility

Investigation (Feb 2026) confirmed:
- **Pegasos2 (MV64361)**: Full transparent MMIO bridge → Modern VirtIO works
- **AmigaOne (Articia S)**: Floating buffer bridge, no transparent CPU↔PCI memory window → Modern MMIO returns 0, cannot be worked around in software

**MMIO access method** (Pegasos2): `stwbrx`/`lwbrx` inline assembly with `mbar`
barrier. `InWord`/`InLong` silently return 0 for memory BARs on AmigaOS 4.1 FE.
Only `InByte`/`OutByte` work; multi-byte access requires byte-assembly helpers.

### Status: Complete (v1.5, 2026-02-28)

All items implemented and tested on both QEMU amigaone (legacy) and QEMU pegasos2 (modern):
- `test_modern.c` probe program validates Modern VirtIO init on Pegasos2
- Auto-detection in driver (`DetectModernVirtIO`) via PCI capability chain walk
- Modern init sequence with LE vring wrappers and ISR dispatch
- PCI Memory Space + Bus Master enable, NULL-safe BAR0, modern-aware notify

### Key Design Decisions
- `stwbrx`/`lwbrx` macros for canonical PPC MMIO — `InWord`/`InLong` return 0 on MMIO BARs
- Feature negotiation: driver accepts full offered set (0x30000006/0x00000001)
- `VIRTIO_SCSI_F_INOUT` not offered by QEMU — does not affect normal disk I/O
- PCI discovery searches for modern 0x1048 first, falls back to legacy 0x1004
- For AmigaOne: continue using legacy device (0x1004) + I/O port interface

### Files
See `docs/v1.5_implementation_plan.md` for the detailed build plan.

---

## Phase 11: Code Review & Build Hardening
**Priority: Medium | Risk: Low | Effort: Small**

### Status: Complete (v1.6, 2026-03-18)

Comprehensive code review and build system improvements:
- **Bug fix**: CMD_READ/CMD_WRITE rejected sub-block I/O with `IOERR_BADLENGTH` instead of silently rounding up
- **Bug fix**: Redundant semaphore release/re-acquire in DoIO cross-unit stash path merged into single lock hold
- **Build**: Automatic header dependency tracking (`-MMD -MP`), stricter warnings (`-Wextra -Wshadow -Wformat=2`)
- **Cleanup**: Named constant for SAM-2 LUN encoding, consistent header guards, ASCII-only debug output
- **Version**: Removed build number from version string (standard AmigaOS major.minor format)

---

## Phase 12: I/O Throughput Optimisation (v1.7)
**Priority: High | Risk: Low-Medium | Effort: Medium**

Hot-path performance review targeting maximum data throughput for both small-file and large-file workloads. See `docs/performance_plan_v2.md` for full analysis.

### Group A — Bounce Buffer Improvements (highest impact)

1. **Increase bounce buffer 4KB → 64KB**: Eliminates 5 DMA syscalls per request for virtually all filesystem I/O (SFS, FFS2 block reads are ≤64KB). Memory cost: ~1MB MEMF_SHARED per active unit.
2. **Word-aligned bounce copy**: Replace byte-at-a-time volatile copy with uint32 word loop. ~4x faster for aligned transfers (all MEMF_SHARED and MEMF_PUBLIC buffers are word-aligned).
3. **Advertise max transfer size in geometry**: Populate `TD_GETGEOMETRY` so filesystems can issue optimally-sized requests instead of conservative 512B chunks.

### Group B — Per-Request Overhead Reduction

4. **O(1) inflight slot allocation**: Free-list pointer instead of linear O(16) scan in Submit. Harvest uses slot index from `req_cmd->id` for O(1) cookie matching.
5. **Pre-allocated DMA entry arrays**: For >64KB transfers, eliminate per-request `AllocSysObjectTags`/`FreeSysObject`. Pre-allocate one `DMAEntry[MAX_SG_ENTRIES]` per inflight slot at unit startup.
6. **Track inflight occupancy counter**: Replace 128-slot scan in interrupt coalescing with increment/decrement counter.

### Group C — Modern-Only Enhancement

7. **Enable indirect descriptors on modern path**: LE byte-swap wrappers already exist; endian issue only affects legacy. Each request consumes 1 vring descriptor instead of up to 64, allowing full queue concurrency for large transfers.

### Files
- `include/virtioscsi.h` — BOUNCE_BUF_SIZE, free-list fields, DMA pool arrays, occupancy counter
- `src/virtio/virtio_scsi_io.c` — bounce_copy, Submit, Harvest, coalescing
- `src/unit_task.c` — pool allocation/deallocation
- `src/exec_cmds/cmd_td_getgeometry.c`, `src/ns_cmds/ns_td_getgeometry64.c` — max transfer
- `src/virtio/virtio_init.c` — indirect desc negotiation (modern only)
- `src/virtio/virtqueue.c` — indirect path with pre-allocated tables

---

## Phase 13: Unified QEMU Platform Setup ✓ (v1.8)
**Status: COMPLETE — tested on AmigaOne, Pegasos2, and SAM460ex**

Single `-device virtio-scsi-pci` (transitional, device ID 0x1004) works on all three QEMU PowerPC machines. The driver auto-detects modern vs legacy transport at boot via an MMIO probe (pattern borrowed from VirtIOGPU's chip_scan_pci_caps):

- PCI discovery reordered: 0x1004 first, 0x1048 fallback
- MMIO probe: write ACKNOWLEDGE to STATUS, read back, check match
- Pegasos2 (MV64361): probe passes → modern mode
- AmigaOne (Articia S): probe fails → legacy I/O
- SAM460ex: probe fails → legacy I/O

---

## Phase 14: Performance Optimisations ✓ (v1.8)
**Status: COMPLETE**

- Cacheable bounce buffers: CopyMem + CacheClearE replaces volatile non-cacheable copy (~10-20x faster)
- O(1) cross-unit cookie routing via encoded req_cmd->id
- ISR occupancy bitmask skips units with no inflight I/O
- gc-sections reverted (breaks AmigaOS Resident structure)

---

## Phase 15: Boot Drive Support ✓ (v53.8)
**Status: COMPLETE — tested on AmigaOne, Pegasos2, and SAM460ex**

- Resident priority changed from -60 to 0 (matching other disk device drivers)
- Major version bumped to 53 (AmigaOS 4.1 FE SDK convention)
- diskboot.config entry documented: `virtioscsi.device 8 3`
- Kicklayout placement: MODULE line before diskboot.config and diskboot.kmod
- Dynamic build date/time stamps via Makefile
- Confirmed working as boot drive on all three QEMU machines

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
| 10 | Modern VirtIO 1.0 (v1.5) | Complete | 2026-02-28 |
| 11 | Code review & build hardening (v1.6) | Complete | 2026-03-18 |
| 12 | I/O throughput optimisation (v1.7) | Complete | 2026-03-18 |
| 13 | Unified QEMU platform setup (v1.8) | Complete | 2026-04-11 |
| 14 | Performance optimisations (v1.8) | Complete | 2026-04-11 |
| 15 | Boot drive support (v53.8) | Complete | 2026-04-12 |
