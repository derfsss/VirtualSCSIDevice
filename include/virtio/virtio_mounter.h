#ifndef VIRTIO_MOUNTER_H
#define VIRTIO_MOUNTER_H

#include "virtioscsi.h"

/*
 * Phase 10: mounter.library integration for hot-added VirtIO SCSI units.
 *
 * The driver opens mounter.library lazily on first hot-plug because boot-time
 * disks are already announced by diskboot.kmod (which reads diskboot.config),
 * and calling mounter at boot crashes MediaToolbox under resident priority 0.
 * After boot, mounter.library is initialised and safe to call.
 *
 * All entry points below are callable from the event task and are no-ops
 * (returning success) when mounter.library is unavailable.  Failure to open
 * the library is non-fatal — the unit remains visible to OpenDevice() even
 * without auto-mount.  Lifecycle bookkeeping is per-unit: Announce sets
 * unit->announced; Denounce clears it; Cleanup walks units[] and denounces
 * any unit that is still announced before closing the library handle.
 */

/*
 * Lazily open mounter.library v53+ and obtain its main interface.
 *
 * Idempotent: subsequent calls after a successful first call return TRUE
 * without re-opening.  After a previously failed open, future calls retry
 * (the library may have been loaded by another process in the meantime).
 *
 * Returns TRUE if mounter is available, FALSE otherwise.  On FALSE, both
 * libBase->MounterBase and libBase->IMounter are guaranteed NULL.
 *
 * Thread-safety: not internally synchronised.  In practice only the event
 * task calls this — Phase 10 mounter operations run exclusively in event
 * task context, so no locking is required.
 */
BOOL EnsureMounter(struct VirtIOSCSIBase *libBase);

/*
 * Announce a unit to mounter.library if it is freshly hot-added.
 *
 * Skips the call if:
 *   - libBase->MounterBase is NULL after EnsureMounter() (mounter unavailable)
 *   - unit == NULL (defensive)
 *   - unit->announced is already TRUE (idempotency guard against re-entry)
 *
 * On success, sets unit->announced = TRUE so DenounceIfAnnounced() can find
 * it later.  AnnounceDeviceTags() is non-blocking; mounter performs the
 * actual partition scan in its own task.
 *
 * Returns TRUE if the announcement succeeded (or was skipped because already
 * announced).  Returns FALSE if mounter is unavailable or refused the call.
 */
BOOL AnnounceIfHotAdded(struct VirtIOSCSIBase *libBase,
                        struct VirtIOUSCSIDevUnit *unit);

/*
 * Denounce a unit from mounter.library if previously announced.
 *
 * Safe to call on units that were never announced (boot-time discovery
 * units, units that hit AnnounceIfHotAdded but the library was unavailable).
 * Idempotent: subsequent calls are no-ops.
 *
 * Clears unit->announced regardless of whether mounter is currently open —
 * if mounter went away we have no way to denounce, but the local flag
 * should still reflect "no longer ours".
 */
void DenounceIfAnnounced(struct VirtIOSCSIBase *libBase,
                         struct VirtIOUSCSIDevUnit *unit);

/*
 * Tear down mounter integration: denounce every unit still marked announced,
 * drop the IMounter interface, close the library.  Safe to call multiple
 * times; safe to call when mounter was never opened.
 *
 * Must be called from a context where mounter operations are quiescent —
 * i.e., AFTER ShutdownEventQueue() in _manager_Expunge.
 */
void CleanupMounter(struct VirtIOSCSIBase *libBase);

#endif /* VIRTIO_MOUNTER_H */
