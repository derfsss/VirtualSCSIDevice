virtioscsi.device - VirtIO SCSI Device Driver for AmigaOS 4.1 FE
=================================================================
Version 1.3 (Build 1042) - 23 February 2026
Author: derfsss
Source: https://github.com/derfsss/VirtualSCSIDevice


INTRODUCTION
------------
virtioscsi.device is a device driver for AmigaOS 4.1 Final Edition that
gives the operating system access to VirtIO SCSI virtual disks in QEMU/KVM
virtual machines.

When running AmigaOS 4.1 FE inside QEMU using the Pegasos2 machine type,
VirtIO SCSI is a fast and flexible alternative to emulated IDE. This driver
makes those virtual disks available to AmigaOS as standard trackdisk-
compatible block devices. Partitions are discovered and mounted at boot
automatically, and standard filesystems (FFS2, SFS, etc.) work normally.


REQUIREMENTS
------------
- AmigaOS 4.1 Final Edition (PowerPC)
- QEMU/KVM configured with -device virtio-scsi-pci

Example QEMU command line:

  qemu-system-ppc -M pegasos2 -bios pegasos2.rom \
    -device virtio-scsi-pci,id=scsi0 \
    -drive file=amigaos.img,if=none,id=hd0 \
    -device scsi-hd,drive=hd0,bus=scsi0.0


FEATURES
--------
- Supports QEMU VirtIO SCSI (PCI Vendor 0x1AF4, Device 0x1004)
- Interrupt-driven I/O - no CPU-burning polling loops
- Asynchronous I/O - per-unit exec task with message port
- Discovers up to 8 SCSI targets at boot
- All discovered partitions automount via mounter.library
- Full trackdisk command set including 64-bit NSD commands
- 4K sector support - block size read from device, not hardcoded
- DMA scatter-gather using AmigaOS 4.1 StartDMA/GetDMAList/EndDMA
- Pre-allocated DMA buffers for low-latency I/O hot path
- No deprecated AmigaOS APIs used


INSTALLATION
------------

Method 1: Standard (copy to DEVS:)

  1. Copy virtioscsi.device to DEVS: on your AmigaOS system.
  2. Reboot.
  3. The driver loads automatically at startup and announces all
     discovered disks to mounter.library.

Method 2: Kickstart module (KickLayout)

  Use this method if your boot disk is a VirtIO SCSI disk, so the driver
  is available before any filesystem is mounted.

  1. Copy virtioscsi.device to your Kickstart module directory
     (e.g. BOOT:Kickstart/).

  2. Add the following line to your KickLayout file:

       KICKMODULE DEVS:virtioscsi.device

     Or add the file via the Kickstart preferences tool.

  3. Reboot. The driver will be resident from the very start of the
     boot sequence.

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

v1.3 build 1042 (23.02.2026)
  - Fixed critical bug: VIRTIO_F_EVENT_IDX kick suppression caused all
    I/O after the first request to be silently dropped. The avail_event
    field in the used ring is zero at init time, which the suppression
    condition incorrectly treated as "do not notify". EVENT_IDX feature
    negotiation disabled until this is resolved properly.
  - Release build (no debug output).

v1.3 build 1041 (23.02.2026)
  - Interrupt-driven I/O: replaced CPU polling with PCI INTx interrupt
    handler. Falls back to polling if interrupt installation fails.
  - Asynchronous I/O: per-unit exec task with message port. BeginIO
    posts slow commands and returns immediately. AbortIO removes pending
    requests from the port queue.
  - Performance: pre-allocated per-unit MEMF_SHARED DMA buffers
    eliminate all per-request allocation and DMA setup on the I/O path.
  - Replaced deprecated CachePreDMA/CachePostDMA with the modern
    StartDMA/GetDMAList/EndDMA API throughout.
  - Build fix: added -fno-tree-loop-distribute-patterns to prevent
    GCC 11 from generating memset() calls in -nostartfiles device
    drivers.

v1.3 build 1033 (22.02.2026)
  - CDB helpers: eliminated copy-paste CDB construction across 6 files.
    Block size is now read from the device (4K sector support).
  - Consolidated thin stub files and duplicate SCSI read/write handlers.
  - Split unit discovery (SCSI INQUIRY scan + mounter announcement)
    into its own module.
  - Shared command name table between BeginIO dispatcher and
    NSCMD_DEVICEQUERY handler.

v1.2 build 1029 (21.02.2026)
  - Multi-disk: discovers up to 8 SCSI targets at boot, all automounted.
  - Fixed boot hang caused by redundant manual AddDevice() call.
  - I/O semaphore protecting VirtIO queue submit window.
  - TD_GETDRIVETYPE returns DRIVE_NEWSTYLE for correct 64-bit geometry.
  - Full 64-bit command coverage: TD_READ64, TD_WRITE64, all ETD and
    NSCMD_ETD variants.
  - CMD_UPDATE/CMD_FLUSH wired to SCSI SYNCHRONIZE CACHE(10).
  - Migrated to modern DMA API with scatter-gather support.

v1.0 (20.02.2026)
  - Initial release: PCI discovery, VirtIO legacy init, real SCSI I/O.
    Single-disk, single-partition operation.


LICENSE
-------
See LICENSE file in the source repository.
https://github.com/derfsss/VirtualSCSIDevice
