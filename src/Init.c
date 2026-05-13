#include "pci/pci_discovery.h"
#include "unit_discovery.h"
#include "virtio/virtio_events.h"
#include "virtio/virtio_init.h"
#include "virtio/virtio_irq.h"
#include "virtioscsi.h"
#include <exec/exec.h>

extern const APTR devInterfaces[];

#include "version.h"

struct Library *_manager_Init(struct Library *library, BPTR seglist, struct Interface *exec)
{
    struct VirtIOSCSIBase *devBase = (struct VirtIOSCSIBase *)library;
    struct ExecIFace *iexec = (struct ExecIFace *)exec;

    iexec->DebugPrintF("[virtioscsi] Loading Version: %s\n", DEVVERSIONSTRING_FULL);

    devBase->IExec = iexec;
    devBase->dev_SegList = seglist;

    iexec->InitSemaphore(&devBase->io_lock);

    for (int i = 0; i < 8; i++) {
        devBase->units[i] = NULL;
    }

    /* Open expansion.library and get PCI interface.
     * v53 minimum: covers the 53.54 install CD through FE Update 2.  FE U3
     * bumps the kernel-embedded copy to 54.1, but the PCIIFace methods used
     * here (FindDeviceTags, GetResourceRange, ReadConfig*, WriteConfig*,
     * FreeDevice) have been stable since well before 53.1, so v53 is enough. */
    devBase->ExpansionBase = iexec->OpenLibrary("expansion.library", 53);
    if (!devBase->ExpansionBase) {
        DPRINTF(iexec, "[virtioscsi:Init.c] Init: Failed to open expansion.library v53.\n");
        return NULL;
    }
    devBase->IPCI = (struct PCIIFace *)iexec->GetInterface(devBase->ExpansionBase, "pci", 1, NULL);
    if (!devBase->IPCI) {
        DPRINTF(iexec, "[virtioscsi:Init.c] Init: Failed to get IPCI interface.\n");
        iexec->CloseLibrary(devBase->ExpansionBase);
        return NULL;
    }

    /* Discover VirtIO SCSI PCI device */
    if (!DiscoverVirtIOSCSI(devBase)) {
        iexec->DropInterface((struct Interface *)devBase->IPCI);
        iexec->CloseLibrary(devBase->ExpansionBase);
        return NULL;
    }

    /* Open utility.library */
    devBase->UtilityBase = iexec->OpenLibrary("utility.library", 50);
    if (!devBase->UtilityBase) {
        DPRINTF(iexec, "[virtioscsi:Init.c] Init: Failed to open utility.library v50.\n");
        iexec->DropInterface((struct Interface *)devBase->IPCI);
        iexec->CloseLibrary(devBase->ExpansionBase);
        return NULL;
    }
    devBase->IUtility = (struct UtilityIFace *)iexec->GetInterface(devBase->UtilityBase, "main", 1, NULL);
    if (!devBase->IUtility) {
        DPRINTF(iexec, "[virtioscsi:Init.c] Init: Failed to get IUtility interface.\n");
        iexec->CloseLibrary(devBase->UtilityBase);
        iexec->DropInterface((struct Interface *)devBase->IPCI);
        iexec->CloseLibrary(devBase->ExpansionBase);
        return NULL;
    }

    /* Initialize VirtIO queues */
    if (!InitVirtIOSCSI(devBase)) {
        CleanupVirtIOSCSI(devBase);
        iexec->DropInterface((struct Interface *)devBase->IUtility);
        iexec->CloseLibrary(devBase->UtilityBase);
        iexec->DropInterface((struct Interface *)devBase->IPCI);
        iexec->CloseLibrary(devBase->ExpansionBase);
        return NULL;
    }

    /* Install PCI interrupt handler (non-fatal — falls back to polling on failure) */
    if (!InstallVirtIOInterrupt(devBase)) {
        DPRINTF(iexec, "[virtioscsi:Init.c] Init: Interrupt install failed, using polling fallback.\n");
    }

    /*
     * Make device public. Under RTF_AUTOINIT | RTF_NATIVE, the kernel
     * handles registration. Manual AddDevice would cause double registration.
     */
    devBase->dev_Base.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    devBase->dev_Base.dd_Library.lib_Node.ln_Pri = 0;
    devBase->dev_Base.dd_Library.lib_Node.ln_Name = DEVNAME;
    devBase->dev_Base.dd_Library.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    devBase->dev_Base.dd_Library.lib_Version = DEVVER;
    devBase->dev_Base.dd_Library.lib_Revision = DEVREV;
    devBase->dev_Base.dd_Library.lib_IdString = DEVVERSIONSTRING;

    /* Scan SCSI targets and announce to mounter.library */
    DiscoverUnits(devBase);

    /* DEBUG: temporarily disabled to isolate the SFS 1.290 mount-hang
     * regression.  If SFS no longer crashes with the event queue off,
     * the bug is in virtio_events.c's task / shared-state setup. */
    /* InitEventQueue(devBase); */
    DPRINTF(iexec, "[virtioscsi:Init.c] *** EVENT QUEUE DISABLED FOR SFS DEBUG ***\n");

    DPRINTF(iexec, "[virtioscsi:Init.c] Init: Device library initialized and public.\n");

    /* Struct-layout diagnostic: print the sizes and offsets so we can
     * correlate raw hex dumps with the SDK's struct Unit / MsgPort layout.
     * Required because the post-DOS hang debugging needs us to know the
     * exact byte positions of unit_flags and unit_OpenCnt. */
    DPRINTF(iexec,
            "[virtioscsi:Init.c] layout: sizeof(Library)=%u Node=%u MsgPort=%u Unit=%u DevUnit=%u\n",
            (uint32)sizeof(struct Library),
            (uint32)sizeof(struct Node),
            (uint32)sizeof(struct MsgPort),
            (uint32)sizeof(struct Unit),
            (uint32)sizeof(struct VirtIOUSCSIDevUnit));
    DPRINTF(iexec,
            "  Unit offsets: MsgPort=%u flags=%u pad=%u OpenCnt=%u (first byte of ln_Type at off 8 of Unit)\n",
            (uint32)__builtin_offsetof(struct Unit, unit_MsgPort),
            (uint32)__builtin_offsetof(struct Unit, unit_flags),
            (uint32)__builtin_offsetof(struct Unit, unit_pad),
            (uint32)__builtin_offsetof(struct Unit, unit_OpenCnt));
    DPRINTF(iexec,
            "  MsgPort offsets: mp_Flags=%u mp_SigBit=%u mp_SigTask=%u mp_MsgList=%u\n",
            (uint32)__builtin_offsetof(struct MsgPort, mp_Flags),
            (uint32)__builtin_offsetof(struct MsgPort, mp_SigBit),
            (uint32)__builtin_offsetof(struct MsgPort, mp_SigTask),
            (uint32)__builtin_offsetof(struct MsgPort, mp_MsgList));

    /* Dump the library's negative-offset jump table so we can tell whether
     * the kernel generated legacy vectors or left it zero-filled.  Classic
     * filesystem handlers call device methods via jsr -6/-12/-18/-24/-30/-36
     * from libBase; a zero-filled neg area means jsr -30(a6) lands in NULL. */
    {
        uint8 *neg = (uint8 *)devBase - 48; /* 48 bytes below libBase */
        DPRINTF(iexec,
                "  neg-table (libBase-48 .. libBase-1, 48 bytes):\n"
                "    %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n"
                "    %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n"
                "    %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
                neg[ 0], neg[ 1], neg[ 2], neg[ 3], neg[ 4], neg[ 5], neg[ 6], neg[ 7],
                neg[ 8], neg[ 9], neg[10], neg[11], neg[12], neg[13], neg[14], neg[15],
                neg[16], neg[17], neg[18], neg[19], neg[20], neg[21], neg[22], neg[23],
                neg[24], neg[25], neg[26], neg[27], neg[28], neg[29], neg[30], neg[31],
                neg[32], neg[33], neg[34], neg[35], neg[36], neg[37], neg[38], neg[39],
                neg[40], neg[41], neg[42], neg[43], neg[44], neg[45], neg[46], neg[47]);
    }

    return (struct Library *)devBase;
}
