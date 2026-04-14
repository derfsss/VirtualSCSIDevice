#ifndef VIRTIO_EVENTS_H
#define VIRTIO_EVENTS_H

#include "virtioscsi.h"

/*
 * Phase 9: VirtIO SCSI event queue support (HOTPLUG + CHANGE).
 *
 * The event queue (VQ1) carries asynchronous device notifications:
 *   - VIRTIO_SCSI_T_TRANSPORT_RESET: target/LUN added or removed
 *   - VIRTIO_SCSI_T_PARAM_CHANGE:    device parameters changed (media, size)
 *   - VIRTIO_SCSI_T_ASYNC_NOTIFY:    SAM-4 asynchronous event notification
 *
 * Only activated when VIRTIO_SCSI_F_HOTPLUG or VIRTIO_SCSI_F_CHANGE was
 * accepted during feature negotiation (libBase->events_enabled == TRUE).
 */

/*
 * Allocate the event buffer pool, DMA-map it, post VIRTIO_SCSI_EVENT_BUFS
 * buffers on VQ1 as device-writable, start the event consumer task.
 * Returns TRUE on success.  Called once after InitVirtIOSCSI_Modern succeeds.
 */
BOOL InitEventQueue(struct VirtIOSCSIBase *libBase);

/*
 * Shut down the event task, EndDMA and free the event pool.
 * Called from _manager_Expunge.  Safe to call if InitEventQueue was never
 * called (events_enabled == FALSE) or failed partway.
 */
void ShutdownEventQueue(struct VirtIOSCSIBase *libBase);

#endif /* VIRTIO_EVENTS_H */
