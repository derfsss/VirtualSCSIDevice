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

/*
 * 68k-compatible jump table.  Without CLT_Vector68K the kernel only
 * generates the default Open/Close/Expunge/Reserved vectors (per
 * MakeFunctions autodoc: "If no pointer or NULL is given, no 68k
 * compatible jump table is generated, except for the default vectors
 * Open, Close, Expunged, and the Reserved vector").  BeginIO at offset
 * -30 and AbortIO at offset -36 end up *absent* -- when a legacy handler
 * like SFS 1.290 calls `jsr -30(a6)` to issue BeginIO, it jumps into
 * unmapped memory and the handler task dies with AN_BadFreeAddr
 * (0x0100000F) from kernel exception cleanup.
 *
 * Order matters: entries map to offsets -6, -12, -18, -24, -30, -36
 * (i.e. first entry is Open, last user entry is AbortIO).  Terminated
 * by -1 per MakeFunctions autodoc.
 */
static const APTR _manager_Vectors68K[] = {
    (APTR)_manager_Open,     /* offset -6  */
    (APTR)_manager_Close,    /* offset -12 */
    (APTR)_manager_Expunge,  /* offset -18 */
    (APTR)NULL,              /* offset -24 (Reserved / ExtFunc) */
    (APTR)_manager_BeginIO,  /* offset -30 */
    (APTR)_manager_AbortIO,  /* offset -36 */
    (APTR)-1,                /* terminator */
};

static const char verstag[] __attribute__((used)) = "\0$VER: " DEVVERSIONSTRING;

extern struct Library *_manager_Init(struct Library *library, BPTR seglist, struct Interface *exec);

/* Non-const to match all working OS4 IDE/SATA device drivers -- see comment on dev_res. */
static struct TagItem dev_init_tags[] = {{CLT_DataSize, sizeof(struct VirtIOSCSIBase)},
                                               {CLT_Interfaces, (ULONG)devInterfaces},
                                               {CLT_InitFunc, (ULONG)_manager_Init},
                                               /* Explicit 68k jump table so SFS 1.290 (and any
                                                * other pre-OS4 handler) can reach BeginIO at
                                                * offset -30 and AbortIO at -36.  Without this
                                                * SFS's `jsr -30(a6)` lands in unmapped memory
                                                * and the handler task dies with AN_BadFreeAddr
                                                * (0x0100000F) from the exception-cleanup path.  */
                                               {CLT_Vector68K, (ULONG)_manager_Vectors68K},
                                               {CLT_NoLegacyIFace, FALSE},
                                               {TAG_END, 0}};

/*
 * NOT `const`!  Every working OS4 disk device kmod (a1ide, peg2ide, sii*ide,
 * it8212ide ...) places its Resident struct in writable .data.  Putting it
 * in .rodata triggers subtle silent breakage when the kernel/DOS tries to
 * patch fields during boot binding.  This is the ONE structural difference
 * between our binary and every shipping IDE driver in the kickstart.
 */
static struct Resident dev_res __attribute__((used)) = {RTC_MATCHWORD,
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
    IExec->DebugPrintF("%s cannot be executed from a shell -- install in SYS:Kickstart/ "
                       "(with MODULE and diskboot.config entries) and reboot.\n",
                       DEVNAME);
    return 20; /* RETURN_FAIL */
}
