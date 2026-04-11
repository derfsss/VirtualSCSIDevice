#ifndef UNIT_DISCOVERY_H
#define UNIT_DISCOVERY_H

#include "virtioscsi.h"

/*
 * Scan SCSI targets 0-7, LUNs 0-7 via INQUIRY.
 * Allocates VirtIOUSCSIDevUnit structs and announces them to mounter.library.
 * Returns the number of units found (0 is not an error — hardware may be empty).
 */
uint32 DiscoverUnits(struct VirtIOSCSIBase *devBase);

#endif /* UNIT_DISCOVERY_H */
