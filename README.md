# virtioscsi.device

An AmigaOS 4.1 Final Edition device driver for VirtIO SCSI disks in QEMU virtual machines.

> **This driver was developed with [Claude AI](https://claude.ai) (Anthropic) acting as the primary engineer — writing all C code, designing the architecture, debugging hardware-level issues, and navigating the AmigaOS 4.1 SDK. It stands as a practical demonstration of AI-assisted low-level systems programming on a niche, legacy platform with minimal training data.**
>
> **[Kyvos](https://ko-fi.com/s/6476fdadd2) was used to develop and test this device driver.**

---

## What is this?

`virtioscsi.device` exposes QEMU VirtIO SCSI virtual disks to AmigaOS 4.1 FE as standard trackdisk-compatible block devices. Once installed, AmigaOS treats them like any other hard disk: partitions are automatically discovered and mounted at boot, and filesystems (FFS2, SFS, etc.) work normally.

The driver auto-detects the best VirtIO transport for each QEMU machine type — no platform-specific QEMU configuration required. Tested on all three QEMU PowerPC machines:
- **Pegasos2** (MV64361 bridge) — modern VirtIO 1.0 MMIO via `stwbrx`/`lwbrx` inline assembly
- **AmigaOne** (Articia S bridge) — modern MMIO from v1.9 onward (runtime workaround for a firmware 64-bit BAR programming bug); legacy I/O on older driver builds
- **SAM460ex** — modern VirtIO 1.0 MMIO

---

## Features

- **Dual VirtIO transport** — Legacy PCI and Modern VirtIO 1.0, auto-detected at boot via MMIO probe. All three supported QEMU machines run the modern path; legacy is the automatic fallback if the MMIO probe fails. Same QEMU config works on every machine.
- **Interrupt-driven I/O** — uses PCI INTx interrupts; no CPU-burning polling loops
- **Async I/O** — per-unit exec task with message port; `BeginIO` returns immediately for slow commands
- **Multi-disk** — discovers up to 8 SCSI targets at boot, each announced to `mounter.library`
- **Automounting** — all discovered partitions mount automatically without manual configuration
- **Full trackdisk command set** — `CMD_READ`, `CMD_WRITE`, `CMD_UPDATE`, `TD_GETGEOMETRY`, `TD_FORMAT`, `TD_READ64`, `TD_WRITE64`, NSD 64-bit commands, `HD_SCSICMD`, and more
- **>2TB disk support** — two-step geometry discovery: READ CAPACITY (10) first; if last LBA == 0xFFFFFFFF, falls back to READ CAPACITY (16) for 64-bit block count
- **SCSI VPD pages** — INQUIRY EVPD requests (page 0x00/0x80/0x83) answered locally with serial number and device ID
- **Accurate SCSI error codes** — sense key decoded and mapped to specific AmigaOS io_Error codes (TDERR_WriteProt, TDERR_DiskChanged, TDERR_BadSecHdr, etc.)
- **4K sector support** — block size read from device via READ CAPACITY, not hardcoded
- **DMA scatter-gather** — uses AmigaOS 4.1 `StartDMA`/`GetDMAList`/`EndDMA` for correct VA→PA translation on the PPC MMU
- **Pre-allocated DMA buffers** — per-unit MEMF_SHARED request/response buffers with permanent DMA mappings eliminate per-I/O allocation overhead
- **Cacheable bounce buffers** — 64KB per inflight slot with cached physical addresses; transfers ≤64KB use `CopyMem` + `CacheClearE` for DMA coherency, eliminating per-call `StartDMA`/`EndDMA` overhead
- **Interrupt coalescing** — `used_event` batching reduces ISR frequency under pipeline load: N in-flight completions → 1 ISR per burst
- **No deprecated APIs** — uses only current AmigaOS 4.1 FE SDK functions (`StartDMA` not `CachePreDMA`, etc.)

---

## Requirements

- AmigaOS 4.1 Final Edition (PowerPC). The driver opens `expansion.library` v53 (frozen since 2008), so it loads on every FE release from the 53.54 install CD through Update 3.
- QEMU with a supported machine type (`amigaone`, `pegasos2`, or `sam460ex`).
- Also runs as a SandboxVM resident on AmigaOne X5000 (v1.10+).

---

## QEMU Setup

Add the following to your existing QEMU command line to attach VirtIO SCSI disks. The same device type (`virtio-scsi-pci`) works on all supported QEMU machines — the driver auto-detects the best transport at boot:

```
-device virtio-scsi-pci,id=scsi0 \
-drive file=image_file.img,if=none,id=vd0,format=raw \
-device scsi-hd,drive=vd0,bus=scsi0.0,channel=0,scsi-id=0,lun=0
```

Replace `image_file.img` with the path to your hard drive image file. You can attach additional drives by adding more `-drive`/`-device scsi-hd` pairs (up to 8 targets):

```
-drive file=second_disk.img,if=none,id=vd1,format=raw \
-device scsi-hd,drive=vd1,bus=scsi0.0,channel=0,scsi-id=1,lun=1
```

> **⚠️ Important:** The `format=` parameter must match your image file's actual format. Use `format=raw` for `.img` / `.raw` files and `format=qcow2` for `.qcow2` files. A mismatch (e.g. `format=raw` on a `.qcow2` file) causes silent boot failures — diskboot reads garbage from the disk and can't find a bootable partition.

> **Note:** Existing Pegasos2 setups using `-device virtio-scsi-pci-non-transitional` continue to work. The transitional device (`virtio-scsi-pci`) is recommended because it works on all machines without changes.

---

## Installation

VirtIO SCSI disks can be used as **boot drives** or as additional data drives. The driver must be added to the Kickstart module set and registered with `diskboot.config` so the system can find bootable partitions.

> **⚠️ Boot-device caveat:** Booting *directly* from a virtio-scsi disk requires the bootloader to know about the device. **BBoot supports this** (it enumerates PCI and loads Kickstart from a memory-resident zip). **Real Pegasos2 firmware (`pegasos2.rom`) does not** — it predates VirtIO and only knows about IDE. When running under real Pegasos2 firmware, keep AmigaOS installed on an IDE (or NVMe) boot drive and use virtio-scsi disks as secondary devices; the driver still loads as a Kickstart module and mounts them once the system is up. The same applies to real SAM460ex U-Boot if VirtIO extensions aren't present. AmigaOne under QEMU has no real firmware image — BBoot is the norm.

### Using BBoot (Kickstart zip archive)

[BBoot](https://codeberg.org/qmiga/bboot/) boots AmigaOS from a zip archive containing all Kickstart modules. To add `virtioscsi.device`:

1. Add `virtioscsi.device` to the `Kickstart/` folder inside your BBoot zip archive.

2. Edit the `Kicklayout` file inside the zip archive. Add the following line just **before** the `diskboot.config` and `diskboot.kmod` entries:

```
MODULE Kickstart/virtioscsi.device
```

For example, the relevant section should look like:

```
MODULE Kickstart/peg2ide.device.kmod
MODULE Kickstart/virtioscsi.device
MODULE Kickstart/diskboot.config
MODULE Kickstart/diskboot.kmod
```

3. Edit `Kickstart/diskboot.config` inside the zip archive and add the following line:

```
virtioscsi.device 8 3
```

4. Save the zip archive and boot with BBoot as normal.

### Without BBoot (SYS:Kickstart folder)

If you are not using BBoot and have AmigaOS installed on a bootable disk:

1. Copy `virtioscsi.device` to the `SYS:Kickstart/` folder on your AmigaOS system disk.

2. Edit `SYS:Kickstart/Kicklayout` and add the following line just **before** the `diskboot.config` and `diskboot.kmod` entries:

```
MODULE Kickstart/virtioscsi.device
```

3. Edit `SYS:Kickstart/diskboot.config` and add the following line:

```
virtioscsi.device 8 3
```

4. Save and reboot. The driver will be resident in memory from the very start of the boot process, and VirtIO SCSI disks can be used as boot drives.

---

## Compiling from Source

The project cross-compiles on Linux/WSL2 using the `walkero/amigagccondocker:os4-gcc11` Docker image.

### Prerequisites

- Docker (or WSL2 + Docker Desktop on Windows)
- The AmigaOS 4.1 SDK is included in the Docker image

### Build (WSL2 / Linux)

```bash
# From the project root:
docker run --rm -v $(pwd):/src -w /src walkero/amigagccondocker:os4-gcc11 make

# If $(pwd) expansion fails (non-interactive shell), use an absolute path:
docker run --rm -v /mnt/w/path/to/VirtualSCSIDevice:/src -w /src walkero/amigagccondocker:os4-gcc11 make
```

Output: `build/virtioscsi.device`

### Debug build

```bash
docker run --rm -v $(pwd):/src -w /src walkero/amigagccondocker:os4-gcc11 make CFLAGS="-O2 -Wall -I./include -fno-tree-loop-distribute-patterns -DDEBUG"
```

With `DEBUG` defined, the driver emits detailed serial/debug output via `IExec->DebugPrintF()` for every I/O operation, PCI discovery step, and VirtIO queue event.

### Distribution

```bash
# Create dist/VirtualSCSIDevice/ directory with driver, installer, docs, and tests:
docker run --rm -v $(pwd):/src -w /src walkero/amigagccondocker:os4-gcc11 make dist

# Create LHA archive for distribution (dist/VirtualSCSIDevice.lha):
docker run --rm -v $(pwd):/src -w /src walkero/amigagccondocker:os4-gcc11 make dist-lha
```

The distribution contains:
- `virtioscsi.device` — the compiled driver
- `Autoinstall` — AmigaDOS install script (copies driver to `SYS:Kickstart/`)
- `README_os4depot.txt` — documentation
- `Tests/` — test programs (`test_virtioscsi`, `test_modern`, `test_inquiry`)

### Clean

```bash
docker run --rm -v $(pwd):/src -w /src walkero/amigagccondocker:os4-gcc11 make clean
```

---

## Source Layout

```
src/
  device.c              — resident tag, library base init
  Init.c                — library open: PCI discovery, VirtIO init, unit discovery
  Open.c / Close.c      — per-opener reference counting, unit task lifecycle
  Expunge.c             — library cleanup
  BeginIO.c             — I/O request dispatcher
  cmd_names.c           — command name table (shared by BeginIO and NSCMD_DEVICEQUERY)
  scsi_cdb_helpers.c    — CDB builders, geometry cache helper
  unit_discovery.c      — SCSI INQUIRY scan, mounter.library announcement
  unit_task.c           — per-unit exec task, pre-allocated DMA buffers
  exec_cmds/            — CMD_READ, CMD_WRITE, TD_GETGEOMETRY, TD_IO64, etc.
  scsi_cmds/            — SCSI INQUIRY, READ CAPACITY, READ/WRITE(10), etc.
  ns_cmds/              — NSD NSCMD_DEVICEQUERY, NSCMD_TD_GETGEOMETRY64, etc.
  pci/                  — PCI bus enumeration, BAR mapping, modern cap detection
  virtio/               — VirtIO queue management, IRQ handler, SCSI I/O engine
include/
  virtioscsi.h          — library base and unit structs
  version.h             — version/revision defines
  virtio/               — VirtIO protocol headers, MMIO helpers
tests/
  test_virtioscsi.c     — stress test (concurrent I/O, geometry, 64-bit offsets)
  test_modern.c         — VirtIO 1.0 Modern device probe (Pegasos2 validation)
```

---

## Changelog

### v1.10 — 2026-05-13
- **expansion.library minimum lowered to v53**: Previously required v54, which only ships in FE Update 3 (kernel-embedded `expansion.library 54.1`, July 2023). Install CD 53.54, Update 1, and Update 2 all carry `expansion.library 53.1` (frozen since 16.6.2008), so the old gate blocked the driver from loading on every release prior to U3 on every platform. The `PCIIFace` methods used (`FindDeviceTags`, `GetResourceRange`, `ReadConfig*`/`WriteConfig*`, `FreeDevice`) have been stable since well before 53.1, so v53 is a safe floor.
- **SFS 1.290 compatibility**: 68k jump-table via `CLT_Vector68K`+`CLT_NoLegacyIFace` (so SFS's `BeginIO`-at-(-30) call site lands on a real handler), `Resident` struct relocated to `.data` to match shipping OS4 IDE drivers, `dg_BufMemType = MEMF_PUBLIC|MEMF_LOCAL` for BPTR-safe low-RAM buffers, `TD_GETDRIVETYPE` returns `DRIVE3_5` (matching `a1ide.device`), `lib_Version` pinned to 53 so SFS's version check accepts us. Without this, SFS 1.290 silently refuses to mount any partition.
- **RDB geometry caching**: `ensure_rdb_geometry_cached()` parses the RDB header and first `PartitionBlock`, so `TD_GETGEOMETRY` reports CHS matching the on-disk partition layout instead of the raw `READ CAPACITY` block count.
- **SandboxVM compatibility**: Every `AllocVecTags` whose buffer flows into `StartDMA` is tagged with `SBV_AVT_HostDMA` (`0x80535601`). On SandboxVM (X5000 host) this routes the allocation through the host's real allocator producing a DMA-mappable buffer; on native AOS4 the tag value sits in the unknown-tag range and `utility.library`'s tag walker silently ignores it. Same source, dual use. Validated as a SandboxVM resident driver on X5000.
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
