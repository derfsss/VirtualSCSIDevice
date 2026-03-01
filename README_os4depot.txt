virtioscsi.device - VirtIO SCSI Device Driver for AmigaOS 4.1 FE
=================================================================
Version 1.5 (Build 1079) - 28 February 2026
Author: derfsss
Source: https://github.com/derfsss/VirtualSCSIDevice


DEVELOPED WITH AI
-----------------
This driver was developed with Claude AI (Anthropic) acting as the primary
engineer - writing all C code, designing the architecture, debugging
hardware-level issues, and navigating the AmigaOS 4.1 SDK. It is a
practical demonstration of AI-assisted low-level systems programming on a
niche, legacy platform with minimal AI training data available.

Kyvos (https://ko-fi.com/s/6476fdadd2) was used to develop and test this
device driver.


INTRODUCTION
------------
virtioscsi.device is a device driver for AmigaOS 4.1 Final Edition that
gives the operating system access to VirtIO SCSI virtual disks in QEMU
virtual machines.

This driver supports both QEMU machine types:

  AmigaOne  - uses VirtIO Legacy PCI (device 0x1004) via I/O port access
  Pegasos2  - uses VirtIO 1.0 Modern PCI (device 0x1048) via MMIO

The correct transport is auto-detected at boot. VirtIO SCSI disks are
faster and more flexible than emulated IDE, and this driver makes them
available to AmigaOS as standard trackdisk-compatible block devices.
Partitions are discovered and mounted at boot automatically, and standard
filesystems (FFS2, SFS, etc.) work normally.


REQUIREMENTS
------------
- AmigaOS 4.1 Final Edition (PowerPC)
- QEMU with one of the supported machine types (amigaone or pegasos2)


QEMU SETUP
----------
Add the following to your existing QEMU command line to attach VirtIO
SCSI disks.

AmigaOne (-M amigaone):

  AmigaOne uses the legacy/transitional VirtIO device (virtio-scsi-pci):

  -device virtio-scsi-pci,id=scsi0 \
  -drive file=virtioscsi1.img,if=none,id=vd0,format=raw \
  -device scsi-hd,drive=vd0,bus=scsi0.0,channel=0,scsi-id=0,lun=0 \
  -drive file=virtioscsi2.img,if=none,id=vd1,format=raw \
  -device scsi-hd,drive=vd1,bus=scsi0.0,channel=0,scsi-id=1,lun=1

Pegasos2 (-M pegasos2):

  Pegasos2 requires the non-transitional (modern-only) VirtIO device
  (virtio-scsi-pci-non-transitional):

  -device virtio-scsi-pci-non-transitional,id=scsi0 \
  -drive file=virtioscsi1.img,if=none,id=vd0,format=raw \
  -device scsi-hd,drive=vd0,bus=scsi0.0,channel=0,scsi-id=0,lun=0 \
  -drive file=virtioscsi2.img,if=none,id=vd1,format=raw \
  -device scsi-hd,drive=vd1,bus=scsi0.0,channel=0,scsi-id=1,lun=1

Replace virtioscsi1.img and virtioscsi2.img with your own hard drive
image files. You can attach fewer or more drives by adjusting the
-drive/-device scsi-hd pairs (up to 8 targets).


FEATURES
--------
- Dual VirtIO transport: Legacy PCI (AmigaOne) and Modern 1.0 (Pegasos2)
- Interrupt-driven I/O - no CPU-burning polling loops
- Asynchronous I/O - per-unit exec task with message port
- Discovers up to 8 SCSI targets at boot
- All discovered partitions automount via mounter.library
- Full trackdisk command set including 64-bit NSD commands
- >2TB disk support via READ CAPACITY (16)
- SCSI VPD pages (0x00, 0x80, 0x83) answered locally
- Accurate SCSI error codes mapped to AmigaOS io_Error values
- 4K sector support - block size read from device, not hardcoded
- DMA scatter-gather using AmigaOS 4.1 StartDMA/GetDMAList/EndDMA
- Pre-allocated DMA buffers for low-latency I/O hot path
- Bounce buffer ring for zero-overhead small I/O
- Interrupt coalescing via used_event batching
- No deprecated AmigaOS APIs used


INSTALLATION
------------

Using BBoot (Kickstart zip archive):

  BBoot (https://codeberg.org/qmiga/bboot/) boots AmigaOS from a zip
  archive containing all Kickstart modules.

  1. Add virtioscsi.device to the Kickstart/ folder inside your BBoot
     zip archive.

  2. Edit the Kicklayout file inside the zip archive and add the
     following line after the existing boot device driver entry (e.g.
     after MODULE Kickstart/a1ide.device.kmod for AmigaOne, or after
     MODULE Kickstart/peg2ide.device.kmod for Pegasos2):

       MODULE Kickstart/virtioscsi.device

  3. Save the zip archive and boot with BBoot as normal.

Without BBoot (SYS:Kickstart folder):

  If you are not using BBoot and have AmigaOS installed on a bootable
  disk:

  1. Copy virtioscsi.device to the SYS:Kickstart/ folder on your
     AmigaOS system disk.

  2. Edit the SYS:Kickstart/Kicklayout file and add the following line
     after the existing boot device driver entry (e.g. after
     MODULE Kickstart/a1ide.device.kmod for AmigaOne, or after
     MODULE Kickstart/peg2ide.device.kmod for Pegasos2):

       MODULE Kickstart/virtioscsi.device

  3. Save and reboot. The driver will be resident in memory from the
     very start of the boot process.

  Note: The driver has a resident priority of -60 so it initialises
  after mounter.library. Ensure mounter.library is also present in
  the Kickstart set when using this method.


COMPILING FROM SOURCE
---------------------
The project cross-compiles on Linux or WSL2 using Docker.

Prerequisites:
  - Docker with the image: walkero/amigagccondocker:os4-gcc11

Build command (from the project root directory in a WSL2 terminal):

  docker run --rm -v $(pwd):/src -w /src \
    walkero/amigagccondocker:os4-gcc11 make

Output: build/virtioscsi.device

Source code: https://github.com/derfsss/VirtualSCSIDevice


CHANGELOG
---------

v1.5 build 1079 (28.02.2026)
  - Pegasos2 support: VirtIO 1.0 Modern PCI transport (device 0x1048)
    with MMIO via stwbrx/lwbrx inline assembly. Auto-detected at boot
    alongside legacy transport (device 0x1004) for AmigaOne.
  - Modern VirtIO init: PCI capability chain walk, full VirtIO 1.0
    status handshake, three-address queue setup, per-queue MMIO notify,
    LE vring byte-swap wrappers.
  - Bug fixes: PCI Memory Space and Bus Master enable before MMIO;
    NULL-safe BAR0 dereference in modern mode; modern-aware queue notify
    in DoIO path; reset polling after device reset.

v1.4 build 1070 (24.02.2026)
  - MAX_INFLIGHT increased from 8 to 16 for higher pipeline depth.

v1.4 build 1069 (24.02.2026)
  - SCSI INQUIRY VPD pages (0x00, 0x80, 0x83) answered locally.

v1.4 build 1068 (24.02.2026)
  - SCSI sense key mapped to specific AmigaOS io_Error codes.

v1.4 build 1067 (24.02.2026)
  - READ CAPACITY (16) fallback for disks >= 2TB. Version bumped to v1.4.

v1.3 build 1065 (24.02.2026)
  - ATA PASS-THROUGH stub for SMART tool compatibility.

v1.3 build 1063 (24.02.2026)
  - Interrupt coalescing via used_event batching.

v1.3 build 1062 (24.02.2026)
  - Bounce buffer ring for zero-overhead small I/O.

v1.3 build 1061 (24.02.2026)
  - Deferred kick: batch QUEUE_NOTIFY for burst I/O.

v1.3 build 1060 (24.02.2026)
  - Bug fix: Harvest discarding DoIO cookies (release-build Heisenbug).

v1.3 build 1059 (23.02.2026)
  - Bug fix: DoIO inner-loop missing break (release-build Heisenbug).

v1.3 build 1058 (23.02.2026)
  - Cross-unit VirtIO completion harvest. Both drives now appear.

v1.3 build 1057 (23.02.2026)
  - Serialised VirtQueue_GetBuf() with io_lock for shared VQ2.

v1.3 build 1055 (23.02.2026)
  - Disabled EVENT_IDX kick suppression (QEMU legacy never writes
    avail_event). Unconditional QUEUE_NOTIFY on every kick.

v1.3 build 1047 (23.02.2026)
  - Pipelined block I/O with up to 8 simultaneous in-flight requests.
  - Persistent per-unit ISR signal. Pre-allocated DMA slots.

v1.3 build 1042 (23.02.2026)
  - Fixed EVENT_IDX kick-suppression bug. Release build.

v1.3 build 1041 (23.02.2026)
  - Interrupt-driven I/O, async I/O with per-unit exec task.
  - Pre-allocated DMA buffers. Modern DMA API throughout.

v1.3 build 1033 (22.02.2026)
  - CDB helpers, stub consolidation, init split, BeginIO cleanup.

v1.2 build 1029 (21.02.2026)
  - Multi-disk automounting, boot hang fix, I/O semaphore.
  - Full 64-bit command coverage. Modern DMA API.

v1.0 (20.02.2026)
  - Initial release: PCI discovery, VirtIO legacy init, real SCSI I/O.
    Single-disk, single-partition operation.


LICENSE
-------
See LICENSE file in the source repository.
https://github.com/derfsss/VirtualSCSIDevice
