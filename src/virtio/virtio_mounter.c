#include "virtio/virtio_mounter.h"
#include "version.h"
#include "virtioscsi.h"
#include <interfaces/exec.h>
#include <libraries/mounter.h>
#include <utility/tagitem.h>

/*
 * Phase 10: mounter.library integration for hot-added VirtIO SCSI units.
 *
 * Design summary
 * --------------
 * Boot-time disks are announced by diskboot.kmod (which reads our
 * diskboot.config entry "virtioscsi.device 8 3" and creates DOSNodes from
 * the on-disk RDB).  Hot-added disks discovered at runtime via
 * VIRTIO_SCSI_T_TRANSPORT_RESET / VIRTIO_SCSI_EVT_RESET_RESCAN have no
 * such mechanism and need explicit publication via mounter.library so
 * they appear on the desktop without a reboot.
 *
 * mounter.library is opened LAZILY on the first hot-add event because:
 *   1. At boot, the driver runs at resident priority 0 and mounter is not
 *      yet initialised.  Calling AnnounceDeviceTags from Init crashed
 *      MediaToolbox in earlier driver versions (see HISTORY.md v53.8).
 *   2. The boot-time path doesn't need it (diskboot.kmod handles boot disks).
 *   3. Many systems will never hot-add a disk, so opening eagerly wastes
 *      a library handle.
 *
 * Per-unit `announced` flag tracks ownership: TRUE means mounter holds the
 * unit and we MUST DenounceDevice() before freeing or repurposing.  The
 * flag is set/cleared only by the helpers below.
 *
 * Failure handling
 * ----------------
 * Mounter being unavailable (library missing, OpenLibrary fails, interface
 * GetInterface fails) is non-fatal -- the unit still appears in units[] and
 * remains usable via OpenDevice("virtioscsi.device", N, ...).  It just
 * won't appear automatically on the Workbench.
 *
 * Resource discipline
 * -------------------
 * Every successful OpenLibrary is matched by exactly one CloseLibrary.
 * Every successful GetInterface is matched by exactly one DropInterface.
 * Every successful AnnounceDeviceTags is matched by exactly one
 * DenounceDevice (CleanupMounter walks units[] to ensure this).
 *
 * Threading
 * ---------
 * EnsureMounter / AnnounceIfHotAdded / DenounceIfAnnounced are called only
 * from the event task (single-threaded for events).  CleanupMounter is
 * called from _manager_Expunge AFTER ShutdownEventQueue has joined the
 * event task -- so by the time CleanupMounter runs, no concurrent caller
 * exists.
 */

/* Minimum mounter.library version we require.  v53 is the AmigaOS 4.1 FE
 * baseline; AnnounceDevice has been present since v50 but v53 matches the
 * SDK headers we're building against. */
#define MOUNTER_MIN_VERSION 53

BOOL EnsureMounter(struct VirtIOSCSIBase *libBase)
{
    struct ExecIFace *IExec = libBase->IExec;

    /* Already open from a previous call -- fast path. */
    if (libBase->IMounter != NULL)
        return TRUE;

    /* Defensive: if a prior partial-open left MounterBase set without a
     * usable interface, close it before retrying.  Should never happen with
     * the cleanup paths below but guards against future refactor mistakes. */
    if (libBase->MounterBase != NULL) {
        IExec->CloseLibrary(libBase->MounterBase);
        libBase->MounterBase = NULL;
    }

    libBase->MounterBase = IExec->OpenLibrary("mounter.library", MOUNTER_MIN_VERSION);
    if (libBase->MounterBase == NULL) {
        /* mounter.library is not available -- likely the system hasn't
         * progressed past pre-mount or the user has stripped it.  Log once
         * per attempt; caller treats FALSE as "skip auto-mount". */
        DPRINTF(IExec, "[virtioscsi:virtio_mounter.c] mounter.library v%lu unavailable\n",
                (uint32)MOUNTER_MIN_VERSION);
        return FALSE;
    }

    libBase->IMounter = (struct MounterIFace *)IExec->GetInterface(
        libBase->MounterBase, "main", 1, NULL);
    if (libBase->IMounter == NULL) {
        DPRINTF(IExec, "[virtioscsi:virtio_mounter.c] mounter.library 'main' interface unavailable\n");
        IExec->CloseLibrary(libBase->MounterBase);
        libBase->MounterBase = NULL;
        return FALSE;
    }

    DPRINTF(IExec, "[virtioscsi:virtio_mounter.c] mounter.library opened (base=%p iface=%p)\n",
            (void *)libBase->MounterBase, (void *)libBase->IMounter);
    return TRUE;
}

BOOL AnnounceIfHotAdded(struct VirtIOSCSIBase *libBase,
                        struct VirtIOUSCSIDevUnit *unit)
{
    if (unit == NULL)
        return FALSE;

    struct ExecIFace *IExec = libBase->IExec;

    /* Idempotency: if we already handed this unit to mounter, do nothing.
     * Re-announcing the same unit number while mounter still owns it would
     * confuse mounter and likely leak a DOSNode. */
    if (unit->announced) {
        DPRINTF(IExec,
                "[virtioscsi:virtio_mounter.c] AnnounceIfHotAdded: unit %lu already announced -- skipping\n",
                unit->unit_num);
        return TRUE;
    }

    if (!EnsureMounter(libBase))
        return FALSE;

    /* AnnounceDeviceTags is non-blocking; mounter performs the actual
     * partition scan in its own context.  The DOS name prefix hint helps
     * users identify hot-added units (e.g. "VSCSI0:") versus boot-time
     * disks named from the RDB (e.g. "DH0:", "Work:").
     *
     * Note: the execDeviceName string memory must remain valid until we
     * call DenounceDevice for this unit (per AutoDoc).  DEVNAME is a
     * compile-time constant string in the device's RTF segment, which lives
     * for the entire device lifetime -- no copy required.
     */
    BOOL ok = libBase->IMounter->AnnounceDeviceTags(
        DEVNAME,
        unit->unit_num,
        MNTA_DosNamePrefixHint, (uint32)"VSCSI",
        TAG_DONE);

    if (!ok) {
        DPRINTF(IExec,
                "[virtioscsi:virtio_mounter.c] AnnounceIfHotAdded: AnnounceDeviceTags(%s, %lu) refused\n",
                DEVNAME, unit->unit_num);
        return FALSE;
    }

    unit->announced = TRUE;
    DPRINTF(IExec,
            "[virtioscsi:virtio_mounter.c] AnnounceIfHotAdded: unit %lu (T%lu L%lu) announced to mounter\n",
            unit->unit_num, unit->target_id, unit->lun_id);
    return TRUE;
}

void DenounceIfAnnounced(struct VirtIOSCSIBase *libBase,
                         struct VirtIOUSCSIDevUnit *unit)
{
    if (unit == NULL || !unit->announced)
        return;

    struct ExecIFace *IExec = libBase->IExec;

    /* Even if mounter is no longer open, clear the local flag so we don't
     * try again later.  In practice the library will be open here because
     * AnnounceIfHotAdded held it open, but defensively check anyway. */
    if (libBase->IMounter != NULL) {
        libBase->IMounter->DenounceDevice(DEVNAME, unit->unit_num);
        DPRINTF(IExec,
                "[virtioscsi:virtio_mounter.c] DenounceIfAnnounced: unit %lu denounced from mounter\n",
                unit->unit_num);
    } else {
        DPRINTF(IExec,
                "[virtioscsi:virtio_mounter.c] DenounceIfAnnounced: unit %lu mounter gone -- clearing flag only\n",
                unit->unit_num);
    }

    unit->announced = FALSE;
}

void CleanupMounter(struct VirtIOSCSIBase *libBase)
{
    struct ExecIFace *IExec = libBase->IExec;

    /* First pass: denounce every still-announced unit so mounter can drop
     * its DOSNodes cleanly.  Walking units[] is safe because the event task
     * has already exited (caller's contract: ShutdownEventQueue first).  */
    for (int i = 0; i < MAX_UNITS; i++) {
        struct VirtIOUSCSIDevUnit *unit = libBase->units[i];
        if (unit && unit->announced)
            DenounceIfAnnounced(libBase, unit);
    }

    /* Second pass: drop the interface and close the library, in that order.
     * Order matters -- DropInterface decrements the library's open count
     * managed by GetInterface; CloseLibrary then decrements OpenLibrary's
     * count.  Reversing them risks a double-decrement if the SDK changes. */
    if (libBase->IMounter != NULL) {
        IExec->DropInterface((struct Interface *)libBase->IMounter);
        libBase->IMounter = NULL;
    }
    if (libBase->MounterBase != NULL) {
        IExec->CloseLibrary(libBase->MounterBase);
        libBase->MounterBase = NULL;
        DPRINTF(IExec, "[virtioscsi:virtio_mounter.c] mounter.library closed\n");
    }
}
