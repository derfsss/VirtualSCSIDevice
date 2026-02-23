# virtioscsi.device

An AmigaOS 4.1 Final Edition device driver for VirtIO SCSI disks in QEMU/KVM virtual machines.

> **This driver was developed with [Claude AI](https://claude.ai) (Anthropic) acting as the primary engineer — writing all C code, designing the architecture, debugging hardware-level issues, and navigating the AmigaOS 4.1 SDK. It stands as a practical demonstration of AI-assisted low-level systems programming on a niche, legacy platform with minimal training data.**

---

## What is this?

`virtioscsi.device` exposes QEMU VirtIO SCSI virtual disks to AmigaOS 4.1 FE as standard trackdisk-compatible block devices. Once installed, AmigaOS treats them like any other hard disk: partitions are automatically discovered and mounted at boot, and filesystems (FFS2, SFS, etc.) work normally.

This driver is useful when running AmigaOS 4.1 FE inside QEMU/KVM using the Pegasos2 machine type. VirtIO SCSI disks are faster and more flexible than emulated IDE, and this driver gives AmigaOS full access to them.

---

## Features

- **VirtIO Legacy PCI** — works with QEMU's `-device virtio-scsi-pci` (Vendor 0x1AF4, Device 0x1004)
- **Interrupt-driven I/O** — uses PCI INTx interrupts; no CPU-burning polling loops
- **Async I/O** — per-unit exec task with message port; `BeginIO` returns immediately for slow commands
- **Multi-disk** — discovers up to 8 SCSI targets at boot, each announced to `mounter.library`
- **Automounting** — all discovered partitions mount automatically without manual configuration
- **Full trackdisk command set** — `CMD_READ`, `CMD_WRITE`, `CMD_UPDATE`, `TD_GETGEOMETRY`, `TD_FORMAT`, `TD_READ64`, `TD_WRITE64`, NSD 64-bit commands, `HD_SCSICMD`, and more
- **4K sector support** — block size read from device via `READ CAPACITY(10)`, not hardcoded
- **DMA scatter-gather** — uses AmigaOS 4.1 `StartDMA`/`GetDMAList`/`EndDMA` for correct VA→PA translation on the PPC MMU
- **Pre-allocated DMA buffers** — per-unit MEMF_SHARED request/response buffers with permanent DMA mappings eliminate per-I/O allocation overhead
- **No deprecated APIs** — uses only current AmigaOS 4.1 FE SDK functions (`StartDMA` not `CachePreDMA`, etc.)

---

## Requirements

- AmigaOS 4.1 Final Edition (PowerPC)
- QEMU/KVM configured with `-device virtio-scsi-pci` and one or more `-drive` attachments

### Example QEMU command line

```sh
qemu-system-ppc \
  -M pegasos2 \
  -bios pegasos2.rom \
  -device virtio-scsi-pci,id=scsi0 \
  -drive file=amigaos.img,if=none,id=hd0 \
  -device scsi-hd,drive=hd0,bus=scsi0.0
```

---

## Installation

### Option 1 — Copy to DEVS: (standard)

1. Copy `virtioscsi.device` to `DEVS:` on your AmigaOS system.
2. Reboot.
3. The driver loads automatically via `RTF_AUTOINIT` at startup and announces all discovered disks to `mounter.library`.

### Option 2 — Kickstart module (KickLayout)

Adding the driver as a Kickstart resident means it is available very early in the boot sequence, before any filesystem is mounted. This is useful if your boot disk is a VirtIO SCSI disk.

1. Copy `virtioscsi.device` to your KickLayout directory (e.g. `BOOT:Kickstart/` or wherever your Kickstart module set is stored).
2. Add an entry to your `KickLayout` file:

```
KICKMODULE DEVS:virtioscsi.device
```

   Or, if using the Kickstart tool directly, add the file to the module list in the Kickstart preferences.

3. Save and reboot. The driver will be resident in memory from the very start of the boot process.

> **Note:** The driver has a resident priority of -60 so it initialises after `mounter.library`. If used as a Kickstart module, ensure `mounter.library` is also present in the Kickstart set.

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
  pci/                  — PCI bus enumeration, BAR mapping
  virtio/               — VirtIO queue management, IRQ handler, SCSI I/O engine
include/
  virtioscsi.h          — library base and unit structs
  version.h             — version/revision/build defines
  virtio/               — VirtIO protocol headers
tests/
  test_virtioscsi.c     — stress test (concurrent I/O, geometry, 64-bit offsets)
```

---

## Changelog

### v1.3 build 1042 — 2026-02-23
- Fixed VIRTIO_F_EVENT_IDX kick-suppression bug: avail_event is zero at init time, causing all I/O after the first request to be silently dropped. EVENT_IDX disabled until the basic I/O path is proven stable.
- Release build (DEBUG disabled by default).

### v1.3 build 1041 — 2026-02-23
- Interrupt-driven I/O (Phase 5): replaced 5M-iteration polling loop with PCI INTx interrupt handler via `pciDevice->MapInterrupt()` + `IExec->AddIntServer()`. Falls back to polling if interrupt installation fails.
- Async I/O (Phase 6): per-unit exec task with message port. `BeginIO` posts slow commands and returns immediately; the unit task executes them and calls `ReplyMsg`. `AbortIO` removes pending requests from the port queue.
- Performance (Phase 7): pre-allocated per-unit `MEMF_SHARED` req/resp DMA buffers — no per-request allocation or DMA setup on the I/O hot path. `zero_shared()` volatile word loop for non-cacheable buffer zeroing (safe alternative to `ClearMem`/`SetMem` on `MEMF_SHARED`).
- Replaced deprecated `CachePreDMA`/`CachePostDMA` with `StartDMA`/`GetDMAList`/`EndDMA` throughout.
- Added `-fno-tree-loop-distribute-patterns` to Makefile: prevents GCC 11 `-O2` from replacing fill loops with `memset()` calls (newlib not linked in device drivers).
- Added VIRTIO_F_EVENT_IDX negotiation in virtio_init.c (later disabled — see build 1042).

### v1.3 build 1033 — 2026-02-22
- CDB helpers (Phase 1): `make_read10_cdb()`, `make_write10_cdb()`, `unpack_io64_offset()`, `ensure_geometry_cached()` — eliminated copy-paste across 6 files. Block size now read from device (4K sector support).
- Stub consolidation (Phase 2): merged `cmd_td_changestate.c`, `cmd_td_protstatus.c`, `cmd_td_getdrivetype.c`, `cmd_success.c` into `cmd_stubs.c`; merged `scsi_read_10.c` + `scsi_write_10.c` into `scsi_rw_10.c`.
- Init split (Phase 3): extracted SCSI INQUIRY scan and mounter announcement into `unit_discovery.c`.
- BeginIO cleanup (Phase 4): extracted `GetCommandName()` and `supported_commands[]` into `cmd_names.c`; `ns_devicequery.c` references the shared table.

### v1.2 build 1029 — 2026-02-21
- Multi-unit automounting: INQUIRY scan of up to 8 targets at Init, announced to `mounter.library`.
- Resolved Workbench boot hang: removed manual `AddDevice()`, rely on `RTF_AUTOINIT`.
- I/O semaphore (`io_lock`) protecting VirtIO queue submit window against concurrent access.
- `TD_GETDRIVETYPE` returns `DRIVE_NEWSTYLE` for correct 64-bit geometry handling.
- `TD_GETNUMTRACKS` fallback of 32768 prevents "empty drive" errors before geometry is cached.
- Migrated DMA API: `CachePreDMA` → `StartDMA`/`GetDMAList`/`EndDMA` (scatter-gather).
- Full 64-bit command coverage: `TD_READ64`, `TD_WRITE64`, all `ETD_*` and `NSCMD_ETD_*` variants.
- `CMD_UPDATE`/`CMD_FLUSH` wired to SCSI `SYNCHRONIZE CACHE(10)`.

### v1.0 — 2026-02-20
- Initial working driver: PCI discovery, VirtIO legacy init, real SCSI I/O (INQUIRY, READ CAPACITY, READ(10), WRITE(10)).
- Single-disk, single-partition operation.
