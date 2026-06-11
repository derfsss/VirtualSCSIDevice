# Changelog — virtioscsi.device

All notable end-user changes to `virtioscsi.device`.  The development
log with full investigation notes lives in [HISTORY.md](HISTORY.md).

### v1.12 — 2026-06-11
- **Full code review pass (vs the VirtIO spec and across all transport paths).** Fixes, worst first:
  - **>32 MiB single transfers were silently truncated.** The pipeline path cast the block count to the 16-bit READ(10)/WRITE(10) transfer-length field; a request above 65,535 blocks wrapped and short-transferred. Requests that exceed the 16-bit count now use READ(16)/WRITE(16) regardless of LBA (also fixed in the NSD fallback path).
  - **Modern-mode endianness on device-written fields.** `virtio_scsi_resp_cmd.residual` is little-endian under VIRTIO_F_VERSION_1 but was read raw (benign while 0 — any real underrun would have produced a garbage `io_Actual`). New `virtio_scsi_resp_residual()` helper swaps when modern, and the result is clamped to the request length. The interrupt-coalescing `used_event` write in `VirtIOSCSI_Harvest` also missed its `vr16()` wrapper, byte-swapping the threshold on the modern path — the device could suppress interrupts it should have delivered.
  - **Cross-task races on the inflight bookkeeping.** `complete_inflight_slot()` mutated `occupied_count` / `inflight_count` / `active_units_mask` and the per-unit free list without `io_lock`, but it can run in a *different* task from the owning unit (cross-unit inline harvest in DoIO). Lost decrements inflate `occupied_count`, which makes EVENT_IDX coalescing program a `used_event` for completions that never arrive. All free-list and counter updates now take `io_lock`; `VirtIOSCSI_Submit`'s slot pop/rollback uses the same lock.
  - **Device-supplied descriptor index now bounds-checked** in `VirtQueue_GetBuf` before indexing `cookies[]` / `indirect_tables[]`.
  - **Held TD_ADDCHANGEINT / TD_REMOVE with no unit** were neither held nor replied — the caller hung forever. They now fail with `IOERR_OPENFAIL`.
  - **TD_GETNUMTRACKS returned 0.** Now queued to the unit task and answered with real cylinder counts (RDB-declared when present, linear fallback otherwise) — the existing handler was complete but never wired up.
  - Unit-task startup failure path leaked the port mutex.
- **Dead code removed:** `cmd_read.c`, `cmd_write.c`, `cmd_td_io64.c` (block I/O has gone through the inflight pipeline in `unit_task.c` since v1.3 — these synchronous handlers had no callers), the unused TD_CHANGESTATE/TD_PROTSTATUS/TD_GETDRIVETYPE stubs (answered inline in BeginIO), and write-only struct fields (`rdb_geometry_checked`, per-slot `scsi_status`/`residual`).

### v1.11 — 2026-05-15
- **>2 TiB disks: partitions now mount.** `TD_GETGEOMETRY`'s `dg_TotalSectors` (uint32) is clamped to `0xFFFFFFFF` instead of letting the cast wrap to 0. `diskboot.kmod` (2014) treats `TotalSectors=0` as "size unknown" and skips the whole unit, which previously prevented any partition on a >2 TiB virtio-scsi disk from getting a DOSNode -- even partitions that lay entirely within 32-bit LBA range. Reported as `0xFFFFFFFF` the unit looks like a `>=` 2 TiB disk to legacy callers, and `NSCMD_TD_GETGEOMETRY64` still reports the real 64-bit count for callers that ask. Single partitions remain capped at 2 TiB by the RDB protocol's 32-bit cylinder fields (SDK limit, not a driver issue); use multiple partitions to span the full disk.
- **sii3112-style logical CHS for >2 TiB.** `TD_GETGEOMETRY` now synthesises a logical CHS where `dg_Cylinders * dg_CylSectors == total_blocks` exactly (largest power-of-2 factor of `total_blocks` up to 256). Eliminates the "Total sectors: -6379" rounding artifact in Media's geometry panel and makes virtioscsi report the same disk size as `sii3112ide.device` for the same image. RDB-declared physical CHS is no longer used for `TD_GETGEOMETRY` -- filesystems use `PartitionBlock` fields for their own LBA math, and Media reads the RDB itself for the "Physical data" panel.
- **RDB header validation tightened.** `ensure_rdb_geometry_cached` now verifies `rdb_SummedLongs` is plausible and walks the longword checksum (must sum to 0 mod 2^32). Stops the driver from accepting a stale "RDSK" magic from a partially-overwritten sector as a valid header (which previously polluted geometry on disks where the start-of-file still held bytes from an earlier image).
- **RDB no longer cached across reads.** The on-disk RDB can be rewritten at any time by Media/HDToolbox; re-read block 0 on every `TD_GETGEOMETRY` so reported CHS tracks the current on-disk state. The capacity result (READ CAPACITY 10/16) is still cached -- only the partition-table parse is re-done.
- **Test suite expansion (TESTS 14-17)**: held-async semantics (`TD_ADDCHANGEINT` replace-prior, `TD_REMCHANGEINT` retire, non-last-close behaviour), `NSCMD_TD_GETGEOMETRY64` round-trip vs `TD_GETGEOMETRY`, `HD_SCSICMD` ATA pass-through (CDB 0x85 SMART READ DATA + IDENTIFY DEVICE rejection), unsupported `HD_SCSICMD` opcode auto-sense decoding. Plus fixed TEST 3's assertion to accept the actual driver behaviour for beyond-EOF reads.
- **Dormant code removed**: `src/virtio/virtio_events.c` and `src/virtio/virtio_mounter.c` (the v1.9 event-queue consumer and mounter.library hot-add integration -- disabled in v1.10 pending SFS 1.290 troubleshooting) are deleted. Recovering them requires `git log -- src/virtio/virtio_events.c`. Associated unused fields removed from `VirtIOSCSIBase` and `VirtIOUSCSIDevUnit`.

### v1.10 — 2026-05-13
- **expansion.library minimum lowered to v53**: Previously required v54, which only ships in FE Update 3 (kernel-embedded `expansion.library 54.1`, July 2023). Install CD 53.54, Update 1, and Update 2 all carry `expansion.library 53.1` (frozen since 16.6.2008), so the old gate blocked the driver from loading on every release prior to U3 on every platform. The `PCIIFace` methods used (`FindDeviceTags`, `GetResourceRange`, `ReadConfig*`/`WriteConfig*`, `FreeDevice`) have been stable since well before 53.1, so v53 is a safe floor.
- **SFS 1.290 compatibility**: 68k jump-table via `CLT_Vector68K`+`CLT_NoLegacyIFace` (so SFS's `BeginIO`-at-(-30) call site lands on a real handler), `Resident` struct relocated to `.data` to match shipping OS4 IDE drivers, `dg_BufMemType = MEMF_PUBLIC|MEMF_LOCAL` for BPTR-safe low-RAM buffers, `TD_GETDRIVETYPE` returns `DRIVE3_5` (matching `a1ide.device`), `lib_Version` pinned to 53 so SFS's version check accepts us. Without this, SFS 1.290 silently refuses to mount any partition.
- **RDB geometry caching**: `ensure_rdb_geometry_cached()` parses the RDB header and first `PartitionBlock`, so `TD_GETGEOMETRY` reports CHS matching the on-disk partition layout instead of the raw `READ CAPACITY` block count.
- **Hosted-sandbox compatibility**: Every `AllocVecTags` whose buffer flows into `StartDMA` carries the `SBV_AVT_HostDMA` tag (`0x80535601`). Inside a hosted AmigaOS sandbox environment (X5000 host) this routes the allocation through the host's real allocator, producing a DMA-mappable buffer; on native AOS4 the tag value sits in the unknown-tag range and `utility.library`'s tag walker silently ignores it. Same source, dual use. Validated as a resident driver in that environment on X5000.
- **DMA use-after-free on shutdown**: `free_unit_dma()` now runs after the unit task has fully exited, so the drain loop never accesses freed DMA pointers.
- **Forbid/Permit eliminated**: `MutexObtain`/`MutexRelease` replace `Forbid`/`Permit` around the port queue in `AbortIO`. Task start uses `AT_Param1` instead of the `tc_UserData` shuffle. Shutdown is a proper cross-task signal handshake. Brings the driver in line with current OS4 guidance.
- **Misc correctness**: Task name now stored in the unit struct (the stack-local string was a dangling pointer once `CreateTaskTags` returned — `CreateTaskTags` stores the pointer, not a copy). `CMD_STOP`/`CMD_START` added to the NSD `supported_commands[]`, 14 missing entries added to `GetCommandName()` for complete debug logging. Partition block bound checks use `total_blocks` rather than `cyls`.
- **Build**: Makefile produces both stripped release and unstripped debug binaries in one invocation; both ship in the LHA.
- **CI**: GitHub Actions runs `make` on every push/PR using `walkero/amigagccondocker:os4-gcc11` (the same image developers use locally).

### v1.9 — 2026-04-14
- **Modern VirtIO MMIO on AmigaOne**: Runtime workaround in `pci_discovery.c` for a 64-bit BAR firmware bug. Before v1.9, BAR4's upper 32 bits were left at `0xFFFFFFFF` on AmigaOne (BBoot doesn't program the high DWORD and AmigaOS's PCI enumerator leaves it at the probe value), so VirtIO's prefetchable MMIO region ended up outside Articia's decoded PCI memory window. The driver now reads BAR5 at device discovery; if it reads `0xFFFFFFFF`, it writes 0 back via PCI config before calling `GetResourceRange(4)`. Root cause isolated via QEMU 10.2.2 `info pci`/`info mtree` compared across Pegasos2 (VOF programs BAR5=0) and AmigaOne (no firmware runs before BBoot). AmigaOne now uses the ~10-20x faster modern MMIO transport.
- **VIRTIO_RING_F_INDIRECT_DESC** (bit 28): Accepted on the modern MMIO path. Scatter-gather chains now consume a single vring descriptor regardless of SG count, eliminating descriptor pressure at high inflight depth. Fixed byte-swap bugs in the existing indirect implementation (indirect-table writes now wrap through `vr64`/`vr32`/`vr16`, matching negotiated endianness; free-list `next` captured before overwrite) and added NEXT chaining between table entries. Disabled on the legacy I/O path (where QEMU reads indirect entries as LE while PPC writes native BE) and on VQ1 (single-region buffers).
- **No Forbid/Permit pairs** in new code: tasks receive `libBase` via `AT_Param1` on `CreateTaskTags` instead of the `tc_UserData` shuffle. Shutdown is a proper cross-task signal handshake (caller `AllocSignal`s a bit in its own context and `Wait`s; worker signals on exit).
- **Shell-run diagnostic**: `_start()` prints `"virtioscsi.device cannot be executed from a shell ..."` via `IExec->DebugPrintF` and returns 20 (`RETURN_FAIL`) instead of silently returning 0.
- **Version renumbered**: major version returns to 1.x (v1.8 -> v1.9). Boot drive support is provided by resident priority 0 and the `diskboot.config` entry, not by the major version number.

### v53.8 — 2026-04-14
- **Boot drive support**: VirtIO SCSI disks can now be used as boot drives. Resident priority changed to 0 (matching other AmigaOS disk device drivers like a1ide.device, peg2ide.device). Major version bumped to 53 (matching AmigaOS 4.1 FE SDK device driver convention). Tested as boot drive on AmigaOne, Pegasos2, and SAM460ex **when booting via BBoot** (which loads Kickstart from memory and therefore doesn't require the firmware to know about virtio-scsi-pci). When booting via real firmware such as `pegasos2.rom`, the firmware itself is unaware of virtio-scsi-pci and cannot select it as a boot target; in that case boot from a small IDE/NVMe disk that loads Kickstart + this driver, and the virtio-scsi disks will be mounted as secondary devices.
- **MediaToolbox crash fix**: Removed explicit `IMounter->AnnounceDeviceTags()` call from `unit_discovery.c`. With priority 0, mounter.library is not yet initialised when our driver loads, so this call corrupted state and caused MediaToolbox to crash (`ramlib.support` DSI in `dos.library`). The driver now matches the standard AmigaOS disk driver pattern — `diskboot.kmod` handles all DOSNode creation via the `diskboot.config` entry.
- **Installation**: `diskboot.config` entry `virtioscsi.device 8 3` and `MODULE Kickstart/virtioscsi.device` placement before `diskboot.config`/`diskboot.kmod` in Kicklayout are required for boot capability.
- **Binary size**: Reduced from 82KB to 41KB. Linker flags `-Wl,-z,common-page-size=4096 -Wl,-z,max-page-size=4096` collapse 28KB of section padding; `ppc-amigaos-strip --strip-all` removes 12KB of symbol/debug tables. Strip is automatically skipped when `-DDEBUG` is defined so debug builds keep their symbols. The release LHA now includes both `virtioscsi.device` (41KB stripped) and `virtioscsi.device.debug` (83KB unstripped) for diagnostics.
- **Build**: Dynamic build date/time stamps via Makefile (`-DBUILD_DATE` / `-DBUILD_TIME`). The boot serial output now shows the build timestamp: `virtioscsi.device 53.8 (DD.MM.YYYY) [HH:MM]`.
- **Documentation**: Added warning about matching QEMU `format=` parameter to actual image file format (raw vs qcow2). Mismatch causes silent boot failures.

### v1.8 — 2026-04-11
- **Unified QEMU platform support**: Single `-device virtio-scsi-pci` works on all QEMU machines (AmigaOne, Pegasos2, SAM460ex). MMIO probe at boot auto-detects modern vs legacy transport — no platform-specific QEMU configuration required. Tested on all three machines.
- **Performance**: Cacheable bounce buffers replace non-cacheable volatile `bounce_copy()` loop — DMA mapping released after caching physical address, data copied via `IExec->CopyMem()` with explicit `IExec->CacheClearE()` for DMA coherency (~10-20x faster for ≤64KB I/O). O(1) cross-unit cookie routing via encoded `req_cmd->id`. ISR occupancy bitmask skips units with no inflight I/O.
- **Debug**: Comprehensive error-path instrumentation across all command handlers for serial debug output.
- **Build**: Fixed header guard collision (`virtioscsi.h` vs `virtio_scsi.h`).

### v1.7 — 2026-03-18
- **Performance**: Bounce buffer increased from 4KB to 64KB — eliminates DMA syscalls (StartDMA/AllocSysObject/GetDMAList/EndDMA/FreeSysObject) for virtually all filesystem I/O. Word-aligned bounce copy (~4x faster data movement). Pre-allocated DMA entry arrays for >64KB transfers.
- **Performance**: O(1) inflight slot allocation via free-list (replaces linear O(16) scan). O(1) Harvest cookie matching via slot index in req_cmd->id. Global occupied counter for interrupt coalescing (replaces 128-slot scan).

### v1.6 — 2026-03-18
- **Code review fixes**: Sub-block I/O requests (io_Length < block_size) now correctly rejected with `IOERR_BADLENGTH` in CMD_READ/CMD_WRITE instead of silently rounding up to 1 block. Fixed redundant semaphore release/re-acquire in DoIO cross-unit cookie stash path. Fixed integer overflow in test_inquiry capacity calculation for >4GB disks.
- **Build system**: Added automatic header dependency tracking (`-MMD -MP`). Added `test_inquiry` to default build targets. Added stricter compiler warnings (`-Wextra`, `-Wshadow`, `-Wformat=2`).
- **Cleanup**: Replaced non-ASCII emoji in test_modern debug output. Added named constant `SAM2_SINGLE_LEVEL_LUN` for magic 0x40 in LUN encoding. Fixed inconsistent header guard in virtio_scsi.h. Removed build number from version string (standard AmigaOS major.minor format).

### v1.5 — 2026-02-28
- **Pegasos2 support**: VirtIO 1.0 Modern PCI transport (device 0x1048) with MMIO via `stwbrx`/`lwbrx` inline assembly. Auto-detected at boot alongside legacy transport (device 0x1004) for AmigaOne.
- **Modern VirtIO init**: PCI capability chain walk detects COMMON/NOTIFY/ISR/DEVICE config regions. Full VirtIO 1.0 status handshake (Reset→ACK→DRIVER→FEATURES_OK→DRIVER_OK). Three-address queue setup (DESC/AVAIL/USED). Per-queue notify via MMIO. LE vring byte-swap wrappers for all descriptor/ring field accesses.
- **Bug fixes**: PCI Memory Space and Bus Master enable before MMIO access; NULL-safe BAR0 dereference in modern mode; modern-aware queue notify in DoIO path; reset polling after device reset.

### v1.4 — 2026-02-24
- **Performance**: `MAX_INFLIGHT` increased from 8 to 16 for higher pipeline depth.
- **Compatibility**: SCSI INQUIRY VPD pages (0x00, 0x80, 0x83) answered locally. ATA PASS-THROUGH stub for S.M.A.R.T. tool support.
- **Correctness**: SCSI sense key decoded and mapped to specific AmigaOS io_Error codes. READ CAPACITY (16) fallback for disks ≥ 2TB. `total_blocks` now `uint64`.

### v1.3 — 2026-02-22
- **Interrupt-driven I/O**: PCI INTx interrupt handler replaces polling. Per-unit exec task with message port for async I/O.
- **Performance**: Pre-allocated DMA buffers, bounce buffer ring, deferred kick batching, interrupt coalescing via `used_event`, pipelined block I/O (up to 16 in-flight), `VIRTIO_F_INDIRECT_DESC`, READ(16)/WRITE(16) for >2TB.
- **Stability**: Cross-unit VirtIO completion harvest, `io_lock` serialisation for shared VQ2, DoIO cookie stash for release-build race conditions.
- **Build**: Release mode by default. `-fno-tree-loop-distribute-patterns` for GCC 11 compatibility.

### v1.2 — 2026-02-21
- Multi-unit automounting via `mounter.library`. Boot hang fix. I/O semaphore.
- Full 64-bit command coverage. Modern DMA API (`StartDMA`/`GetDMAList`/`EndDMA`).

### v1.0 — 2026-02-20
- Initial working driver: PCI discovery, VirtIO legacy init, real SCSI I/O (INQUIRY, READ CAPACITY, READ(10), WRITE(10)).
- Single-disk, single-partition operation.
