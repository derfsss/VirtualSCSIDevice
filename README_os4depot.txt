virtioscsi.device - VirtIO SCSI Device Driver for AmigaOS 4.1 FE
=================================================================
Version 1.11 - 15 May 2026


INTRODUCTION
------------
virtioscsi.device is a device driver for AmigaOS 4.1 Final Edition that
gives the operating system access to VirtIO SCSI virtual disks in QEMU
virtual machines.

The driver auto-detects the best VirtIO transport for each QEMU machine
type - no platform-specific QEMU configuration required. Tested on all
three QEMU PowerPC machines:

  Pegasos2  (MV64361 bridge)   - modern VirtIO 1.0 MMIO
  AmigaOne  (Articia S bridge) - modern VirtIO 1.0 MMIO (v1.9+)
  SAM460ex                     - modern VirtIO 1.0 MMIO

All three machines run the modern path; legacy PCI I/O is the
automatic fallback if the MMIO probe fails. VirtIO SCSI disks are
faster and more flexible than emulated IDE, and this driver makes them
available to AmigaOS as standard trackdisk-compatible block devices.
Partitions are discovered and mounted at boot automatically, and standard
filesystems (FFS2, SFS, etc.) work normally.


REQUIREMENTS
------------
- AmigaOS 4.1 Final Edition (PowerPC). Driver opens expansion.library
  v53, so it loads on every FE release from the 53.54 install CD
  through Update 3.
- QEMU with a supported machine type (amigaone, pegasos2, or sam460ex).
- Also runs as a SandboxVM resident on AmigaOne X5000 (v1.10+).


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
- >2 TiB disk support: READ CAPACITY (16) for 64-bit block count, plus
  a sii3112ide-compatible logical CHS in TD_GETGEOMETRY so partitions
  on disks larger than 2 TiB get DOSNodes and mount via diskboot.kmod.
  NSCMD_TD_GETGEOMETRY64 reports the unclamped 64-bit count. (Single
  partitions remain limited to ~2 TiB by AmigaOS RDB's 32-bit fields --
  use multiple partitions to span the full disk.)
- SCSI VPD pages (0x00, 0x80, 0x83) answered locally
- Accurate SCSI error codes mapped to AmigaOS io_Error values
- 4K sector support - block size read from device, not hardcoded
- DMA scatter-gather using AmigaOS 4.1 StartDMA/GetDMAList/EndDMA
- Pre-allocated DMA buffers for low-latency I/O hot path
- Bounce buffer ring for zero-overhead small I/O
- Interrupt coalescing via used_event batching
- VIRTIO_F_INDIRECT_DESC for one-descriptor scatter-gather chains
- SFS 1.290 / FFS2 / CDFileSystem all mount cleanly
- SandboxVM (X5000 host) compatibility via SBV_AVT_HostDMA tagging
- No deprecated AmigaOS APIs used (no Forbid/Permit, no CachePreDMA)


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

Output: build/virtioscsi.device  (stripped release build)
        build/virtioscsi.device.debug  (unstripped, same sources)

The release LHA includes both binaries -- the stripped one goes in
Kickstart/, and the .debug variant stays alongside it for diagnostic
sessions where symbol addresses help decode DSI/grim-reaper reports.


CHANGELOG
---------

v1.11 (15.05.2026)
  - >2 TiB partitions now mount. TD_GETGEOMETRY's dg_TotalSectors
    (uint32) is clamped at 0xFFFFFFFF instead of letting the cast
    wrap to 0. diskboot.kmod (2014) was treating TotalSectors=0 as
    "size unknown" and skipping the whole unit, so no partition --
    not even partitions within 32-bit LBA range -- ever got a
    DOSNode. With the clamp, virtioscsi behaves identically to
    sii3112ide on the same image: diskboot creates DOSNodes for
    every partition. NSCMD_TD_GETGEOMETRY64 still reports the
    unclamped 64-bit count for callers that ask.
  - sii3112-style logical CHS in TD_GETGEOMETRY: dg_Cylinders *
    dg_CylSectors == total_blocks exactly (largest power-of-2
    factor up to 256). Eliminates Media's "Total sectors: -NNN"
    rounding artifact; the reported disk size now matches sii3112
    bit-for-bit. RDB-declared physical CHS is no longer fed into
    TD_GETGEOMETRY -- filesystems use PartitionBlock for their LBA
    math and Media reads RDB directly for its physical-data panel.
  - Tighter RDB validation in ensure_rdb_geometry_cached: checks
    rdb_SummedLongs is plausible and verifies the longword checksum.
    Prevents stray "RDSK" magics in stale start-of-file data being
    accepted as a valid RDB.
  - RDB no longer cached across TD_GETGEOMETRY calls: the on-disk
    RDB can be rewritten by Media at any time, re-read each time so
    geometry tracks the current state. Capacity (RC10/RC16) is
    still cached.
  - Test suite expanded with TESTS 14-17: held-async semantics,
    NSCMD_TD_GETGEOMETRY64 round-trip, ATA pass-through (CDB 0x85),
    HD_SCSICMD unsupported-opcode auto-sense. TEST 3 assertion fixed
    (beyond-EOF NSCMD_TD_READ64 maps to IOERR_NOCMD via SCSI sense
    decoding -- previously asserted not-IOERR_NOCMD which always
    failed).
  - Dormant code removed: src/virtio/virtio_events.c (v1.9 event-queue
    consumer) and src/virtio/virtio_mounter.c (v1.9 hot-add
    mounter.library integration), both disabled in v1.10 and never
    revisited. Recoverable via git log if needed.

v1.10 (17.04.2026)
  - SFS 1.290 compatibility: explicit 68k-compatible jump table
    (CLT_Vector68K) ensures BeginIO at offset -30 and AbortIO at -36
    are reachable from legacy handlers. Resident struct placed in
    writable .data (matches all shipping OS4 IDE drivers). DriveGeometry
    struct zeroed before filling (SFS checks dg_Reserved).
    dg_BufMemType set to MEMF_PUBLIC|MEMF_LOCAL to keep DOSEnvVec in
    BPTR-safe low RAM. TD_GETDRIVETYPE returns DRIVE3_5 (matching
    a1ide.device). dev_Unit properly initialised with NT_MSGPORT +
    UNITF_ACTIVE.
  - RDB geometry caching: TD_GETGEOMETRY reads the RDB header and first
    PartitionBlock to report CHS that matches the on-disk layout.
  - lib_Version pinned to 53: SFS 1.290 rejects devices with
    lib_Version below ~50. Display version is 1.10 but the Resident
    struct reports 53.10.
  - Makefile dual-build: separate release (stripped) and debug (with
    DPRINTF + symbols) targets built in parallel.
  - Stress suite updated: accepts --port, --monitor, --volume CLI args.
    Shell-run test checks serial log for diagnostic. 9P tier detects
    already-mounted SHARED:.
  - SandboxVM compatibility (X5000 host): every AllocVecTags whose
    buffer flows into StartDMA is tagged with SBV_AVT_HostDMA. No-op
    on native AOS4 (the tag value is unknown so utility.library
    ignores it); on SandboxVM it routes the allocation through the
    host allocator so the buffer is DMA-mappable.
  - expansion.library minimum lowered to v53. v54 only ships in FE
    Update 3; with v53 the driver loads on every FE release from the
    53.54 install CD onward (CD, U1, U2, U3) on every platform.
  - Validated 14/14 on AmigaOne, Pegasos2, and SAM460ex.

v1.9 (15.04.2026)
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
  - mounter.library integration: the driver opens mounter.library
    lazily and uses it to clean up DOSNodes at Expunge. mounter is
    closed after denouncing any remaining units. Non-fatal if mounter
    is unavailable -- units stay reachable via OpenDevice.
  - Shell-run diagnostic: _start() now prints an error via
    IExec->DebugPrintF and returns RETURN_FAIL (20) instead of 0.
  - Version renumbered: v53.8 -> v1.9. Boot drive support is unchanged
    (resident priority 0 + diskboot.config entry "virtioscsi.device 8 3").
  - Validated against a 12-check stress suite on all three QEMU PowerPC
    machines (AmigaOne, Pegasos2, SAM460ex): data-integrity round-trips
    at tiny / 90 KB / 554 KB / 1.26 MB, dir copy, 100-iteration
    upload/download loop, three parallel on-volume copies, double
    Open/Close UAF guard, and a 2-minute baseline-normalised soak. SCSI
    memory drift stays below the IDE baseline on every machine -- no
    per-I/O leak signal.

v53.8 (14.04.2026)
  - Boot drive support: VirtIO SCSI disks can be used as boot drives
    when booting via BBoot (loads Kickstart from memory so the
    firmware does not need a virtio-scsi driver). Resident priority
    changed to 0 (matching a1ide.device, peg2ide.device).  Major
    version bumped to 53 (AmigaOS 4.1 FE SDK convention). Tested
    with BBoot on AmigaOne, Pegasos2, and SAM460ex.

    NOTE: Real Pegasos2 firmware (pegasos2.rom) does not know about
    virtio-scsi-pci and cannot select it as a boot target. On that
    setup boot AmigaOS from a small IDE (or NVMe) disk, and the
    virtio-scsi disks will be mounted as secondary devices once the
    driver loads.
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
