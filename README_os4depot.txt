virtioscsi.device - VirtIO SCSI Device Driver for AmigaOS 4.1 FE
=================================================================
Version 1.9 - 14 April 2026
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
type - no platform-specific QEMU configuration required. Tested on all
three QEMU PowerPC machines:

  Pegasos2  (MV64361 bridge)   - modern VirtIO 1.0 MMIO
  AmigaOne  (Articia S bridge) - legacy VirtIO I/O port access
  SAM460ex                     - legacy VirtIO I/O port access

The correct transport is auto-detected at boot. VirtIO SCSI disks are
faster and more flexible than emulated IDE, and this driver makes them
available to AmigaOS as standard trackdisk-compatible block devices.
Partitions are discovered and mounted at boot automatically, and standard
filesystems (FFS2, SFS, etc.) work normally.


REQUIREMENTS
------------
- AmigaOS 4.1 Final Edition (PowerPC)
- QEMU with a supported machine type (amigaone, pegasos2, or sam460ex)


QEMU SETUP
----------
Add the following to your existing QEMU command line to attach VirtIO
SCSI disks. The same device type (virtio-scsi-pci) works on all
supported QEMU machines - the driver auto-detects the best transport:

  -device virtio-scsi-pci,id=scsi0 \
  -drive file=image_file.img,if=none,id=vd0,format=raw \
  -device scsi-hd,drive=vd0,bus=scsi0.0,channel=0,scsi-id=0,lun=0

Replace image_file.img with the path to your hard drive image file.
You can attach additional drives by adding more -drive/-device scsi-hd
pairs (up to 8 targets):

  -drive file=second_disk.img,if=none,id=vd1,format=raw \
  -device scsi-hd,drive=vd1,bus=scsi0.0,channel=0,scsi-id=1,lun=1

IMPORTANT: The format= parameter must match your image file's actual
format. Use format=raw for .img/.raw files and format=qcow2 for
.qcow2 files. A mismatch (e.g. format=raw on a .qcow2 file) causes
silent boot failures - diskboot reads garbage from the disk and
can't find a bootable partition.

Note: Existing Pegasos2 setups using
-device virtio-scsi-pci-non-transitional continue to work. The
transitional device (virtio-scsi-pci) is recommended because it works
on all machines without changes.


FEATURES
--------
- Dual VirtIO transport: auto-detected via MMIO probe (modern on
  Pegasos2, legacy on AmigaOne/SAM460, same QEMU config for all)
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

VirtIO SCSI disks can be used as boot drives or as additional data
drives. The driver must be added to the Kickstart module set and
registered with diskboot.config so the system can find bootable
partitions.

Using BBoot (Kickstart zip archive):

  BBoot (https://codeberg.org/qmiga/bboot/) boots AmigaOS from a zip
  archive containing all Kickstart modules.

  1. Add virtioscsi.device to the Kickstart/ folder inside your BBoot
     zip archive.

  2. Edit the Kicklayout file inside the zip archive. Add the
     following line just BEFORE the diskboot.config and diskboot.kmod
     entries:

       MODULE Kickstart/virtioscsi.device

     For example, the relevant section should look like:

       MODULE Kickstart/peg2ide.device.kmod
       MODULE Kickstart/virtioscsi.device
       MODULE Kickstart/diskboot.config
       MODULE Kickstart/diskboot.kmod

  3. Edit Kickstart/diskboot.config inside the zip archive and add:

       virtioscsi.device 8 3

  4. Save the zip archive and boot with BBoot as normal.

Without BBoot (SYS:Kickstart folder):

  If you are not using BBoot and have AmigaOS installed on a bootable
  disk:

  1. Copy virtioscsi.device to the SYS:Kickstart/ folder on your
     AmigaOS system disk.

  2. Edit SYS:Kickstart/Kicklayout and add the following line just
     BEFORE the diskboot.config and diskboot.kmod entries:

       MODULE Kickstart/virtioscsi.device

  3. Edit SYS:Kickstart/diskboot.config and add the following line:

       virtioscsi.device 8 3

  4. Save and reboot. The driver will be resident in memory from the
     very start of the boot process, and VirtIO SCSI disks can be
     used as boot drives.


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

v1.9 (14.04.2026)
  - Modern VirtIO MMIO on AmigaOne: runtime workaround in pci_discovery.c
    for a 64-bit BAR firmware bug. Before v1.9, BAR4's high DWORD was
    left at 0xFFFFFFFF on AmigaOne (BBoot doesn't program it and
    AmigaOS's PCI enumerator leaves it at the probe value), putting the
    MMIO BAR outside Articia's PCI memory window. The driver now reads
    BAR5 at discovery and writes 0 back if it reads 0xFFFFFFFF. AmigaOne
    now uses the ~10-20x faster modern MMIO transport.
  - VIRTIO_RING_F_INDIRECT_DESC accepted on modern path: scatter-gather
    chains consume one vring descriptor regardless of SG count. Fixed
    byte-swap bugs in the indirect-table writes.
  - VIRTIO_SCSI_F_HOTPLUG / F_CHANGE accepted: event queue (VQ1) now
    carries async device events. A consumer task drains events and
    handles TRANSPORT_RESET (rescan/removed), PARAM_CHANGE (media or
    size change), and ASYNC_NOTIFY.
  - CD / DVD media change: PARAM_CHANGE events with ASC 0x28 (medium
    inserted) or 0x3A (medium not present) bump the per-unit change
    counter, toggle media_present, invalidate cached geometry, and wake
    any held TD_ADDCHANGEINT. TD_CHANGENUM and TD_CHANGESTATE now report
    real values. Use (qemu) eject/change scsi0-0-0-0 to swap CDs live.
  - Phase 10 mounter integration: hot-added disks (device_add scsi-hd
    in QEMU monitor) are announced to mounter.library, so the disk
    appears on the Workbench without a reboot. Removal (device_del)
    triggers DenounceDevice. mounter is opened lazily on first hot-add
    (never at boot, to avoid the old MediaToolbox crash) and closed in
    Expunge after denouncing any remaining units. Non-fatal if mounter
    is unavailable -- units stay reachable via OpenDevice.
  - Shell-run diagnostic: _start() now prints an error via
    IExec->DebugPrintF and returns RETURN_FAIL (20) instead of 0.
  - Version renumbered: v53.8 -> v1.9. Boot drive support is unchanged
    (resident priority 0 + diskboot.config entry "virtioscsi.device 8 3").

v53.8 (14.04.2026)
  - Boot drive support: VirtIO SCSI disks can now be used as boot
    drives. Resident priority changed to 0 (matching other AmigaOS
    disk drivers like a1ide.device, peg2ide.device). Major version
    bumped to 53 (AmigaOS 4.1 FE SDK convention). Tested as boot
    drive on AmigaOne, Pegasos2, and SAM460ex.
  - MediaToolbox crash fix: removed explicit mounter.library
    AnnounceDeviceTags() call from unit_discovery.c. With priority 0,
    mounter is not yet initialised when our driver loads, so the call
    corrupted state. Driver now matches the standard AmigaOS disk
    driver pattern - diskboot.kmod handles all DOSNode creation via
    the diskboot.config entry.
  - Installation: requires diskboot.config entry "virtioscsi.device 8 3"
    AND MODULE Kickstart/virtioscsi.device line BEFORE diskboot.config
    and diskboot.kmod entries in the Kicklayout file.
  - Binary size: reduced from 82KB to 41KB. Linker flags collapse 28KB
    of section padding; strip removes 12KB of symbol/debug tables.
    Strip auto-skipped for debug builds. The release LHA includes both
    virtioscsi.device (41KB stripped) and virtioscsi.device.debug (83KB
    unstripped) for diagnostics.
  - Build: dynamic build date/time stamps via Makefile. Boot serial
    output shows: virtioscsi.device 53.8 (DD.MM.YYYY) [HH:MM]
  - Documentation: added warning about matching QEMU format= parameter
    to actual image file format (raw vs qcow2). Mismatch causes silent
    boot failures.

v1.8 (11.04.2026)
  - Unified platform: single -device virtio-scsi-pci works on all QEMU
    machines (AmigaOne, Pegasos2, SAM460ex). MMIO probe auto-detects
    transport at boot. Tested on all three machines.
  - Performance: cacheable bounce buffers replace non-cacheable volatile
    copy — CopyMem + CacheClearE for DMA coherency (~10-20x faster for
    <=64KB I/O). O(1) cross-unit cookie routing. ISR occupancy bitmask
    skips inactive units.
  - Debug: error-path instrumentation across all command handlers.
  - Build: fixed header guard collision.

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
