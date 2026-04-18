#ifndef UNIT_DISCOVERY_H
#define UNIT_DISCOVERY_H

#include "virtioscsi.h"

/*
 * Scan SCSI targets 0-7, LUNs 0-7 via INQUIRY.
 * Allocates VirtIOUSCSIDevUnit structs and announces them to mounter.library.
 * Returns the number of units found (0 is not an error — hardware may be empty).
 */
uint32 DiscoverUnits(struct VirtIOSCSIBase *devBase);

/*
 * Initialize the embedded struct Unit inside a VirtIOUSCSIDevUnit to a sane,
 * valid state.  Must be called after allocating a unit and setting unit_num,
 * target_id, lun_id, and media_present.  Used by both boot-time discovery
 * (DiscoverUnits) and hot-add (probe_and_add).
 */
void init_dev_unit(struct ExecIFace *IExec, struct VirtIOUSCSIDevUnit *unit);

#endif /* UNIT_DISCOVERY_H */
