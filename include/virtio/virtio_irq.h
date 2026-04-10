#ifndef VIRTIO_IRQ_H
#define VIRTIO_IRQ_H

#include <exec/types.h>

struct VirtIOSCSIBase; /* forward declaration */

/*
 * Install the PCI interrupt handler for VirtIO queue completion.
 * Must be called after VirtIO init and PCI discovery (bar0 + pciDevice valid).
 * Returns TRUE on success, FALSE on failure.
 */
BOOL InstallVirtIOInterrupt(struct VirtIOSCSIBase *base);

/*
 * Remove the PCI interrupt handler.
 * Must be called before freeing PCI resources in Expunge.
 */
void RemoveVirtIOInterrupt(struct VirtIOSCSIBase *base);

#endif /* VIRTIO_IRQ_H */
