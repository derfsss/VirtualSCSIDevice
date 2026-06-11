# virtioscsi.device

A VirtIO SCSI block device driver for AmigaOS 4.1 Final Edition running in QEMU virtual machines.

**Status:** Beta (v1.12) — boot-drive capable; validated on QEMU Pegasos2, AmigaOne, and SAM460ex.

> ⚠️ **Beta — actively under development.** Expect bugs and rough
> edges; do not rely on it for anything important. Use at your own
> risk.

---

## Overview

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
- **Multi-disk** — discovers up to 8 SCSI targets at boot; partitions get DOSNodes via `diskboot.kmod` (the standard AmigaOS disk-driver pattern)
- **Automounting** — all discovered partitions mount automatically without manual configuration
- **Full trackdisk command set** — `CMD_READ`, `CMD_WRITE`, `CMD_UPDATE`, `TD_GETGEOMETRY`, `TD_FORMAT`, `TD_READ64`, `TD_WRITE64`, NSD 64-bit commands, `HD_SCSICMD`, and more
- **>2 TiB disk support** — two-step geometry discovery: READ CAPACITY (10) first; if last LBA == 0xFFFFFFFF, falls back to READ CAPACITY (16) for the 64-bit block count. `TD_GETGEOMETRY` reports a synthesised logical CHS where `dg_Cylinders * dg_CylSectors == total_blocks` exactly so callers that compute size from CHS see the full disk; `NSCMD_TD_GETGEOMETRY64` reports the unclamped 64-bit count via `struct DriveGeometry64`. (Single AmigaOS partitions remain limited to ~2 TiB by the RDB protocol's 32-bit cylinder fields; spread the disk across multiple partitions to use the full capacity.)
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
- Also validated as a resident driver inside a hosted AmigaOS sandbox environment on AmigaOne X5000 hardware (v1.10+).

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

## Building from Source

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

## Project Structure

```
src/
  device.c              — resident tag, library base init
  Init.c                — library open: PCI discovery, VirtIO init, unit discovery
  Open.c / Close.c      — per-opener reference counting, unit task lifecycle
  Expunge.c             — library cleanup
  BeginIO.c             — I/O request dispatcher
  cmd_names.c           — command name table (shared by BeginIO and NSCMD_DEVICEQUERY)
  scsi_cdb_helpers.c    — CDB builders, geometry cache helper
  unit_discovery.c      — SCSI INQUIRY scan, unit struct setup
  unit_task.c           — per-unit exec task, pre-allocated DMA buffers
  exec_cmds/            — TD_GETGEOMETRY, TD_GETNUMTRACKS, CMD_UPDATE handlers
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
  test_inquiry.c        — NSCMD_DEVICEQUERY / INQUIRY / READ CAPACITY smoke test
```

---

## Version History

**v1.12 (2026-06-11)** — full code-review pass: fixed silent truncation of
single transfers above 32 MiB, modern-VirtIO endianness on device-written
fields, cross-task races in the in-flight bookkeeping, and several smaller
command-handling issues; removed dead code.  60+ checks pass per machine in
the automated QEMU test suite.

See [CHANGELOG.md](CHANGELOG.md) for the full release history and
[HISTORY.md](HISTORY.md) for the development log.

---

## Development

This driver was developed with [Claude](https://claude.ai) (Anthropic)
acting as the primary engineer — writing the C code, designing the
architecture, debugging hardware-level issues, and navigating the
AmigaOS 4.1 SDK — with a human developer directing, reviewing, and
testing the result.

[Kyvos](https://ko-fi.com/s/6476fdadd2) was used to develop and test
this driver.

## License

Copyright © 2026. All rights reserved.
