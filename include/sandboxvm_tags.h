#ifndef SANDBOXVM_TAGS_H
#define SANDBOXVM_TAGS_H

/*
 * SandboxVM-private AllocVecTags tag. When this driver runs under
 * SandboxVM (via `sandboxvm -r virtioscsi.device`), the private IExec
 * sees this tag and routes the allocation through the host's real
 * allocator instead of guest vmem, so the returned buffer is in the
 * host address space and therefore valid for StartDMA / GetDMAList.
 * Without it, DMA-bound buffers come out of guest vmem (extmem >= 2GB)
 * which the kernel's IOMMU plumbing refuses to DMA-map -- the driver's
 * resident-init then returns NULL and the device never enumerates.
 *
 * On native AOS4 the tag value is above TAG_USER + small offsets
 * (0x80535600 range), so the utility.library tag walker treats it
 * as an unknown tag and silently ignores it. Same-source, dual-use.
 *
 * Defined here so this driver source has no build dependency on the
 * SandboxVM tree. Value must stay in sync with
 * SandboxVM/VM-OS4/include/sbvm_tags.h.
 */
#ifndef SBV_AVT_HostDMA
#define SBV_AVT_HostDMA        (0x80535601u)
#endif

#endif /* SANDBOXVM_TAGS_H */
