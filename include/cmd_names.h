#ifndef CMD_NAMES_H
#define CMD_NAMES_H

#include <exec/types.h>

/* Returns a human-readable name for a device command number (for debug logging) */
const char *GetCommandName(uint32 cmd);

/* Terminated by 0. Shared between NSCMD_DEVICEQUERY and BeginIO. */
extern const uint16 supported_commands[];

#endif /* CMD_NAMES_H */
