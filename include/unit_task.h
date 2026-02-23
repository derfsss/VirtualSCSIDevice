#ifndef UNIT_TASK_H
#define UNIT_TASK_H

#include <exec/types.h>

struct VirtIOSCSIBase;
struct VirtIOUSCSIDevUnit;

/*
 * Message used to synchronise UnitTask_Start() with the new task's entry.
 * The spawned task signals parent_task with ready_mask when its port is ready.
 * The same signal is reused for shutdown confirmation.
 */
struct UnitTaskStartMsg
{
    struct VirtIOSCSIBase    *libBase;
    struct VirtIOUSCSIDevUnit *unit;
    struct Task              *parent_task;
    uint32                    ready_mask;  /* 1 << ready_bit */
    int8                      ready_bit;
};

/*
 * Start a device task for the given unit.
 * Called from _manager_Open() on the first open of a unit.
 * Returns TRUE on success.
 */
BOOL UnitTask_Start(struct VirtIOSCSIBase *libBase, struct VirtIOUSCSIDevUnit *unit);

/*
 * Shut down the device task for the given unit.
 * Blocks until the task has fully exited.
 * Called from _manager_Close() (last close) and Expunge().
 */
void UnitTask_Shutdown(struct VirtIOSCSIBase *libBase, struct VirtIOUSCSIDevUnit *unit);

/*
 * Entry point for the unit device task (called by exec, not directly).
 */
void UnitTask_Entry(void);

#endif /* UNIT_TASK_H */
