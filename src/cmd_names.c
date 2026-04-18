#include "cmd_names.h"
#include "virtioscsi_cmds.h"

/*
 * Single source of truth for supported commands.
 * Referenced by NSCMD_DEVICEQUERY and available for debug logging.
 */
const uint16 supported_commands[] = {
    /* Standard Exec commands */
    CMD_READ, CMD_WRITE, CMD_UPDATE, CMD_CLEAR, CMD_FLUSH, CMD_START, CMD_STOP,

    /* Trackdisk commands */
    TD_MOTOR, TD_SEEK, TD_FORMAT, TD_REMOVE, TD_CHANGENUM, TD_CHANGESTATE, TD_PROTSTATUS, TD_GETDRIVETYPE,
    TD_GETNUMTRACKS, TD_ADDCHANGEINT, TD_REMCHANGEINT, TD_GETGEOMETRY, TD_EJECT,

    /* Legacy 64-bit commands */
    TD_READ64, TD_WRITE64, TD_SEEK64, TD_FORMAT64,

    /* Extended trackdisk commands */
    ETD_READ, ETD_WRITE, ETD_MOTOR, ETD_SEEK, ETD_FORMAT, ETD_UPDATE, ETD_CLEAR,

    /* SCSI direct */
    HD_SCSICMD,

    /* NSD commands */
    NSCMD_DEVICEQUERY, NSCMD_TD_READ64, NSCMD_TD_WRITE64, NSCMD_TD_SEEK64, NSCMD_TD_FORMAT64, NSCMD_TD_GETGEOMETRY64,
    NSCMD_TD_CHANGEUNIT, NSCMD_TD_ADDSTATCALLBACK, NSCMD_TD_REMSTATCALLBACK,

    /* NSD ETD commands */
    NSCMD_ETD_READ64, NSCMD_ETD_WRITE64, NSCMD_ETD_SEEK64, NSCMD_ETD_FORMAT64,

    0 /* terminator */
};

const char *GetCommandName(uint32 cmd)
{
    switch (cmd) {
    case CMD_READ:            return "CMD_READ";
    case CMD_WRITE:           return "CMD_WRITE";
    case CMD_UPDATE:          return "CMD_UPDATE";
    case CMD_CLEAR:           return "CMD_CLEAR";
    case CMD_STOP:            return "CMD_STOP";
    case CMD_START:           return "CMD_START";
    case CMD_FLUSH:           return "CMD_FLUSH";
    case TD_MOTOR:            return "TD_MOTOR";
    case TD_SEEK:             return "TD_SEEK";
    case TD_FORMAT:           return "TD_FORMAT";
    case TD_REMOVE:           return "TD_REMOVE";
    case TD_CHANGENUM:        return "TD_CHANGENUM";
    case TD_CHANGESTATE:      return "TD_CHANGESTATE";
    case TD_PROTSTATUS:       return "TD_PROTSTATUS";
    case TD_GETDRIVETYPE:     return "TD_GETDRIVETYPE";
    case TD_GETNUMTRACKS:     return "TD_GETNUMTRACKS";
    case TD_ADDCHANGEINT:     return "TD_ADDCHANGEINT";
    case TD_REMCHANGEINT:     return "TD_REMCHANGEINT";
    case TD_GETGEOMETRY:      return "TD_GETGEOMETRY";
    case TD_EJECT:            return "TD_EJECT";
    case HD_SCSICMD:          return "HD_SCSICMD";
    case TD_READ64:           return "TD_READ64";
    case TD_WRITE64:          return "TD_WRITE64";
    case TD_SEEK64:           return "TD_SEEK64";
    case TD_FORMAT64:         return "TD_FORMAT64";
    case NSCMD_DEVICEQUERY:       return "NSCMD_DEVICEQUERY";
    case NSCMD_TD_READ64:         return "NSCMD_TD_READ64";
    case NSCMD_TD_WRITE64:        return "NSCMD_TD_WRITE64";
    case NSCMD_TD_SEEK64:         return "NSCMD_TD_SEEK64";
    case NSCMD_TD_FORMAT64:       return "NSCMD_TD_FORMAT64";
    case NSCMD_TD_GETGEOMETRY64:  return "NSCMD_TD_GETGEOMETRY64";
    case NSCMD_TD_CHANGEUNIT:     return "NSCMD_TD_CHANGEUNIT";
    case NSCMD_TD_ADDSTATCALLBACK: return "NSCMD_TD_ADDSTATCALLBACK";
    case NSCMD_TD_REMSTATCALLBACK: return "NSCMD_TD_REMSTATCALLBACK";
    case NSCMD_ETD_READ64:        return "NSCMD_ETD_READ64";
    case NSCMD_ETD_WRITE64:       return "NSCMD_ETD_WRITE64";
    case NSCMD_ETD_SEEK64:        return "NSCMD_ETD_SEEK64";
    case NSCMD_ETD_FORMAT64:      return "NSCMD_ETD_FORMAT64";
    case ETD_READ:                return "ETD_READ";
    case ETD_WRITE:               return "ETD_WRITE";
    case ETD_MOTOR:               return "ETD_MOTOR";
    case ETD_SEEK:                return "ETD_SEEK";
    case ETD_FORMAT:              return "ETD_FORMAT";
    case ETD_UPDATE:              return "ETD_UPDATE";
    case ETD_CLEAR:               return "ETD_CLEAR";
    default:                  return "UNKNOWN";
    }
}
