virtioscsi.device - VirtIO SCSI Device Driver for AmigaOS 4.1 FE
=================================================================
Version 1.7 - 18 March 2026
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

The driver auto-detects the best VirtIO transport for each QEMU machine
type - no platform-specific QEMU configuration required:

  Pegasos2  (MV64361 bridge)  - modern VirtIO 1.0 MMIO
  AmigaOne  (Articia S bridge) - legacy VirtIO I/O port access

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
SCSI disks. The same device type (virtio-scsi-pci) works on all
supported QEMU machines - the driver auto-detects the best transport:

  -device virtio-scsi-pci,id=scsi0 \
  -drive file=virtioscsi1.img,if=none,id=vd0,format=raw \
  -device scsi-hd,drive=vd0,bus=scsi0.0,channel=0,scsi-id=0,lun=0 \
  -drive file=virtioscsi2.img,if=none,id=vd1,format=raw \
  -device scsi-hd,drive=vd1,bus=scsi0.0,channel=0,scsi-id=1,lun=1

Replace virtioscsi1.img and virtioscsi2.img with your own hard drive
image files. You can attach fewer or more drives by adjusting the
-drive/-device scsi-hd pairs (up to 8 targets).

Note: Existing Pegasos2 setups using
-device virtio-scsi-pci-non-transitional continue to work. The
transitional device (virtio-scsi-pci) is recommended because it works
on all machines without changes.


FEATURES
--------
- Dual VirtIO transport: auto-detected via MMIO probe (modern on Pegasos2,
  legacy on AmigaOne, same QEMU config for both)
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

Automatic (from AmigaOS Shell):

  1. Extract the archive and open a Shell in the VirtualSCSIDevice
     directory.
  2. Run:  Execute Autoinstall
  3. The script copies virtioscsi.device to SYS:Kickstart/.
  4. Follow the on-screen instructions to add the MODULE line to
     your Kicklayout file, then reboot.

Manual installation:

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

v1.7 (18.03.2026)
  - Performance: bounce buffer increased 4KB to 64KB, eliminating DMA
    syscalls for most filesystem I/O. Word-aligned bounce copy (~4x).
    Pre-allocated DMA entry arrays. O(1) inflight slot allocation and
    cookie matching. Global occupied counter for interrupt coalescing.

v1.6 (18.03.2026)
  - Code review fixes: sub-block I/O rejected with IOERR_BADLENGTH,
    redundant semaphore dance in DoIO cross-unit path simplified,
    integer overflow in test capacity calculation fixed.
  - Build system: automatic header dependency tracking, test_inquiry
    added to default targets, stricter compiler warnings.
  - Cleanup: non-ASCII emoji replaced, SAM-2 LUN magic constant named,
    header guard naming fixed, build number removed from version string.

v1.5 (28.02.2026)
  - Pegasos2 support: VirtIO 1.0 Modern PCI transport (device 0x1048)
    with MMIO via stwbrx/lwbrx inline assembly. Auto-detected at boot
    alongside legacy transport (device 0x1004) for AmigaOne.
  - Modern VirtIO init: PCI capability chain walk, full VirtIO 1.0
    status handshake, three-address queue setup, per-queue MMIO notify,
    LE vring byte-swap wrappers.
  - Bug fixes: PCI Memory Space and Bus Master enable before MMIO;
    NULL-safe BAR0 dereference in modern mode; modern-aware queue notify
    in DoIO path; reset polling after device reset.

v1.4 (24.02.2026)
  - MAX_INFLIGHT increased from 8 to 16 for higher pipeline depth.
  - SCSI INQUIRY VPD pages (0x00, 0x80, 0x83) answered locally.
  - SCSI sense key mapped to specific AmigaOS io_Error codes.
  - READ CAPACITY (16) fallback for disks >= 2TB.
  - ATA PASS-THROUGH stub for SMART tool compatibility.

v1.3 (22.02.2026)
  - Interrupt-driven I/O, async I/O with per-unit exec task.
  - Performance: pre-allocated DMA buffers, bounce buffer ring,
    deferred kick batching, interrupt coalescing, pipelined block I/O.
  - Stability: cross-unit completion harvest, io_lock serialisation.

v1.2 (21.02.2026)
  - Multi-disk automounting, boot hang fix, I/O semaphore.
  - Full 64-bit command coverage. Modern DMA API.

v1.0 (20.02.2026)
  - Initial release: PCI discovery, VirtIO legacy init, real SCSI I/O.
    Single-disk, single-partition operation.


LICENSE
-------
See LICENSE file in the source repository.
https://github.com/derfsss/VirtualSCSIDevice
