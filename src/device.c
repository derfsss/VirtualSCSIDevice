#include "version.h"
#include "virtioscsi.h"
#include <exec/exectags.h>
#include <exec/interfaces.h>
#include <exec/resident.h>

/* Implementation of the dummy Obtain/Release for the device interface */
uint32 _manager_Obtain(struct DeviceManagerInterface *Self)
{
    Self->Data.RefCount++;
    return Self->Data.RefCount;
}

uint32 _manager_Release(struct DeviceManagerInterface *Self)
{
    Self->Data.RefCount--;
    return Self->Data.RefCount;
}

/* Vector table for the AmigaOS 4 device interface */
static const APTR _manager_Vectors[] = {(APTR)_manager_Obtain,
                                        (APTR)_manager_Release,
                                        (APTR)NULL,
                                        (APTR)NULL,
                                        (APTR)_manager_Open,
                                        (APTR)_manager_Close,
                                        (APTR)_manager_Expunge,
                                        (APTR)NULL,
                                        (APTR)_manager_BeginIO,
                                        (APTR)_manager_AbortIO,
                                        (APTR)-1};

static const struct TagItem _manager_Tags[] = {
    {MIT_Name, (ULONG) "__device"}, {MIT_VectorTable, (ULONG)_manager_Vectors}, {MIT_Version, 1}, {TAG_END, 0}};

const APTR devInterfaces[] = {(APTR)_manager_Tags, (APTR)NULL};

static const char verstag[] __attribute__((used)) = "\0$VER: " DEVVERSIONSTRING;

extern struct Library *_manager_Init(struct Library *library, BPTR seglist, struct Interface *exec);

static const struct TagItem dev_init_tags[] = {{CLT_DataSize, sizeof(struct VirtIOSCSIBase)},
                                               {CLT_Interfaces, (ULONG)devInterfaces},
                                               {CLT_InitFunc, (ULONG)_manager_Init},
                                               {CLT_NoLegacyIFace, TRUE},
                                               {TAG_END, 0}};

static const struct Resident dev_res __attribute__((used)) = {RTC_MATCHWORD,
                                                              (struct Resident *)&dev_res,
                                                              (struct Resident *)(&dev_res + 1),
                                                              RTF_NATIVE | RTF_COLDSTART | RTF_AUTOINIT,
                                                              DEVVER,
                                                              NT_DEVICE,
                                                              0,
                                                              DEVNAME,
                                                              DEVVERSIONSTRING,
                                                              (APTR)dev_init_tags};

int _start(char *argstring, int arglen, struct ExecBase *sysbase)
{
    /*
     * virtioscsi.device is a Kickstart-resident device driver.  It is loaded
     * by the system at boot via its Resident structure and receives control
     * through _manager_Init(), not a shell entry point.  If a user runs it
     * from a shell, print a short diagnostic via DebugPrintF (no dos.library
     * opening required) and return failure so the shell reports it properly.
     */
    (void)argstring;
    (void)arglen;

    struct ExecIFace *IExec = (struct ExecIFace *)sysbase->MainInterface;
    IExec->DebugPrintF("%s cannot be executed from a shell — install in SYS:Kickstart/ "
                       "(with MODULE and diskboot.config entries) and reboot.\n",
                       DEVNAME);
    return 20; /* RETURN_FAIL */
}
