#ifndef VIRTIO_SCSI_CMDS_H
#define VIRTIO_SCSI_CMDS_H

#include "virtioscsi.h"
#include <devices/newstyle.h>
#include <devices/scsidisk.h>
/* SDK quirk: struct DriveGeometry64 (used by NSCMD_TD_GETGEOMETRY64) lives
 * in libraries/mounter.h, not in any device header. */
#include <libraries/mounter.h>

#ifndef NSCMD_TD_GETGEOMETRY64
#define NSCMD_TD_GETGEOMETRY64 0xA004
#endif

#ifndef NSCMD_TD_CHANGEUNIT
#define NSCMD_TD_CHANGEUNIT 0xA005
#endif

#ifndef NSCMD_TD_ADDSTATCALLBACK
#define NSCMD_TD_ADDSTATCALLBACK 0xA006
#endif

#ifndef NSCMD_TD_REMSTATCALLBACK
#define NSCMD_TD_REMSTATCALLBACK 0xA007
#endif

/* NSCMD_ETD_* may not be in all SDK versions */
#ifndef NSCMD_ETD_READ64
#define NSCMD_ETD_READ64 0xE000
#define NSCMD_ETD_WRITE64 0xE001
#define NSCMD_ETD_SEEK64 0xE002
#define NSCMD_ETD_FORMAT64 0xE003
#endif

/* Parsers */
void Parse_SCSI_Command(struct VirtIOSCSIBase *libBase, struct IOStdReq *req);
void Parse_NS_Command(struct VirtIOSCSIBase *libBase, struct IOStdReq *req);

#define SCSI_LOG_SENSE 0x4D

/* SCSI Handlers */
void Handle_SCSI_Inquiry(struct VirtIOSCSIBase *libBase, struct IOStdReq *req, struct SCSICmd *scsiCmd);
void Handle_SCSI_ReadCapacity10(struct VirtIOSCSIBase *libBase, struct IOStdReq *req, struct SCSICmd *scsiCmd);
void Handle_SCSI_Read10(struct VirtIOSCSIBase *libBase, struct IOStdReq *req, struct SCSICmd *scsiCmd);
void Handle_SCSI_Write10(struct VirtIOSCSIBase *libBase, struct IOStdReq *req, struct SCSICmd *scsiCmd);
void Handle_SCSI_TestUnitReady(struct VirtIOSCSIBase *libBase, struct IOStdReq *req, struct SCSICmd *scsiCmd);
void Handle_SCSI_LogSense(struct VirtIOSCSIBase *libBase, struct IOStdReq *req, struct SCSICmd *scsiCmd);
void Handle_SCSI_ATAPassthrough(struct VirtIOSCSIBase *libBase, struct IOStdReq *req, struct SCSICmd *scsiCmd);

/* NS Handlers */
void Handle_NS_DeviceQuery(struct VirtIOSCSIBase *libBase, struct IOStdReq *req);
void Handle_NS_TD_GetGeometry64(struct VirtIOSCSIBase *libBase, struct IOStdReq *req);
void Handle_NS_TD_IO64(struct VirtIOSCSIBase *libBase, struct IOStdReq *req);

/* Exec Handlers.
 * Block I/O (CMD_READ/WRITE, TD_*64, NSCMD_TD_*64) has no Handle_* entry:
 * it is submitted through the inflight pipeline by unit_task.c's
 * submit_block_io(). */
void Handle_CMD_Success(struct VirtIOSCSIBase *libBase, struct IOStdReq *req);
void Handle_CMD_Update(struct VirtIOSCSIBase *libBase, struct IOStdReq *req);
void Handle_TD_GetGeometry(struct VirtIOSCSIBase *libBase, struct IOStdReq *req);
void Handle_TD_GetNumTracks(struct VirtIOSCSIBase *libBase, struct IOStdReq *req);

#endif /* VIRTIO_SCSI_CMDS_H */
