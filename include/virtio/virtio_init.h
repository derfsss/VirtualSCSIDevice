#ifndef VIRTIO_INIT_H
#define VIRTIO_INIT_H

#include "virtioscsi.h"

BOOL InitVirtIOSCSI(struct VirtIOSCSIBase *libBase);
void CleanupVirtIOSCSI(struct VirtIOSCSIBase *libBase);

#endif /* VIRTIO_INIT_H */
