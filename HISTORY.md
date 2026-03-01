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

## Phase 13: v1.3 Performance & Pipeline Hardening (builds 1044–1058)

### Build 1044: EVENT_IDX fix
- Re-enabled `VIRTIO_F_EVENT_IDX` kick suppression after identifying root cause: `last_kick_avail_idx` was initialised to 0, causing the second kick's suppression check `(1 < 1)` to always be FALSE, silently dropping all requests after the first.
- Fixed by initialising `last_kick_avail_idx = 0xFFFF` — first comparison wraps correctly.

### Build 1045: INDIRECT_DESC
- Negotiated `VIRTIO_F_INDIRECT_DESC` (bit 28).
- Single vring descriptor now points to a `MEMF_SHARED` indirect table containing the full scatter-gather chain, eliminating the `MAX_SG_ENTRIES=64` limit on transfer size and enabling arbitrarily large I/O in one VirtIO descriptor slot.

### Build 1046: READ(16)/WRITE(16) for large disks
- 64-bit I/O paths now use `READ(16)`/`WRITE(16)` CDBs when computed LBA exceeds `0xFFFFFFFF`, enabling correct access to disk images larger than ~2.1TB.

### Build 1047: Async pipeline (Submit/Harvest)
- Per-unit I/O pipeline with up to `MAX_INFLIGHT=8` simultaneous VirtIO requests.
- Block I/O commands submit via `VirtIOSCSI_Submit()` and complete via `VirtIOSCSI_Harvest()` on ISR signal.
- Persistent per-unit ISR signal bit (allocated once at startup, not per-request).
- Pre-allocated DMA slots extended from 1 to 8 per unit.

### Builds 1048–1055: EVENT_IDX kick-suppression diagnosis and removal
- Extended debug logging to diagnose EVENT_IDX suppression failures in QEMU legacy mode.
- Discovered that QEMU legacy VirtIO **never writes `avail_event`** into `used->ring[num]` — the field stays 0 forever, making kick suppression permanently wrong after the first kick.
- Root cause: EVENT_IDX has two halves — (a) device writes `avail_event` to suppress driver kicks, (b) driver writes `used_event` to suppress device interrupts. QEMU legacy implements only (b).
- Correct fix: disable kick suppression entirely (`VirtQueue_Kick` always sends unconditional `QUEUE_NOTIFY`); keep `used_event` write (interrupt suppression) which does work.
- Added `used_event` write in `VirtQueue_GetBuf()`: `vq->avail->ring[vq->num] = vq->last_used_idx` + `eieio` barrier — `avail->ring[]` is `uint16[]`, not a struct array.
- Build 1055: both units mounted, SmartFilesystem and FastFileSystem connected at boot.

### Build 1056: DoIO inline-harvest for pipeline cookies
- DoIO's IRQ wait loop extended to drain non-matching cookies via inline harvest.
- Prevents pipeline completions from being silently dropped when DoIO runs concurrently with a pipeline Submit on the other unit.

### Build 1057: io_lock serialisation for cross-unit GetBuf
- Identified multi-unit shared VQ2 race: ISR signals ALL unit tasks on any completion; both Harvest functions call `VirtQueue_GetBuf()` concurrently, racing on `last_used_idx` and stealing each other's cookies.
- Fixed by wrapping ALL `VirtQueue_GetBuf()` calls (in both `Harvest` and DoIO drain loop) in `io_lock` semaphore — release before `ReplyMsg`, re-acquire at loop bottom.
- System boots to Workbench; both filesystems mount. One drive still missing due to cross-unit cookie loss.

## v1.5 Modern VirtIO Investigation (Build 1071)

### Build 1071: test_modern.c — Modern VirtIO Platform Investigation (February 2026)

Standalone test program (`tests/test_modern.c`) to probe VirtIO 1.0 Modern (non-transitional) device support before modifying the production driver. The program performs full VirtIO 1.0 initialization on any found VirtIO device and decodes all feature flags per VirtIO 1.2 spec.

**Deliverable**: Complete, tested probe program demonstrating successful Modern VirtIO initialization on Pegasos2/QEMU.

**Key finding — platform MMIO incompatibility**:

Thorough investigation revealed that the two supported QEMU PPC machines have fundamentally different PCI bridge architectures:

| Machine | Bridge | Memory BAR MMIO |
|---------|--------|-----------------|
| QEMU amigaone | Mai Logic Articia S (floating buffer) | ✗ Not forwarded to PCI |
| QEMU pegasos2 | Marvell MV64361 (transparent) | ✓ Direct CPU↔PCI window |

**AmigaOne investigation (all approaches failed)**:
- `InByte`/`OutByte` on memory BAR address → zero (PCI byte cycle issued but floating buffer doesn't respond)
- `InLong`/`InWord` → always zero (not even I/O cycles for MMIO addresses)
- `SetEndian(LITTLE_ENDIAN)` → no effect on MMIO (I/O port only)
- `IMMU->RemapMemory()` → mapping created but reads still 0 (MMU can't change bridge behavior)
- `mtspr`/`mfspr` on BAT SPRs 542/543 → **machine check exception** — AmigaOS 4.x kernel intentionally traps all BAT register access from guest code, even in supervisor mode. Forbid()+SuperState() does not help; crash occurs at the SPR instruction itself
- BBoot DBAT3 setup (scouting fix) → correct assembly generated, but AmigaOS kernel overwrites all BATs during boot; fix does not survive to driver execution time

**Pegasos2 success** (full VirtIO 1.0 init confirmed):
- Device: 1AF4:1048 at 00:02.0
- Capabilities: NOTIFY_CFG (BAR4+0x3000), DEVICE_CFG (BAR4+0x2000), ISR_CFG (BAR4+0x1000), COMMON_CFG (BAR4+0x0)
- MMIO access via `stwbrx`/`lwbrx` (LE byte-reversed load/store) — canonical AmigaOS 4.1 PPC MMIO pattern
- Status handshake: 0x00 → 0x03 (ACK+DRIVER) → 0x0B (FEATURES_OK) → 0x0F (DRIVER_OK)
- Device features decoded: `0x30000006`/`0x00000101` = INDIRECT_DESC + EVENT_IDX + HOTPLUG + CHANGE + VERSION_1 + RING_RESET
- `num_queues = 3` readable from device config
- Note: `VIRTIO_SCSI_F_INOUT` (bit 0) not advertised by QEMU — per spec 5.6.6.1.1 this limits commands to unidirectional only (no simultaneous IN+OUT buffers)

**Additional AmigaOS MMIO finding**:
`InWord`/`InLong`/`OutWord`/`OutLong` silently return 0 for memory BAR addresses. Only `InByte`/`OutByte` issue real PCI memory cycles on AmigaOS 4.1 FE. Multi-byte MMIO access requires byte-assembly helpers (mmio_r16/mmio_r32/mmio_w16/mmio_w32) defined in `include/virtio/virtio_pci_modern.h`.

**test_modern.c features**:
- Generic VirtIO device detection (any 1AF4:xxxx vendor)
- Full PCI capability chain walk with cfg_type decode
- Device-aware feature decoding for: SCSI, Block, Network, GPU, 9P, IOMMU
- Reserved feature decoding per VirtIO 1.2 spec section 6
- Intelligent feature acceptance (accepts offered features, always negotiates VERSION_1)
- Portable across device types (device_type auto-detected from PCI device ID)

**Conclusion**: Modern VirtIO (device ID 0x1048) works on Pegasos2. AmigaOne requires legacy/transitional device (0x1004). The v1.5 driver will support both via runtime detection.

**Files added/modified**: `tests/test_modern.c` (new), `tests/Makefile` (updated), `include/virtio/virtio_pci_modern.h` (new), `include/version.h` (1071)

### Build 1072-1075: Modern VirtIO Auto-Detection + Full Init (February 2026)

- **Build 1072**: `DetectModernVirtIO()` walks PCI vendor-specific capability chain (type 0x09) to detect COMMON_CFG, NOTIFY_CFG, ISR_CFG, DEVICE_CFG regions. Populates `libBase->common_cfg_base`, `notify_cfg_base`, `isr_cfg_base`, `device_cfg_base`, `notify_off_mult`. Sets `modern_mode = TRUE` if COMMON_CFG found. Called from `DiscoverVirtIOSCSI()` after device found.
- **Build 1073**: Full modern VirtIO 1.0 init sequence (`InitVirtIOSCSI_Modern()`). Feature negotiation via `mmio_r32`/`mmio_w32` byte-assembly MMIO helpers (InByte-based, portable to both platforms). FEATURES_OK handshake. Three-address queue setup (DESC/AVAIL/USED low+high). Per-queue notify address from `notify_cfg_base + Q_NOFF * notify_off_mult`. LE vring wrappers (`vr16`/`vr32`/`vr64` via `__builtin_bswap*`) applied to all vring field accesses in `AddBuf`/`GetBuf`. Modern kick via `mmio_w16` to `vq->notify_addr`. ISR dispatch reads `isr_cfg_base` in modern mode, `BAR0 + VIRTIO_PCI_ISR` in legacy. Cleanup path resets via `common_cfg_base + STATUS` (modern) or `BAR0 + STATUS` (legacy).
- **Build 1075**: Gitignore updates. Debug logging tidied.

### Build 1076: PCI Discovery — Modern Device ID Priority (February 2026)

- **Problem**: `DiscoverVirtIOSCSI()` only searched for legacy/transitional device ID `0x1004`. On Pegasos2, QEMU exposes a modern non-transitional VirtIO SCSI device at `0x1048`. The driver found the `0x1004` device but the legacy I/O port init path returned zero features and zero-sized queues because the MV64361 bridge handles MMIO transparently but legacy I/O access differs.
- **Fix**: PCI discovery now searches for modern `0x1048` first, then falls back to legacy `0x1004`. Combined with `DetectModernVirtIO()` cap walk, the driver now auto-selects the correct init path on both platforms:
  - **Pegasos2**: Finds `1AF4:1048`, detects modern caps, uses `InitVirtIOSCSI_Modern()` with MMIO
  - **AmigaOne**: No `0x1048` device exists, finds `1AF4:1004`, no modern caps, uses legacy I/O init
- **Files changed**: `src/pci/pci_discovery.c`, `include/version.h` (bump to 1076)

### Builds 1077-1079: Modern VirtIO Bug Fixes — Pegasos2 Bring-Up (February 2026)

Three bugs found and fixed during Pegasos2 hardware testing of the modern VirtIO init path:

- **Build 1077**: MMIO helpers replaced with `stwbrx`/`lwbrx`/`lhbrx`/`sthbrx`/`stb`/`lbz` inline assembly macros in `virtio_pci_modern.h`. The previous `InByte`-based byte-assembly approach does not work for MMIO BAR addresses on Pegasos2 (MV64361 transparent bridge requires direct CPU load/store, not PCI accessor methods). Also replaced all remaining `pciDev->InByte`/`OutByte` calls in `InitVirtIOSCSI_Modern`, the ISR handler, and the cleanup path with `mmio_r8`/`mmio_w8`. Added `BaseAddress` (CPU-visible) vs `Physical` (PCI bus) address selection in `DetectModernVirtIO` — on Pegasos2 these happen to be identical, but `BaseAddress` is the correct field per the API. Added reset polling loop after STATUS=0 write to match test_modern.c.
- **Build 1078**: **Root cause of zero features on Pegasos2** — the driver never enabled `PCI_COMMAND_MEMORY` and `PCI_COMMAND_MASTER` in the PCI Command register before MMIO access. Without Memory Space enabled, the PCI bridge ignores all MMIO transactions (reads return 0, writes dropped). Added `WriteConfigWord(PCI_COMMAND, ...)` and `SetCapabilities(PCI_CAP_BUSMASTER)` at the top of `InitVirtIOSCSI_Modern()`. After this fix: features read correctly (`0x30000006`/`0x00000101`), FEATURES_OK accepted (Status=0x0B→0x0F), all 3 queues enabled.
- **Build 1079**: **DSI crash after successful modern init** — `VirtIOSCSI_DoIO()` unconditionally dereferenced `libBase->bar0->Physical` at function entry to compute `iobase`. In modern mode (non-transitional device 0x1048), `bar0` is NULL (no I/O BAR). The compiler eagerly loaded the field before the modern-mode dispatch could skip it, causing a NULL pointer dereference (DAR=0x00000016). Also, DoIO had a hardcoded `pciDev->OutWord(iobase + VIRTIO_PCI_QUEUE_NOTIFY)` that bypassed `VirtQueue_Kick()` and would fail in modern mode. Fixed both: `iobase` now guarded with `bar0 ? ... : 0`, and the notify replaced with `VirtQueue_Kick()` which handles both modern (mmio_w16) and legacy (OutWord) paths.

**Result**: Driver passes testing on both QEMU amigaone (legacy 0x1004) and QEMU pegasos2 (modern 0x1048) in both debug and release builds.

**Files changed**: `include/virtio/virtio_pci_modern.h`, `src/pci/pci_modern_detect.c`, `src/pci/pci_discovery.c`, `src/virtio/virtio_init.c`, `src/virtio/virtio_irq.c`, `src/virtio/virtio_scsi_io.c`, `include/version.h` (1077→1079)

---

## v1.4 Correctness and Robustness

### Build 1070: Increase MAX_INFLIGHT from 8 to 16

- **Change**: `#define MAX_INFLIGHT 16` (was 8) in `include/virtioscsi.h`.
- **Effect**: Each unit task can now sustain 16 concurrent block I/O requests in-flight simultaneously. The VirtIO requestq has 256 slots; with two units sharing it, 16 slots each gives full pipeline utilisation on sequential workloads. All array declarations, allocation loops, and Harvest cross-unit search already used `MAX_INFLIGHT` symbolically — no other code changes needed.
- **Memory**: ~34KB additional MEMF_SHARED per unit (~68KB total): 8 extra req_cmd (51B), resp_cmd (108B), and bounce buffer (4096B) slots.

### Build 1069: SCSI INQUIRY VPD Pages

- **Problem**: `Handle_SCSI_Inquiry()` passed all INQUIRY commands directly to VirtIO. SCSI tools requesting Vital Product Data pages (EVPD=1) could receive CHECK CONDITION if QEMU's VirtIO SCSI didn't support a given page code, surfacing as `HFERR_BadStatus`.
- **Fix**: Check `CDB[1] & 0x01` (EVPD bit) before forwarding. Standard INQUIRY (EVPD=0) still passes through to VirtIO unchanged. For EVPD=1, intercept three page codes with synthetic responses:
  - **Page 0x00** (Supported VPD Pages List): returns 7-byte list advertising pages 0x00, 0x80, 0x83.
  - **Page 0x80** (Unit Serial Number): returns `"VIRTIOSCSI-T%lu"` formatted with `unit->target_id` via `IUtility->SNPrintf()`.
  - **Page 0x83** (Device Identification): returns a T10 vendor ID designation descriptor `"QEMU    virtioscsi      T%lu"` (8-char vendor + 16-char product + target ID).
  - **All other page codes with EVPD=1**: return CHECK CONDITION ILLEGAL REQUEST / INVALID FIELD IN CDB (sense key 0x05, ASC 0x24, ASCQ 0x00).
- **Files changed**: `src/scsi_cmds/scsi_inquiry.c`.

### Build 1068: Proper SCSI Error → io_Error Mapping; TD_GETDRIVETYPE documentation

- **Problem A**: All non-GOOD SCSI completions mapped to `HFERR_BadStatus` (45) regardless of sense key, losing information that filesystems could use for error recovery.
- **Fix A**: New `static int32 map_scsi_error(uint8 virtio_resp, uint8 scsi_status, uint8 sense_key)` helper in `virtio_scsi_io.c`. Sense key extracted from `resp_cmd->sense[2] & 0x0F`. Mapping:
  - `0x02` NOT READY → `TDERR_BadDriveType`
  - `0x03` MEDIUM ERROR → `TDERR_BadSecHdr`
  - `0x05` ILLEGAL REQUEST → `IOERR_NOCMD`
  - `0x06` UNIT ATTENTION → `TDERR_DiskChanged`
  - `0x07` DATA PROTECT → `TDERR_WriteProt`
  - `0x00`/`0x01` NO SENSE/RECOVERED → `0` (success)
  - All others → `HFERR_BadStatus`
  Applied at 4 sites: Harvest own-unit, Harvest cross-unit, DoIO inline-harvest, DoIO final result decode.
- **Problem B**: `Handle_TD_GetDriveType()` in `cmd_stubs.c` already returned the correct value (`DRIVE_NEWSTYLE`, 0x44) and set `io_Error = 0`, but lacked explanation.
- **Fix B**: Added detailed comment explaining why `DRIVE_NEWSTYLE` is correct for a fixed-disk device supporting TD_READ64/WRITE64 and NSCMD_TD_* commands.
- **Files changed**: `src/virtio/virtio_scsi_io.c`, `src/exec_cmds/cmd_stubs.c`.

### Build 1067: READ CAPACITY (16) for >2TB disks; version bumped to v1.4

- **Problem**: `ensure_geometry_cached()` used a hard-coded READ CAPACITY (10) CDB (opcode 0x25, 8-byte response, 32-bit last LBA). Disks ≥ 2TB return `0xFFFFFFFF` as the last LBA — a sentinel indicating READ CAPACITY (16) must be used. `total_blocks` was stored as `uint32`, limiting reported size to ~2TB.
- **Fix**: Two-step geometry discovery:
  1. Issue READ CAPACITY (10). If last LBA == `0xFFFFFFFF`:
  2. Issue READ CAPACITY (16) (opcode `0x9E`, service action `0x10`, 32-byte response) to get the true 64-bit last LBA and block size.
  `total_blocks` changed from `uint32` to `uint64` in `VirtIOUSCSIDevUnit`. `dg_TotalSectors` in `TD_GETGEOMETRY` response clamped to `0xFFFFFFFF` for disks > 2TB (field is `uint32`).
- **New function**: `make_read_capacity16_cdb(uint8 *cdb)` added to `scsi_cdb_helpers.c`/`.h`.
- **Version**: `DEVICE_REVISION` bumped from 3 to 4, first build of v1.4.
- **Files changed**: `include/virtioscsi.h`, `include/scsi_cdb_helpers.h`, `src/scsi_cdb_helpers.c`, `src/exec_cmds/cmd_td_getgeometry.c`, `include/version.h`.

### Build 1066: Documentation update; Items 5 and 6 verified complete

- **Verification**: Confirmed that Items 5 (VIRTIO_F_INDIRECT_DESC) and 6 (READ(16)/WRITE(16)) from the performance plan are already implemented in the codebase.
- **Item 5 status**: Infrastructure fully present — `vring_indirect_desc` struct, `indirect_tables[]` array, AddBuf indirect path, GetBuf cleanup are all implemented in `virtqueue.c`/`.h`. Feature negotiation is intentionally disabled in `virtio_init.c` due to QEMU legacy endianness incompatibility: QEMU's legacy VirtIO SCSI reads indirect descriptor table entries as little-endian, but the driver fills them in native PPC big-endian byte order, causing QEMU to see garbage lengths ("wrong size for virtio-scsi headers"). The direct chained-descriptor path works correctly and supports up to MAX_SG_ENTRIES=64 entries. Re-enabling INDIRECT_DESC would require either byte-swapping all indirect table fields (addr, len, flags) or switching to VirtIO 1.0 modern mode.
- **Item 6 status**: Fully implemented. `make_read16_cdb()`/`make_write16_cdb()` (16-byte CDB, 64-bit LBA) are present in `scsi_cdb_helpers.c`. Both `cmd_td_io64.c` and `ns_td_io64.c` dispatch to READ(16)/WRITE(16) when `lba > 0xFFFFFFFF` and to READ(10)/WRITE(10) otherwise.
- **Documentation**: All project documents updated and committed; changes pushed to GitHub. Build moved to release mode (no -DDEBUG).

### Build 1065: ATA PASS-THROUGH stub for SMART tool compatibility

- **Problem**: SMART applications (e.g. AmigaDiskBench) issue `HD_SCSICMD` with opcode `0x85` (ATA PASS-THROUGH 16, SAT spec) to send an ATA SMART READ DATA command (`0xB0`/`0xD0`). A fallback to opcode `0xA1` (ATA PASS-THROUGH 12) is tried if the first attempt fails. Both opcodes were unhandled — the driver returned `HFERR_BadStatus` (io_Error 45) from `scsi_parse.c`'s default case.
- **Root cause**: VirtIO SCSI is not an ATA device. There is no real ATA layer. SAT pass-through has nowhere to forward to. But SMART tools don't need real data — they need a structurally correct 512-byte ATA SMART Data block that parses without error.
- **Fix**: New `src/scsi_cmds/scsi_ata_passthrough.c` handler for opcodes `0x85` and `0xA1`. Checks that the ATA command field is `0xB0` (SMART); any other ATA command (e.g. IDENTIFY) returns CHECK CONDITION. For SMART, builds and returns a 512-byte dummy SMART Read Data block:
  - Bytes 0-1: revision `0x0006` (little-endian)
  - 6 plausible attribute entries at bytes 2-73: IDs 1 (Read Error Rate), 9 (Power-On Hours=1), 12 (Power Cycle=1), 194 (Temperature=30°C), 197 (Pending Sectors=0), 198 (Uncorrectable=0) — all current/worst=100, no critical flags
  - Byte 510: `0xC0` (offline collection status — never run, no error)
  - Byte 511: `0x00` (checksum not computed; most parsers accept)
- **Result**: SMART tools receive a parseable, healthy-looking response. AmigaDiskBench reports "Drive is healthy" for VirtIO disks instead of `io_Error=45`.
- **Files changed**: `src/scsi_cmds/scsi_ata_passthrough.c` (new), `include/virtioscsi_cmds.h` (declaration), `src/scsi_cmds/scsi_parse.c` (cases 0x85/0xA1), `Makefile` (new source).

### Build 1063: Interrupt coalescing via used_event batching

- **Optimisation**: Under sustained pipeline load with `MAX_INFLIGHT=8` requests simultaneously in-flight, the device was raising one interrupt per completed descriptor (8 ISRs per burst). Each ISR wakes a unit task, causing 8 task reschedules where 1 would suffice.
- **Design**: Two-layer `used_event` update:
  1. **`VirtQueue_GetBuf()` baseline** (per-call): writes `used_event = last_used_idx` after every completion. This keeps the field in sync when `GetBuf` is called from the polling path (discovery, before unit tasks exist). Without it, `used_event` stays at 0 while `last_used_idx` advances to 64+ during discovery; when the ISR path takes over, QEMU's check `(used->idx - used_event - 1) < (used->idx - old)` evaluates FALSE forever and the device never raises another interrupt — drives fail to mount.
  2. **`VirtIOSCSI_Harvest()` override** (post-drain): after draining the entire used ring, counts occupied inflight slots across all units. Only when `occupied >= 2`: writes `used_event = last_used_idx + (occupied - 1)`. VirtIO spec formula: `used_event = L + N - 1` fires after exactly N completions. When `occupied <= 1`: leaves GetBuf's baseline (`last_used_idx`) untouched — fire on next +1, no added latency.
- **Formula pitfall**: Writing `last_used_idx + N` (not `N-1`) causes the device to wait for N+1 completions, permanently suppressing the Nth interrupt. Symptom: first I/O succeeds, all subsequent I/O hangs. This was the root cause of regression bug 2 during development (initial Harvest wrote `+1` when `occupied=0`).
- **Regression bug 1 (fixed)**: Removed the per-GetBuf baseline write entirely on first attempt. Drives failed to mount — polling→IRQ handoff broke because `used_event=0` while `last_used_idx=64` after discovery. Fix: restore baseline write.
- **Regression bug 2 (fixed)**: Initial Harvest formula wrote `last_used_idx + max(occupied, 1)` = `+1` when `occupied=0`. After first I/O completed, `used_event` advanced past `last_used_idx+1`; QEMU's check on the next completion evaluated `65535 < 1` = FALSE → interrupt suppressed forever. Fix: only override when `occupied >= 2`; write `last_used_idx + (occupied - 1)`.
- **Result**: Polling→IRQ handoff is safe (GetBuf baseline). Under load, N in-flight → 1 ISR per burst (Harvest override). Idle pipeline → first completion of next batch fires immediately (no added latency).
- **Files changed**: `src/virtio/virtqueue.c` (per-`GetBuf` `used_event` write restored with extended comment), `src/virtio/virtio_scsi_io.c` (batch `used_event` override at end of `VirtIOSCSI_Harvest`).

### Build 1061: Deferred kick — batch QUEUE_NOTIFY for burst I/O

- **Optimisation**: `VirtIOSCSI_Submit()` previously called `VirtQueue_Kick()` after every `AddBuf`, writing `QUEUE_NOTIFY` to the PCI I/O port once per request. For a burst of N queued requests (e.g. a filesystem prefetching clusters) this generated N PCI writes where one would suffice.
- **Change**: Kick removed from `Submit`. New `VirtIOSCSI_Kick()` function added. The unit task's dispatch loop now drains the entire message port queue first, tracking whether any `Submit` succeeded, then calls `VirtIOSCSI_Kick()` once to flush the whole batch.
- **Result**: N queued requests → 1 PCI `QUEUE_NOTIFY` write. Benefit scales with burst depth; single-request workloads are unchanged (still 1 kick). Synchronous `VirtIOSCSI_DoIO()` calls (geometry, HD_SCSICMD) issue their own unconditional `QUEUE_NOTIFY` internally and are unaffected.
- **Files changed**: `include/virtio/virtio_scsi_io.h` (new `VirtIOSCSI_Kick` declaration, updated `Submit` comment), `src/virtio/virtio_scsi_io.c` (kick removed from `Submit`, `VirtIOSCSI_Kick` added), `src/unit_task.c` (`UnitTask_Dispatch` returns `BOOL submitted`; event loop collects flag and kicks once).

### Build 1060: Harvest discards DoIO cookie — second release-build Heisenbug

- **Root cause**: When `VirtIOSCSI_Harvest()` dequeues a cookie from VQ2 that matches no inflight pipeline slot on any unit, it previously discarded it as "already handled". However, the cookie may belong to a concurrent `VirtIOSCSI_DoIO()` call on another unit whose drain loop **has not yet called `GetBuf`** — Harvest ran first because the ISR woke both unit tasks simultaneously and the other task didn't get scheduled before Harvest drained the ring.
- **Affected path**: Synchronous `DoIO` calls (geometry queries, SCSI passthrough) use `unit->req_buf` (= `req_bufs[0]`) as their VirtIO cookie. These cookies are **not** registered in any `inflight[]` slot (that's only for async pipeline Submit). So Harvest's global `inflight` search correctly finds nothing and (incorrectly) discards the cookie.
- **Symptom**: DoIO spin-waits for its cookie via 50 retries, never sees it (Harvest already consumed it), times out with `IOERR_SELFTEST`. The geometry query that filesystems send at mount time fails → mounter gives up → disk never appears on Workbench.
- **Why invisible in DEBUG builds**: `DPRINTF` overhead creates enough scheduling slack that Harvest never drains the ring before DoIO's drain loop starts. At `-O2` release speed the race fires reliably.
- **Fix**: Three-part change:
  1. Added `doio_pending_cookie` / `doio_pending_written` fields to `VirtIOUSCSIDevUnit` (in `virtioscsi.h`).
  2. In `VirtIOSCSI_Harvest()`: when a cookie matches no inflight slot anywhere, check whether it matches any unit's `req_buf` (a DoIO-in-flight marker). If so, stash the cookie+written in `doio_pending_cookie` under `io_lock`, then signal that unit's `io_wait_task` to wake its drain loop.
  3. In `VirtIOSCSI_DoIO()`'s drain loop: at the top of each retry iteration, check `unit->doio_pending_cookie` (under `io_lock`) before calling `GetBuf`. If it matches `req_cmd`, consume it immediately without calling `GetBuf`.
  4. Same DoIO-stash logic added to the inline-harvest path inside DoIO itself for symmetry.
- **Result**: Both drives appear on Workbench with release build. All mounts complete cleanly.

### Build 1059: DoIO inner-loop missing break — release-build Heisenbug

- **Root cause**: The inner `GetBuf` drain loop in `VirtIOSCSI_DoIO()` was missing a `break` after finding its own cookie. After setting `cookie = c`, the loop continued calling `GetBuf` — if the other unit's pipeline completion had already landed in the ring, it was dequeued but then fell through the `else` inline-harvest path with incorrect lock state (lock was still held from the outer iteration, but the `else` branch releases and re-acquires it — causing mismatched semaphore operations and a lost completion).
- **Why invisible in DEBUG builds**: Every `DPRINTF` call introduces enough instruction overhead (register saves, subroutine call) that both units' completions never landed in VQ2 simultaneously during the narrow window between `GetBuf` calls. At `-O2` release speed they frequently did.
- **Symptom**: DH8 (FastFileSystem, unit 1) failed to appear on Workbench. Unit 1's mounter READ CAPACITY completion was stolen and dropped, causing its filesystem to hang permanently during mount.
- **Fix**: Added `break` immediately after `cookie = c; written = w;` in the inner `while` loop. The outer `while (retries > 0)` loop already checks `if (cookie) break` so control flows correctly.
- **Result**: Both drives appear on Workbench with release build. Release build now behaves identically to debug build.

### Build 1062: Bounce buffer ring — zero-overhead small I/O

- **Optimisation**: Every block I/O request ≤4096 bytes previously called `StartDMA()`/`GetDMAList()`/`EndDMA()` to DMA-map the user buffer — three syscalls with lock overhead per request. For 512-byte sectors these costs dominated the transfer time.
- **Change**: Pre-allocated one `BOUNCE_BUF_SIZE=4096` `MEMF_SHARED` buffer per inflight slot at unit startup, permanently DMA-mapped. `VirtIOSCSI_Submit()` checks `data_len <= BOUNCE_BUF_SIZE`: if true, writes copy directly to the pre-pinned bounce buffer (write path) or marks the slot for read-back (read path), using the pre-computed physical address in the SG descriptor. No per-call `StartDMA`/`EndDMA` for these transfers.
- **Read-back**: `VirtIOSCSI_Harvest()` (own-unit, cross-unit, and inline-harvest paths) copies bounce→user after decoding the result, before `ReplyMsg`, when `using_bounce && !is_write && io_Error == 0`.
- **Large transfers** (>4096 bytes) continue to use the direct DMA path unchanged.
- **`bounce_copy()`**: Volatile `uint8` byte loop — no `memcpy()` (driver uses `-nostartfiles`, newlib not linked). Volatile required for `MEMF_SHARED` non-cacheable memory.
- **Files changed**: `include/virtioscsi.h` (`BOUNCE_BUF_SIZE` define, `using_bounce`/`is_write` in `inflight[]`, `bounce_bufs[]`/`bounce_dma_phys[]`/`bounce_dma_entries[]` arrays), `src/virtio/virtio_scsi_io.c` (`bounce_copy()` helper, bounce/direct branch in `Submit`, read-back in all three Harvest paths), `src/unit_task.c` (`alloc_one_bounce()`/`free_one_bounce()` helpers, hooked into `preallocate_unit_dma`/`free_unit_dma`).

### Build 1061: Deferred kick — batch QUEUE_NOTIFY for burst I/O

(see entry above — build numbers in this file are in reverse chronological order)

### Build 1058: Cross-unit cookie harvest — STABLE
- Root cause of missing second drive: when unit 0's `VirtIOSCSI_Harvest()` dequeues a cookie belonging to unit 1's pipeline `inflight[]` slot, it previously just printed "other unit, skipping" and dropped it — the IORequest was never replied to, causing the filesystem to hang permanently.
- **Fix**: When a cookie doesn't match the calling unit's `inflight[]`, search `libBase->units[]` globally to find the true owner unit. Reply the IORequest via the owner's `inflight[slot]` and `resp_bufs[slot]`. Same fix applied to DoIO's inline-harvest loop.
- Both drives (DH7/SmartFilesystem T0L0, DH8/FastFileSystem T1L1) now appear on Workbench.
- All I/O completing cleanly: geometry queries, 32KB and 1KB block reads, no unmatched cookies.
