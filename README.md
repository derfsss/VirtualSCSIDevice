# virtioscsi.device

An AmigaOS 4.1 Final Edition device driver for VirtIO SCSI disks in QEMU virtual machines.

> **This driver was developed with [Claude AI](https://claude.ai) (Anthropic) acting as the primary engineer — writing all C code, designing the architecture, debugging hardware-level issues, and navigating the AmigaOS 4.1 SDK. It stands as a practical demonstration of AI-assisted low-level systems programming on a niche, legacy platform with minimal training data.**
>
> **[Kyvos](https://ko-fi.com/s/6476fdadd2) was used to develop and test this device driver.**

---

## What is this?

`virtioscsi.device` exposes QEMU VirtIO SCSI virtual disks to AmigaOS 4.1 FE as standard trackdisk-compatible block devices. Once installed, AmigaOS treats them like any other hard disk: partitions are automatically discovered and mounted at boot, and filesystems (FFS2, SFS, etc.) work normally.

The driver auto-detects the best VirtIO transport for each QEMU machine type — no platform-specific QEMU configuration required:
- **Pegasos2** (MV64361 bridge) — modern VirtIO 1.0 MMIO via `stwbrx`/`lwbrx` inline assembly
- **AmigaOne** (Articia S bridge) — legacy VirtIO I/O port access

---

## Features

- **Dual VirtIO transport** — Legacy PCI and Modern VirtIO 1.0, auto-detected at boot via MMIO probe (Pegasos2 gets modern, AmigaOne gets legacy, same QEMU config for both)
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
- **Bounce buffer ring** — pre-pinned 4096-byte MEMF_SHARED bounce buffers per inflight slot; transfers ≤4096 bytes skip per-call `StartDMA`/`EndDMA` entirely
- **Interrupt coalescing** — `used_event` batching reduces ISR frequency under pipeline load: N in-flight completions → 1 ISR per burst
- **No deprecated APIs** — uses only current AmigaOS 4.1 FE SDK functions (`StartDMA` not `CachePreDMA`, etc.)

---

## Requirements

- AmigaOS 4.1 Final Edition (PowerPC)
- QEMU with one of the supported machine types (`amigaone` or `pegasos2`)

---

## QEMU Setup

Add the following to your existing QEMU command line to attach VirtIO SCSI disks. The same device type (`virtio-scsi-pci`) works on all supported QEMU machines — the driver auto-detects the best transport at boot:

```
-device virtio-scsi-pci,id=scsi0 \
-drive file=virtioscsi1.img,if=none,id=vd0,format=raw \
-device scsi-hd,drive=vd0,bus=scsi0.0,channel=0,scsi-id=0,lun=0 \
-drive file=virtioscsi2.img,if=none,id=vd1,format=raw \
-device scsi-hd,drive=vd1,bus=scsi0.0,channel=0,scsi-id=1,lun=1
```

Replace `virtioscsi1.img` and `virtioscsi2.img` with your own hard drive image files. You can attach fewer or more drives by adjusting the `-drive`/`-device scsi-hd` pairs (up to 8 targets).

> **Note:** Existing Pegasos2 setups using `-device virtio-scsi-pci-non-transitional` continue to work. The transitional device (`virtio-scsi-pci`) is recommended because it works on all machines without changes.

---

## Installation

### Using BBoot (Kickstart zip archive)

[BBoot](https://codeberg.org/qmiga/bboot/) boots AmigaOS from a zip archive containing all Kickstart modules. To add `virtioscsi.device`:

1. Add `virtioscsi.device` to the `Kickstart/` folder inside your BBoot zip archive.
2. Edit the `Kicklayout` file inside the zip archive and add the following line after the existing boot device driver entry (e.g. after `MODULE Kickstart/a1ide.device.kmod` for AmigaOne, or after `MODULE Kickstart/peg2ide.device.kmod` for Pegasos2):

```
MODULE Kickstart/virtioscsi.device
```

3. Save the zip archive and boot with BBoot as normal.

### Without BBoot (SYS:Kickstart folder)

If you are not using BBoot and have AmigaOS installed on a bootable disk:

1. Copy `virtioscsi.device` to the `SYS:Kickstart/` folder on your AmigaOS system disk.
2. Edit the `SYS:Kickstart/Kicklayout` file and add the following line after the existing boot device driver entry (e.g. after `MODULE Kickstart/a1ide.device.kmod` for AmigaOne, or after `MODULE Kickstart/peg2ide.device.kmod` for Pegasos2):

```
MODULE Kickstart/virtioscsi.device
```

3. Save and reboot. The driver will be resident in memory from the very start of the boot process.

> **Note:** The driver has a resident priority of -60 so it initialises after `mounter.library`. Ensure `mounter.library` is also present in your Kickstart module set.

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
