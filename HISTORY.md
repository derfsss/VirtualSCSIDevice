# Project History & Changelog: VirtualSCSIDevice

This file contains a persistent timeline of the development steps and decisions made for this driver, aiming to provide a complete build history.

## [WIP] Initial Project Setup
- Created `VirtualSCSIDevice` project folder.
- Configured directory layout (`src/`, `include/`) modeled after `AmigaDiskBench`.
- Written initial cross-compilation `Makefile`.
- Created placeholder files for Knowledge Base, Agent Handover, and History.
## Phase 1: V1 Dummy SCSI Device Driver
- Implemented core boilerplate: `Init`, `Open`, `Close`, `Expunge` functions based on `TestDevice`.
- Fixed linker issue causing initial ISI crash by switching from `-nostdlib -Wl,-Ttext=0` to `-nostartfiles`.
- Built modular `BeginIO` dispatcher to separate `scsi_cmds`, `ns_cmds`, and `exec_cmds` into separate folders.
- Spoofed 1GB SCSI Drive by intercepting `SCSI_INQUIRY` and `SCSI_READ_CAPACITY_10`.
- Implemented dummy 1MB RAM buffer inline to safely handle block reads (`CMD_READ`, `NSCMD_TD_READ64`) and writes.
- Reviewed AmigaOS NSD documentation and fully satisfied all requirements for `NSDEVTYPE_TRACKDISK` (implementing `TD_MOTOR`, `TD_GETGEOMETRY`, `NSCMD_TD_FORMAT64`, `NSCMD_TD_SEEK64`, etc.).
- Successfully formatted the dummy device locally via `AmigaDOS Mount` command without crashing.

## Phase 2: QEMU VirtIO PCI Discovery
- Created `src/pci/` isolated architecture to keep AmigaOS PCI enumeration completely separated from VirtIO protocol logic.
- Hooked `expansion.library` opens and closes into standard `Init` and `Expunge` vectors.
- Implemented `pci_discovery.c` to gracefully probe the AmigaOS 4 PCI bus for Vendor ID `0x1AF4` and Device ID `0x1004`.
- Extracted and safely mapped the QEMU AmigaOne base registers for VirtIO SCSI (BAR 0 mapped to Legacy I/O `0x00800500`, BAR 4 mapped to MMIO `0x84204000`).
- Diagnosed and fixed core DSI crashes related to OS4 varargs calling conventions (`TAG_DONE` requirement) and `GetInterface` naming mismatches.

## Phase 3: VirtIO Component Initialization
- Implemented full Legacy VirtIO PCI init sequence (Reset → ACK → DRIVER → DRIVER_OK = 0x07).
- Allocated and configured 3 virtqueues (controlq, eventq, requestq) with page-aligned DMA buffers.
- Discovered vring fields use native guest endian (Big Endian on PPC) — no byte-swapping needed.

## Phase 4: Real VirtIO I/O
- Created `VirtIOSCSI_DoIO()` synchronous I/O function with descriptor chain management.
- Wired SCSI INQUIRY, READ CAPACITY(10), READ(10), WRITE(10) to real VirtIO I/O.
- Fixed critical DMA issue: used `CachePreDMA()` for VA→PA translation (virtual ≠ physical on PPC MMU).
- Implemented UNIT ATTENTION auto-retry for transient sense key 0x06.

## Phase 5: Write Persistence & Format
- Wired `CMD_READ`/`CMD_WRITE` to VirtIO SCSI READ(10)/WRITE(10) commands.
- Replaced dummy 1MB RAM buffer with real disk I/O.
- Implemented `TD_GETGEOMETRY` using SCSI READ CAPACITY(10) with cached results.
- Fixed CHS geometry: H=4, S=16 to match QEMU IDE (avoids capacity truncation).
- Wired `TD_FORMAT`/`ETD_FORMAT`/`TD_FORMAT64` to write handler (was the root cause of format checksum errors).
- Discovered `CachePostDMA()` crashes with DSI from user mode — removed (not needed in QEMU VM).
- Successfully formatted partition with FFS2 filesystem.

## Phase 6: Core Command Implementation
- Implemented `CMD_UPDATE`/`CMD_FLUSH` using SCSI SYNCHRONIZE CACHE(10).
- Implemented `TD_CHANGESTATE` — returns 0 (disk present) for non-removable VirtIO disk.
- Implemented `TD_PROTSTATUS` — returns 0 (writable).

## Phase 7: Complete Command Coverage
- Implemented legacy 64-bit I/O (`TD_READ64`, `TD_WRITE64`) via new `cmd_td_io64.c`.
- Wired all missing ETD commands: `ETD_READ`, `ETD_WRITE`, `ETD_UPDATE`, `ETD_CLEAR`, `ETD_SEEK`.
- Wired all NSCMD_ETD commands: `NSCMD_ETD_READ64`, `NSCMD_ETD_WRITE64`, `NSCMD_ETD_SEEK64`, `NSCMD_ETD_FORMAT64`.
- Fixed `TD_CHANGENUM` to return `io_Actual = 0` (fixed media, never changed).
- Added `SCSIF_AUTOSENSE` stub using SDK `<scsi/sense_codes.h>` defines (18-byte fixed-format sense, ILLEGAL_REQUEST).
- Updated `SupportedCommands` array in `NSCMD_DEVICEQUERY` to advertise all implemented commands.
- Made expected no-ops (TD_MOTOR, TD_REMOVE, etc.) silent — no more debug log spam.


## Phase 8: DMA Reliability & Workbench Stability
- Migrated from obsolete `CachePreDMA` to modern V50+ DMA API (`StartDMA`, `GetDMAList`, `EndDMA`).
- Implemented robust Scatter-Gather list building to handle fragmented physical memory mappings.
- Resolved "Silent Data Corruption" during large SFS/FFS2 file transfers.
- Fixed critical Workbench DSI crash by moving unit lifecycle management from `Open`/`Close` to `Init`/`Expunge`.
- Added explicit zeroing of `io_Actual` on all failure paths to enhance OS robustness.

## Phase 9: Multi-Unit Automounting
- Extended driver to support up to 8 concurrent disk units.
- Implemented SCSI `INQUIRY` discovery loop during `Init()` to find all connected drives.
- Integrated with `mounter.library` to automatically announce and mount every discovered drive at boot.
-### Phase 9: Fixed Kickstart Race Condition
- Adjusted resident priority to -60 to initialize after `mounter.library`.
- Reordered `Init()` to call `AddDevice()` before unit discovery loop and `AnnounceDevice()`.
- Verified `AnnounceDevice` successfully opens the public device and probes units.
- Updated all I/O command paths (Standard, NSD, SCSI Direct) to support dynamic target routing.
- Verified stable operation with multiple disks on AmigaOS 4.1 Workbench.

### Phase 11: Stability & Multi-Disk Success
- **Resolved Early-Boot Hang**: Identified a race condition in `mounter.library` caused by redundant manual `AddDevice` calls. Resolved by removing manual registration and relying on the `RTF_AUTOINIT` kernel sequence.
- **Implemented I/O Synchronization**: Added a `SignalSemaphore` (`io_lock`) to `VirtIOSCSI_DoIO` to protect shared hardware queues from concurrent OS probes (e.g., parallel partition mounting).
- **Standardized Drive Identity**: Updated `TD_GETDRIVETYPE` to return `DRIVE_NEWSTYLE`. This corrects the 64-bit geometry handling and prevents legacy OS components from misidentifying VirtIO disks as floppy drives.
- **Improved Geometry Reliability**: 
    - Fixed `TD_GETNUMTRACKS` to return a non-zero fallback (32768) if geometry isn't cached, preventing "empty drive" errors.
    - Enhanced `BeginIO` tracing with Task Names (`FindTask(NULL)`) and physical offsets for precise diagnostic correlation.
- **Fixed Multi-Unit Routing Bugs**: Corrected a long-standing issue where background commands (`TD_ADDCHANGEINT`, `TD_REMOVE`) were hardcoded to unit 0. They now correctly use `ioreq->io_Unit`.
- **Verified Successful Boot**: Achieved stable system startup with multiple VirtIO SCSI disks on QEMU AmigaOne. All partitions mount automatically without freezing.

## Phase 12: v1.2 Release & Stress Testing
- **Driver Version 1.2 (Build 1029)**: Officially bumped version and revision.
- **Centralized Debug Suppression**: Implemented `DPRINTF` macro across over 40 driver-side logging points. Logging is now off by default, allowing the test suite output to remain clear and focused.
- **Advanced Bug Incubator**: Developed `test_virtioscsi.c` to proactively find edge-case failures.
- **Resilience Verification**:
  - **Bad Data**: Confirmed the driver handles NULL buffers, zero-length requests, and unaligned memory without crashing.
  - **64-bit Logic**: Verified correct 64-bit offset handling for disk images > 4GB.
  - **Concurrency Stress**: Spawned multi-threaded race conditions (parallel I/O vs. geometry queries) to validate `io_lock` semaphore stability. 
- **Build Success**: Verified clean build using the `os4-gcc11` cross-compiler.
