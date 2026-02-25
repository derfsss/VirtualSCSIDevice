#include "virtio/virtio_irq.h"
#include "virtio/virtio_scsi.h"
#include "virtioscsi.h"

/*
 * VirtIO PCI interrupt handler.
 *
 * Called in interrupt context on the shared PCI INTx line.
 * Must be fast: no memory allocation, no blocking calls, no DebugPrintF.
 *
 * V50+ calling convention:
 *   uint32 handler(struct ExceptionContext *ctx,
 *                  struct ExecBase *SysBase,
 *                  APTR is_Data)
 *
 * Returns non-zero if we claimed the interrupt, 0 otherwise.
 */
static uint32 VirtIOSCSI_InterruptHandler(struct ExceptionContext *ctx, struct ExecBase *SysBase, APTR is_Data)
{
    struct VirtIOSCSIBase *base = (struct VirtIOSCSIBase *)is_Data;
    struct PCIDevice *pciDev = base->pciDevice;

    (void)ctx;

    /*
     * Read the ISR register. This simultaneously:
     *   1. Tells us if this interrupt is from our device (non-zero)
     *   2. Acknowledges the interrupt to the device (clears ISR bits)
     *
     * If ISR == 0, this interrupt is not ours — return 0 to let the
     * next handler in the shared chain process it.
     *
     * In modern mode, the ISR register lives in the ISR_CFG capability region
     * (a dedicated MMIO address, accessed via isr_cfg_base).  In legacy mode
     * it is at BAR0 + VIRTIO_PCI_ISR.
     *
     * No DebugPrintF in interrupt context.
     */
    uint8 isr;
    if (base->modern_mode) {
        isr = pciDev->InByte(base->isr_cfg_base);
    } else {
        uint32 iobase = (uint32)base->bar0->Physical;
        isr = pciDev->InByte(iobase + VIRTIO_PCI_ISR);
    }

    if (isr == 0)
        return 0; /* Not our interrupt */

    /*
     * Bit 0: virtqueue update (I/O completion)
     * Bit 1: device configuration change (ignored for now)
     *
     * Scan all units. For each unit with a waiting task, signal it.
     * The per-unit io_cookie is compared against the VirtIO "cookie"
     * (which is req_cmd pointer from AddBuf). If a unit is waiting, we
     * signal it to wake its task — that task then calls GetBuf() to
     * confirm and retrieve the response.
     *
     * Note: we signal unconditionally when io_wait_task is set and
     * io_cookie is non-NULL. The sleeping task re-checks with GetBuf().
     */
    if (isr & 1) {
        struct ExecIFace *IExec = base->IExec;
        for (int i = 0; i < 8; i++) {
            struct VirtIOUSCSIDevUnit *unit = base->units[i];
            if (unit && unit->io_wait_task) {
                IExec->Signal(unit->io_wait_task, unit->io_signal_mask);
            }
        }
    }

    return 1; /* Interrupt claimed */
}

BOOL InstallVirtIOInterrupt(struct VirtIOSCSIBase *base)
{
    struct ExecIFace *IExec = base->IExec;

    /* Get the mapped interrupt vector for our PCI device */
    base->irq_number = base->pciDevice->MapInterrupt();

    DPRINTF(IExec, "[virtioscsi:virtio_irq.c] MapInterrupt returned vector %lu\n", base->irq_number);

    if (base->irq_number == 0) {
        DPRINTF(IExec, "[virtioscsi:virtio_irq.c] MapInterrupt failed (returned 0)\n");
        return FALSE;
    }

    /* Set up the Interrupt structure */
    base->irq_handler.is_Node.ln_Type = NT_INTERRUPT;
    base->irq_handler.is_Node.ln_Pri = 0;
    base->irq_handler.is_Node.ln_Name = DEVNAME;
    base->irq_handler.is_Data = (APTR)base;
    base->irq_handler.is_Code = (VOID (*)())VirtIOSCSI_InterruptHandler;

    /* Install on the shared interrupt chain */
    BOOL ok = IExec->AddIntServer(base->irq_number, &base->irq_handler);

    if (!ok) {
        DPRINTF(IExec, "[virtioscsi:virtio_irq.c] AddIntServer failed for vector %lu\n", base->irq_number);
        return FALSE;
    }

    base->irq_installed = TRUE;

    DPRINTF(IExec, "[virtioscsi:virtio_irq.c] Interrupt handler installed on vector %lu\n", base->irq_number);

    return TRUE;
}

void RemoveVirtIOInterrupt(struct VirtIOSCSIBase *base)
{
    struct ExecIFace *IExec = base->IExec;

    if (base->irq_installed) {
        IExec->RemIntServer(base->irq_number, &base->irq_handler);
        base->irq_installed = FALSE;

        DPRINTF(IExec, "[virtioscsi:virtio_irq.c] Interrupt handler removed from vector %lu\n", base->irq_number);
    }
}
