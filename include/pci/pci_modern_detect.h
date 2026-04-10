#ifndef PCI_MODERN_DETECT_H
#define PCI_MODERN_DETECT_H

#include <exec/types.h>

struct VirtIOSCSIBase; /* forward declaration */

/*
 * DetectModernVirtIO: walk the PCI capability list for VirtIO vendor-specific
 * capabilities and populate libBase->modern_mode and *_cfg_base fields.
 * Returns TRUE if a modern device (VIRTIO_F_VERSION_1) is detected.
 */
BOOL DetectModernVirtIO(struct VirtIOSCSIBase *libBase);

#endif /* PCI_MODERN_DETECT_H */
