/*
 * test_modern.c — VirtIO Modern Device Feature Detection on Pegasos2/MV64361
 *
 * Tests VirtIO 1.0 Modern PCI device detection and feature negotiation.
 * Supports: SCSI, Block, Network, GPU, 9P FileSystem, and IOMMU devices.
 * 
 * Designed specifically for Pegasos2 hardware (Marvell MV64361 transparent bridge)
 * which supports direct MMIO access via stwbrx/lwbrx assembly instructions.
 *
 * This program demonstrates proper VirtIO feature detection and decoding for
 * multiple device types, showing which device features are advertised in human-readable form.
 *
 * Output goes to the serial debug console via IExec->DebugPrintF().
 *
 * Build:  see Makefile target 'build/test_modern'
 * Run:    test_modern  (from AmigaOS shell on Pegasos2)
 */

#include <proto/exec.h>
#include <proto/expansion.h>
#include <expansion/pci.h>
#include <exec/types.h>
#include <exec/memory.h>
#include "virtio/virtio_pci_modern.h"

/* VirtIO status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE  0x01
#define VIRTIO_STATUS_DRIVER       0x02
#define VIRTIO_STATUS_FEATURES_OK  0x08
#define VIRTIO_STATUS_DRIVER_OK    0x04

/* PCI Command register bits */
#define PCI_COMMAND_IO      0x0001   /* I/O space enable */
#define PCI_COMMAND_MEMORY  0x0002   /* Memory space enable */
#define PCI_COMMAND_MASTER  0x0004   /* Bus master enable */

/* VirtIO Device Types - VALUES MUST MATCH VirtIO 1.2 SPEC DEVICE TYPE NUMBERS
 * PCI Device ID = 0x1040 + device_type_number
 * Reference: VirtIO 1.2 specification, section Device Types
 * For example: SCSI is device type 8, so PCI Device ID = 0x1048
 */
#define VIRTIO_DEV_NET              1   /* Network card (0x1041) */
#define VIRTIO_DEV_BLOCK            2   /* Block device (0x1042) */
#define VIRTIO_DEV_CONSOLE          3   /* Console (0x1043) */
#define VIRTIO_DEV_RNG              4   /* Entropy source (0x1044) */
#define VIRTIO_DEV_BALLOON_LEGACY   5   /* Memory ballooning traditional (0x1045) */
#define VIRTIO_DEV_IOMEMORY         6   /* IO Memory (0x1046) */
#define VIRTIO_DEV_RPMSG            7   /* RPC Message (0x1047) */
#define VIRTIO_DEV_SCSI             8   /* SCSI Host Adapter (0x1048) */
#define VIRTIO_DEV_9P               9   /* 9P Transport (0x1049) */
#define VIRTIO_DEV_WLAN            10   /* MAC80211 WLAN (0x104A) */
#define VIRTIO_DEV_RPROC_SERIAL    11   /* RProc Serial (0x104B) */
#define VIRTIO_DEV_CAIF            12   /* Virtio CAIF (0x104C) */
#define VIRTIO_DEV_BALLOON         13   /* Memory Balloon (0x104D) */
#define VIRTIO_DEV_GPU             16   /* GPU Device (0x1050) */
#define VIRTIO_DEV_TIMER           17   /* Timer/Clock Device (0x1051) */
#define VIRTIO_DEV_INPUT           18   /* Input Device (0x1052) */
#define VIRTIO_DEV_SOCKET          19   /* Socket Device (0x1053) */
#define VIRTIO_DEV_CRYPTO          20   /* Crypto Device (0x1054) */
#define VIRTIO_DEV_SIGNAL_DIST     21   /* Signal Distribution Module (0x1055) */
#define VIRTIO_DEV_PSTORE          22   /* PStore Device (0x1056) */
#define VIRTIO_DEV_IOMMU           23   /* IOMMU Device (0x1057) */
#define VIRTIO_DEV_MEMORY          24   /* Memory Device (0x1058) */
#define VIRTIO_DEV_AUDIO           25   /* Audio Device (0x1059) */
#define VIRTIO_DEV_FILESYSTEM      26   /* File System Device (0x105A) */
#define VIRTIO_DEV_PMEM            27   /* PMEM Device (0x105B) */
#define VIRTIO_DEV_RPMB            28   /* RPMB Device (0x105C) */
#define VIRTIO_DEV_HWSIM           29   /* MAC80211 HWSIM (0x105D) */
#define VIRTIO_DEV_VIDEO_ENC       30   /* Video Encoder Device (0x105E) */
#define VIRTIO_DEV_VIDEO_DEC       31   /* Video Decoder Device (0x105F) */
#define VIRTIO_DEV_SCMI            32   /* SCMI Device (0x1060) */
#define VIRTIO_DEV_NITRO_MODULE    33   /* Nitro Secure Module (0x1061) */
#define VIRTIO_DEV_I2C             34   /* I2C Adapter (0x1062) */
#define VIRTIO_DEV_WATCHDOG        35   /* Watchdog (0x1063) */
#define VIRTIO_DEV_CAN             36   /* CAN Device (0x1064) */
#define VIRTIO_DEV_PARAM_SERVER    38   /* Parameter Server (0x1066) */
#define VIRTIO_DEV_AUDIO_POLICY    39   /* Audio Policy Device (0x1067) */
#define VIRTIO_DEV_BLUETOOTH       40   /* Bluetooth Device (0x1068) */
#define VIRTIO_DEV_GPIO            41   /* GPIO Device (0x1069) */
#define VIRTIO_DEV_RDMA            42   /* RDMA Device (0x106A) */

/* VirtIO common feature bits (shared across all device types) */
#define VIRTIO_F_INDIRECT_DESC      28  /* Support for indirect descriptor tables */
#define VIRTIO_F_EVENT_IDX          29  /* Used/avail ring event index support */
#define VIRTIO_F_VERSION_1          32  /* VirtIO 1.0 compliance */
#define VIRTIO_F_ACCESS_PLATFORM    33  /* Device uses platform-translated addresses (IOMMU) */
#define VIRTIO_F_RING_PACKED        34  /* Support for packed virtqueue layout */
#define VIRTIO_F_IN_ORDER           35  /* Buffers used in the order made available */
#define VIRTIO_F_ORDER_PLATFORM     36  /* Platform-ordered memory access */
#define VIRTIO_F_SR_IOV             37  /* Single Root I/O Virtualization support */
#define VIRTIO_F_NOTIFICATION_DATA  38  /* Driver notifications include extra data */
#define VIRTIO_F_NOTIF_CONFIG_DATA  39  /* Use queue_notify_data for notifications */
#define VIRTIO_F_RING_RESET         40  /* Support for individual queue reset */

/* VirtIO SCSI device feature bits (from VirtIO SCSI spec section 5.6.3) */
#define VIRTIO_SCSI_F_INOUT         0   /* Single request can have read+write buffers */
#define VIRTIO_SCSI_F_HOTPLUG       1   /* Hot-plug/hot-unplug event reporting */
#define VIRTIO_SCSI_F_CHANGE        2   /* PARAM_CHANGE event reporting */
#define VIRTIO_SCSI_F_T10_PI        3   /* T10 DIF/DIX protection information support */

/* VirtIO Block device feature bits (VirtIO 1.2 spec, section 5.2.3) */
#define VIRTIO_BLK_F_BARRIER        0   /* Legacy: request barriers (legacy interface only) */
#define VIRTIO_BLK_F_SIZE_MAX       1   /* Maximum segment size in size_max */
#define VIRTIO_BLK_F_SEG_MAX        2   /* Maximum number of segments in seg_max */
#define VIRTIO_BLK_F_GEOMETRY       4   /* Disk-style geometry in geometry struct */
#define VIRTIO_BLK_F_RO             5   /* Device is read-only */
#define VIRTIO_BLK_F_BLK_SIZE       6   /* Block size in blk_size */
#define VIRTIO_BLK_F_SCSI           7   /* Legacy: SCSI packet commands (legacy interface only) */
#define VIRTIO_BLK_F_FLUSH          9   /* Cache flush command support */
#define VIRTIO_BLK_F_TOPOLOGY      10   /* Optimal I/O alignment info in topology struct */
#define VIRTIO_BLK_F_CONFIG_WCE    11   /* Cache writeback/writethrough toggle via writeback field */
#define VIRTIO_BLK_F_MQ            12   /* Multiqueue: num_queues field valid */
#define VIRTIO_BLK_F_DISCARD       13   /* Discard command support */
#define VIRTIO_BLK_F_WRITE_ZEROES  14   /* Write zeroes command support */
#define VIRTIO_BLK_F_LIFETIME      15   /* Storage lifetime information */
#define VIRTIO_BLK_F_SECURE_ERASE  16   /* Secure erase command support */
/* Note: VIRTIO_BLK_F_ROTATION_RATE does not exist in VirtIO 1.x spec.
 * The feature bit 10 is VIRTIO_BLK_F_TOPOLOGY. The rotation_rate field
 * does not appear in the modern virtio_blk_config struct at all. */

/* VirtIO Network device feature bits — lo word (bits 0-31, VirtIO 1.2 spec section 5.1.3) */
#define VIRTIO_NET_F_CSUM                0   /* Checksum offload: device handles partial checksum */
#define VIRTIO_NET_F_GUEST_CSUM          1   /* Guest handles partial checksum */
#define VIRTIO_NET_F_CTRL_GUEST_OFFLOADS 2   /* Control channel offloads reconfiguration */
#define VIRTIO_NET_F_MTU                 3   /* Device reports max MTU */
#define VIRTIO_NET_F_MAC                 5   /* Device has given MAC address */
#define VIRTIO_NET_F_GSO                 6   /* Generic segmentation offload (legacy) */
#define VIRTIO_NET_F_GUEST_TSO4          7   /* Guest can receive TSOv4 */
#define VIRTIO_NET_F_GUEST_TSO6          8   /* Guest can receive TSOv6 */
#define VIRTIO_NET_F_GUEST_ECN           9   /* Guest can receive TSO with ECN */
#define VIRTIO_NET_F_GUEST_UFO           10  /* Guest can receive UFO */
#define VIRTIO_NET_F_HOST_TSO4           11  /* Device can receive TSOv4 */
#define VIRTIO_NET_F_HOST_TSO6           12  /* Device can receive TSOv6 */
#define VIRTIO_NET_F_HOST_ECN            13  /* Device can receive TSO with ECN */
#define VIRTIO_NET_F_HOST_UFO            14  /* Device can receive UFO */
#define VIRTIO_NET_F_MRG_RXBUF           15  /* Driver can merge receive buffers */
#define VIRTIO_NET_F_STATUS              16  /* Configuration status field available */
#define VIRTIO_NET_F_CTRL_VQ             17  /* Control channel available */
#define VIRTIO_NET_F_CTRL_RX             18  /* Control channel RX mode support */
#define VIRTIO_NET_F_CTRL_VLAN           19  /* Control channel VLAN filtering */
#define VIRTIO_NET_F_GUEST_ANNOUNCE      21  /* Driver can send gratuitous packets */
#define VIRTIO_NET_F_MQ                  22  /* Multiqueue with automatic receive steering */
#define VIRTIO_NET_F_CTRL_MAC_ADDR       23  /* Set MAC address through control channel */
/* VirtIO Network device feature bits — hi word (bits 56-63 = hi bits 24-31, VirtIO 1.2 spec 5.1.3) */
#define VIRTIO_NET_F_HOST_USO            56  /* Device can receive USO packets */
#define VIRTIO_NET_F_HASH_REPORT         57  /* Device can report per-packet hash value */
#define VIRTIO_NET_F_GUEST_HDRLEN        59  /* Driver can provide exact hdr_len value */
#define VIRTIO_NET_F_RSS                 60  /* RSS with Toeplitz hash (requires CTRL_VQ) */
#define VIRTIO_NET_F_RSC_EXT             61  /* Process duplicated ACKs, report coalesced segments */
#define VIRTIO_NET_F_STANDBY             62  /* Device may act as standby for primary with same MAC */
#define VIRTIO_NET_F_SPEED_DUPLEX        63  /* Device reports speed and duplex */

/* VirtIO GPU device feature bits */
#define VIRTIO_GPU_F_VIRGL          0   /* Virgl rendering support */
#define VIRTIO_GPU_F_EDID           1   /* EDID support */
#define VIRTIO_GPU_F_RESOURCE_UUID  2   /* Resource UUID support */
#define VIRTIO_GPU_F_RESOURCE_BLOB  3   /* Resource blob support */
#define VIRTIO_GPU_F_CONTEXT_INIT   4   /* Context initialization support */

/* VirtIO File System device feature bits (VirtIO 1.2 spec section 5.11.3) */
#define VIRTIO_FS_F_NOTIFICATION    0   /* Device has FUSE notify message support (enables notification queue) */

/* VirtIO 9P (FileSystem) device feature bits */
#define VIRTIO_9P_F_MOUNT_TAG       0   /* Mount tag support */

/* VirtIO IOMMU device feature bits */
#define VIRTIO_IOMMU_F_INPUT_RANGE  0   /* Input address range */
#define VIRTIO_IOMMU_F_DOMAIN_RANGE 1   /* Domain range */
#define VIRTIO_IOMMU_F_MAP_UNMAP    2   /* Map/unmap commands */
#define VIRTIO_IOMMU_F_BYPASS       3   /* Bypass support */
#define VIRTIO_IOMMU_F_PROBE        4   /* Probe support */
#define VIRTIO_IOMMU_F_RESV_MEM     5   /* Reserved memory support */

static const char *cfg_type_name(uint8 t)
{
    switch (t) {
    case VIRTIO_PCI_CAP_COMMON_CFG: return "COMMON_CFG";
    case VIRTIO_PCI_CAP_NOTIFY_CFG: return "NOTIFY_CFG";
    case VIRTIO_PCI_CAP_ISR_CFG:    return "ISR_CFG";
    case VIRTIO_PCI_CAP_DEVICE_CFG: return "DEVICE_CFG";
    case VIRTIO_PCI_CAP_PCI_CFG:    return "PCI_CFG";
    default:                         return "UNKNOWN";
    }
}

static const char *device_type_name(uint16 devid)
{
    /* Device ID = 0x1040 + device_type */
    uint16 dtype = devid - 0x1040;
    switch (dtype) {
    case VIRTIO_DEV_NET:            return "Network Card";
    case VIRTIO_DEV_BLOCK:          return "Block Device";
    case VIRTIO_DEV_CONSOLE:        return "Console";
    case VIRTIO_DEV_RNG:            return "Entropy Source";
    case VIRTIO_DEV_BALLOON_LEGACY: return "Memory Balloon (Legacy)";
    case VIRTIO_DEV_IOMEMORY:       return "IO Memory";
    case VIRTIO_DEV_RPMSG:          return "RPC Message";
    case VIRTIO_DEV_SCSI:           return "SCSI Host";
    case VIRTIO_DEV_9P:             return "9P Transport";
    case VIRTIO_DEV_WLAN:           return "MAC80211 WLAN";
    case VIRTIO_DEV_RPROC_SERIAL:   return "RProc Serial";
    case VIRTIO_DEV_CAIF:           return "Virtio CAIF";
    case VIRTIO_DEV_BALLOON:        return "Memory Balloon";
    case VIRTIO_DEV_GPU:            return "GPU";
    case VIRTIO_DEV_TIMER:          return "Timer/Clock";
    case VIRTIO_DEV_INPUT:          return "Input Device";
    case VIRTIO_DEV_SOCKET:         return "Socket";
    case VIRTIO_DEV_CRYPTO:         return "Crypto";
    case VIRTIO_DEV_SIGNAL_DIST:    return "Signal Distribution Module";
    case VIRTIO_DEV_PSTORE:         return "PStore";
    case VIRTIO_DEV_IOMMU:          return "IOMMU";
    case VIRTIO_DEV_MEMORY:         return "Memory Device";
    case VIRTIO_DEV_AUDIO:          return "Audio";
    case VIRTIO_DEV_FILESYSTEM:     return "File System";
    case VIRTIO_DEV_PMEM:           return "PMEM";
    case VIRTIO_DEV_RPMB:           return "RPMB";
    case VIRTIO_DEV_HWSIM:          return "MAC80211 HWSIM";
    case VIRTIO_DEV_VIDEO_ENC:      return "Video Encoder";
    case VIRTIO_DEV_VIDEO_DEC:      return "Video Decoder";
    case VIRTIO_DEV_SCMI:           return "SCMI";
    case VIRTIO_DEV_NITRO_MODULE:   return "Nitro Secure Module";
    case VIRTIO_DEV_I2C:            return "I2C Adapter";
    case VIRTIO_DEV_WATCHDOG:       return "Watchdog";
    case VIRTIO_DEV_CAN:            return "CAN Device";
    case VIRTIO_DEV_PARAM_SERVER:   return "Parameter Server";
    case VIRTIO_DEV_AUDIO_POLICY:   return "Audio Policy";
    case VIRTIO_DEV_BLUETOOTH:      return "Bluetooth";
    case VIRTIO_DEV_GPIO:           return "GPIO";
    case VIRTIO_DEV_RDMA:           return "RDMA";
    default:
        if (devid >= 0x1000 && devid <= 0x103F)
            return "Legacy/Transitional";
        return "Unknown";
    }
}

/* Decode and print VirtIO Block device-specific feature bits */
static void print_block_features(uint32 lo, uint32 hi)
{
    IExec->DebugPrintF("[test_modern]   VirtIO Block Device Features:\n");
    if (lo & (1 << VIRTIO_BLK_F_BARRIER))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_BLK_F_BARRIER (legacy: request barriers)\n");
    if (lo & (1 << VIRTIO_BLK_F_SIZE_MAX))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_BLK_F_SIZE_MAX (size_max field valid)\n");
    if (lo & (1 << VIRTIO_BLK_F_SEG_MAX))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_BLK_F_SEG_MAX (seg_max field valid)\n");
    if (lo & (1 << VIRTIO_BLK_F_GEOMETRY))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_BLK_F_GEOMETRY (geometry struct valid)\n");
    if (lo & (1 << VIRTIO_BLK_F_RO))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_BLK_F_RO (read-only device)\n");
    if (lo & (1 << VIRTIO_BLK_F_BLK_SIZE))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_BLK_F_BLK_SIZE (blk_size field valid)\n");
    if (lo & (1 << VIRTIO_BLK_F_SCSI))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_BLK_F_SCSI (legacy: SCSI packet commands)\n");
    if (lo & (1 << VIRTIO_BLK_F_FLUSH))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_BLK_F_FLUSH (cache flush command)\n");
    if (lo & (1 << VIRTIO_BLK_F_TOPOLOGY))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_BLK_F_TOPOLOGY (topology struct valid)\n");
    if (lo & (1 << VIRTIO_BLK_F_CONFIG_WCE))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_BLK_F_CONFIG_WCE (writeback field valid)\n");
    if (lo & (1 << VIRTIO_BLK_F_MQ))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_BLK_F_MQ (num_queues field valid)\n");
    if (lo & (1 << VIRTIO_BLK_F_DISCARD))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_BLK_F_DISCARD (discard command support)\n");
    if (lo & (1 << VIRTIO_BLK_F_WRITE_ZEROES))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_BLK_F_WRITE_ZEROES (write zeroes command)\n");
    if (lo & (1 << VIRTIO_BLK_F_LIFETIME))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_BLK_F_LIFETIME (storage lifetime info)\n");
    if (lo & (1 << VIRTIO_BLK_F_SECURE_ERASE))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_BLK_F_SECURE_ERASE (secure erase command)\n");
}

/* Decode and print VirtIO Network device-specific feature bits */
static void print_network_features(uint32 lo, uint32 hi)
{
    IExec->DebugPrintF("[test_modern]   VirtIO Network Device Features:\n");
    /* lo word: bits 0-31 */
    if (lo & (1UL << VIRTIO_NET_F_CSUM))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_CSUM (0: checksum offload)\n");
    if (lo & (1UL << VIRTIO_NET_F_GUEST_CSUM))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_GUEST_CSUM (1: guest checksums)\n");
    if (lo & (1UL << VIRTIO_NET_F_CTRL_GUEST_OFFLOADS))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_CTRL_GUEST_OFFLOADS (2: control channel offload reconfig)\n");
    if (lo & (1UL << VIRTIO_NET_F_MTU))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_MTU (3: MTU reporting)\n");
    if (lo & (1UL << VIRTIO_NET_F_MAC))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_MAC (5: MAC address set)\n");
    if (lo & (1UL << VIRTIO_NET_F_GSO))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_GSO (6: generic segmentation offload, legacy)\n");
    if (lo & (1UL << VIRTIO_NET_F_GUEST_TSO4))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_GUEST_TSO4 (7: guest TCP seg IPv4)\n");
    if (lo & (1UL << VIRTIO_NET_F_GUEST_TSO6))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_GUEST_TSO6 (8: guest TCP seg IPv6)\n");
    if (lo & (1UL << VIRTIO_NET_F_GUEST_ECN))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_GUEST_ECN (9: guest ECN)\n");
    if (lo & (1UL << VIRTIO_NET_F_GUEST_UFO))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_GUEST_UFO (10: guest UDP fragmentation)\n");
    if (lo & (1UL << VIRTIO_NET_F_HOST_TSO4))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_HOST_TSO4 (11: host TCP seg IPv4)\n");
    if (lo & (1UL << VIRTIO_NET_F_HOST_TSO6))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_HOST_TSO6 (12: host TCP seg IPv6)\n");
    if (lo & (1UL << VIRTIO_NET_F_HOST_ECN))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_HOST_ECN (13: host ECN)\n");
    if (lo & (1UL << VIRTIO_NET_F_HOST_UFO))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_HOST_UFO (14: host UDP fragmentation)\n");
    if (lo & (1UL << VIRTIO_NET_F_MRG_RXBUF))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_MRG_RXBUF (15: merge RX buffers)\n");
    if (lo & (1UL << VIRTIO_NET_F_STATUS))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_STATUS (16: config status field available)\n");
    if (lo & (1UL << VIRTIO_NET_F_CTRL_VQ))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_CTRL_VQ (17: control queue available)\n");
    if (lo & (1UL << VIRTIO_NET_F_CTRL_RX))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_CTRL_RX (18: control channel RX mode)\n");
    if (lo & (1UL << VIRTIO_NET_F_CTRL_VLAN))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_CTRL_VLAN (19: VLAN filtering)\n");
    if (lo & (1UL << VIRTIO_NET_F_GUEST_ANNOUNCE))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_GUEST_ANNOUNCE (21: gratuitous packets)\n");
    if (lo & (1UL << VIRTIO_NET_F_MQ))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_MQ (22: multiqueue / auto receive steering)\n");
    if (lo & (1UL << VIRTIO_NET_F_CTRL_MAC_ADDR))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_CTRL_MAC_ADDR (23: set MAC via control channel)\n");
    /* hi word: bits 56-63 are hi bits 24-31 */
    if (hi & (1UL << (VIRTIO_NET_F_HOST_USO    - 32)))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_HOST_USO (56: host receives USO packets)\n");
    if (hi & (1UL << (VIRTIO_NET_F_HASH_REPORT - 32)))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_HASH_REPORT (57: per-packet hash reporting)\n");
    if (hi & (1UL << (VIRTIO_NET_F_GUEST_HDRLEN - 32)))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_GUEST_HDRLEN (59: driver provides exact hdr_len)\n");
    if (hi & (1UL << (VIRTIO_NET_F_RSS         - 32)))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_RSS (60: RSS with Toeplitz hash)\n");
    if (hi & (1UL << (VIRTIO_NET_F_RSC_EXT     - 32)))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_RSC_EXT (61: duplicate ACK coalescing)\n");
    if (hi & (1UL << (VIRTIO_NET_F_STANDBY     - 32)))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_STANDBY (62: standby for primary device)\n");
    if (hi & (1UL << (VIRTIO_NET_F_SPEED_DUPLEX - 32)))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_NET_F_SPEED_DUPLEX (63: speed/duplex reporting)\n");
}

/* Decode and print VirtIO GPU device-specific feature bits */
static void print_gpu_features(uint32 lo, uint32 hi)
{
    IExec->DebugPrintF("[test_modern]   VirtIO GPU Device Features:\n");
    if (lo & (1 << VIRTIO_GPU_F_VIRGL))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_GPU_F_VIRGL (Virgl rendering)\n");
    if (lo & (1 << VIRTIO_GPU_F_EDID))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_GPU_F_EDID (EDID support)\n");
    if (lo & (1 << VIRTIO_GPU_F_RESOURCE_UUID))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_GPU_F_RESOURCE_UUID (resource UUID)\n");
    if (lo & (1 << VIRTIO_GPU_F_RESOURCE_BLOB))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_GPU_F_RESOURCE_BLOB (resource blob)\n");
    if (lo & (1 << VIRTIO_GPU_F_CONTEXT_INIT))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_GPU_F_CONTEXT_INIT (context initialization)\n");
}

/* Decode and print VirtIO 9P device-specific feature bits */
static void print_9p_features(uint32 lo, uint32 hi)
{
    IExec->DebugPrintF("[test_modern]   VirtIO 9P FileSystem Features:\n");
    if (lo & (1 << VIRTIO_9P_F_MOUNT_TAG))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_9P_F_MOUNT_TAG (mount tag support)\n");
}

/* Decode and print VirtIO File System device-specific feature bits */
static void print_fs_features(uint32 lo, uint32 hi)
{
    IExec->DebugPrintF("[test_modern]   VirtIO File System Device Features:\n");
    if (lo & (1 << VIRTIO_FS_F_NOTIFICATION))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_FS_F_NOTIFICATION (0: FUSE notify messages + notification queue)\n");
}

/* Decode and print VirtIO IOMMU device-specific feature bits */
static void print_iommu_features(uint32 lo, uint32 hi)
{
    IExec->DebugPrintF("[test_modern]   VirtIO IOMMU Device Features:\n");
    if (lo & (1 << VIRTIO_IOMMU_F_INPUT_RANGE))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_IOMMU_F_INPUT_RANGE (input address range)\n");
    if (lo & (1 << VIRTIO_IOMMU_F_DOMAIN_RANGE))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_IOMMU_F_DOMAIN_RANGE (domain range)\n");
    if (lo & (1 << VIRTIO_IOMMU_F_MAP_UNMAP))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_IOMMU_F_MAP_UNMAP (map/unmap commands)\n");
    if (lo & (1 << VIRTIO_IOMMU_F_BYPASS))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_IOMMU_F_BYPASS (bypass support)\n");
    if (lo & (1 << VIRTIO_IOMMU_F_PROBE))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_IOMMU_F_PROBE (probe support)\n");
    if (lo & (1 << VIRTIO_IOMMU_F_RESV_MEM))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_IOMMU_F_RESV_MEM (reserved memory)\n");
}

/* Decode and print VirtIO reserved feature bits (bits 24-40 across both halves) */
static void print_reserved_features(uint32 lo, uint32 hi)
{
    /* Reserved features in lo (bits 24-31) */
    if (lo & (1 << 24))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_F_NOTIFY_ON_EMPTY (legacy, transitional only)\n");
    if (lo & (1 << 25))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_F_25 (reserved)\n");
    if (lo & (1 << 26))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_F_26 (reserved)\n");
    if (lo & (1 << 27))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_F_ANY_LAYOUT (legacy, transitional only)\n");
    
    /* Reserved features in hi (bits 32-40 = bits 0-8 in hi word) */
    if (hi & (1 << (VIRTIO_F_VERSION_1 - 32)))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_F_VERSION_1 (VirtIO 1.0+ compliance) **REQUIRED**\n");
    if (hi & (1 << (VIRTIO_F_ACCESS_PLATFORM - 32)))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_F_ACCESS_PLATFORM (IOMMU/address translation support)\n");
    if (hi & (1 << (VIRTIO_F_RING_PACKED - 32)))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_F_RING_PACKED (packed virtqueue layout support)\n");
    if (hi & (1 << (VIRTIO_F_IN_ORDER - 32)))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_F_IN_ORDER (in-order buffer use guarantee)\n");
    if (hi & (1 << (VIRTIO_F_ORDER_PLATFORM - 32)))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_F_ORDER_PLATFORM (platform-ordered memory access)\n");
    if (hi & (1 << (VIRTIO_F_SR_IOV - 32)))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_F_SR_IOV (Single Root I/O Virtualization)\n");
    if (hi & (1 << (VIRTIO_F_NOTIFICATION_DATA - 32)))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_F_NOTIFICATION_DATA (driver notifications w/ extra data)\n");
    if (hi & (1 << (VIRTIO_F_NOTIF_CONFIG_DATA - 32)))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_F_NOTIF_CONFIG_DATA (use queue_notify_data)\n");
    if (hi & (1 << (VIRTIO_F_RING_RESET - 32)))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_F_RING_RESET (individual queue reset support)\n");
}

/* Decode and print VirtIO common feature bits (bits 28-29 in lower 32) */
static void print_common_features(uint32 lo, uint32 hi)
{
    IExec->DebugPrintF("[test_modern]   Common (Queue/Negotiation) Features:\n");
    if (lo & (1 << VIRTIO_F_INDIRECT_DESC))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_F_INDIRECT_DESC (scatter-gather via indirect descriptors)\n");
    if (lo & (1 << VIRTIO_F_EVENT_IDX))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_F_EVENT_IDX (used/avail ring event index batching)\n");
    
    IExec->DebugPrintF("[test_modern]   Reserved Features:\n");
    print_reserved_features(lo, hi);
}

/* Decode and print VirtIO SCSI-specific feature bits (bits 0-3 in SCSI feature space) */
static void print_scsi_features(uint32 lo, uint32 hi)
{
    IExec->DebugPrintF("[test_modern]   VirtIO SCSI Device Features:\n");
    
    if (lo & (1 << VIRTIO_SCSI_F_INOUT))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_SCSI_F_INOUT (single cmd with read+write buffers) **IMPORTANT**\n");
    else
        IExec->DebugPrintF("[test_modern]     [!] VIRTIO_SCSI_F_INOUT NOT advertised (commands cannot mix input/output)\n");
    
    if (lo & (1 << VIRTIO_SCSI_F_HOTPLUG))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_SCSI_F_HOTPLUG (device hotplug/unplug event reporting)\n");
    if (lo & (1 << VIRTIO_SCSI_F_CHANGE))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_SCSI_F_CHANGE (PARAM_CHANGE event reporting)\n");
    if (lo & (1 << VIRTIO_SCSI_F_T10_PI))
        IExec->DebugPrintF("[test_modern]     • VIRTIO_SCSI_F_T10_PI (T10 DIF/DIX protection information)\n");
}

/* stwbrx/lwbrx macros for direct MMIO access on Pegasos2 */
#define MMIO_W32(addr, val) do { \
    uint32 _v = (val); \
    uint32 *_a = (uint32 *)(addr); \
    __asm__ volatile("stwbrx %1,0,%0; mbar" : : "r"(_a), "r"(_v) : "memory"); \
} while(0)

#define MMIO_R32(addr) __extension__({ \
    uint32 _r; \
    uint32 *_a = (uint32 *)(addr); \
    __asm__ volatile("lwbrx %0,0,%1" : "=r"(_r) : "r"(_a) : "memory"); \
    _r; \
})

#define MMIO_W8(addr, val) do { \
    uint8 _v = (val); \
    uint8 *_a = (uint8 *)(addr); \
    __asm__ volatile("stb %1,0(%0); mbar" : : "r"(_a), "r"(_v) : "memory"); \
} while(0)

#define MMIO_R8(addr) __extension__({ \
    uint8 _r; \
    uint8 *_a = (uint8 *)(addr); \
    __asm__ volatile("lbz %0,0(%1)" : "=r"(_r) : "r"(_a) : "memory"); \
    _r; \
})

int main(void)
{
    IExec->DebugPrintF("\n[test_modern] VirtIO Modern Device Feature Test (Pegasos2/MV64361)\n");
    IExec->DebugPrintF("[test_modern] ======================================================\n\n");

    struct Library *ExpansionBase = IExec->OpenLibrary("expansion.library", 54);
    if (!ExpansionBase) {
        IExec->DebugPrintF("[test_modern] ERROR: cannot open expansion.library v54\n");
        return 1;
    }

    struct PCIIFace *IPCI = (struct PCIIFace *)IExec->GetInterface(ExpansionBase, "pci", 1, NULL);
    if (!IPCI) {
        IExec->DebugPrintF("[test_modern] ERROR: cannot get IPCI interface\n");
        IExec->CloseLibrary(ExpansionBase);
        return 1;
    }
    IExec->DebugPrintF("[test_modern] expansion.library + IPCI OK\n");

    /* Find ANY VirtIO device (vendor ID 0x1AF4) — let the device-aware logic determine device type */
    struct PCIDevice *dev = IPCI->FindDeviceTags(
        FDT_VendorID, 0x1AF4,
        TAG_DONE);

    if (!dev) {
        IExec->DebugPrintF("[test_modern] ERROR: No VirtIO device found (vendor ID 0x1AF4)\n");
        IExec->DropInterface((struct Interface *)IPCI);
        IExec->CloseLibrary(ExpansionBase);
        return 1;
    }

    uint8 bus, slot, fn;
    dev->GetAddress(&bus, &slot, &fn);
    uint16 vendor = dev->ReadConfigWord(PCI_VENDOR_ID);
    uint16 devid  = dev->ReadConfigWord(PCI_DEVICE_ID);
    
    /* Extract device type from PCI Device ID (0x1040 = Network, 0x1041 = Block, etc.)
     * Virtio device types: devid - 0x1040 (or mask lower 8 bits if using 0x1000 base) */
    uint16 device_type = (devid >= 0x1040) ? (devid - 0x1040) : devid;
    
    IExec->DebugPrintF("[test_modern] Found: %04lX:%04lX at %02lX:%02lX.%lu (%s)\n\n",
        (uint32)vendor, (uint32)devid, (uint32)bus, (uint32)slot, (uint32)fn, device_type_name(devid));

    /* Enable PCI Memory Space + Bus Master */
    uint16 pci_cmd = dev->ReadConfigWord(PCI_COMMAND);
    if (!(pci_cmd & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER))) {
        dev->WriteConfigWord(PCI_COMMAND, pci_cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    }

    /* Set AmigaOS capability */
    uint32 caps = dev->GetCapabilities();
    if (!(caps & PCI_CAP_BUSMASTER)) {
        dev->SetCapabilities(PCI_CAP_BUSMASTER | PCI_CAP_SETCLR);
    }

    /* Lock device exclusively */
    BOOL locked = dev->Lock(PCI_LOCK_EXCLUSIVE);
    if (!locked) {
        IExec->DebugPrintF("[test_modern] WARNING: could not lock device exclusively\n");
    }

    /* Find COMMON_CFG, NOTIFY_CFG, and DEVICE_CFG capabilities */
    IExec->DebugPrintF("[test_modern] Scanning PCI capabilities:\n");
    BOOL found_modern = FALSE;
    uint32 common_phys = 0;
    uint32 common_base = 0;
    uint32 device_cfg_base = 0;
    uint32 notify_cfg_base = 0;
    uint32 notify_off_mult = 0;

    struct PCICapability *cap = dev->GetFirstCapability();
    int cap_idx = 0;
    while (cap && cap_idx < 32) {
        if (cap->Type == PCI_CAPABILITYID_VENDOR) {
            uint8  cfg_type = dev->ReadConfigByte(cap->CapOffset + VIRTIO_CAP_OFF_CFG_TYPE);
            uint8  bar_num  = dev->ReadConfigByte(cap->CapOffset + VIRTIO_CAP_OFF_BAR);
            uint32 offset   = dev->ReadConfigLong(cap->CapOffset + VIRTIO_CAP_OFF_OFFSET);

            struct PCIResourceRange *cap_bar = dev->GetResourceRange(bar_num);
            if (cap_bar) {
                uint32 phys = cap_bar->Physical + offset;
                uint32 base = cap_bar->BaseAddress + offset;

                IExec->DebugPrintF("[test_modern]   [%d] %s (BAR%lu+0x%lX) phys=0x%08lX base=0x%08lX\n",
                    cap_idx, cfg_type_name(cfg_type), (uint32)bar_num, offset, phys, base);

                if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG) {
                    common_phys = phys;
                    common_base = base;
                    found_modern = TRUE;
                } else if (cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG) {
                    device_cfg_base = base;
                } else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                    notify_cfg_base = base;
                    notify_off_mult = dev->ReadConfigLong(cap->CapOffset + VIRTIO_CAP_OFF_NOTIFY_MULT);
                    IExec->DebugPrintF("[test_modern]     notify_off_multiplier: %lu\n", notify_off_mult);
                }
            }
        }
        cap = dev->GetNextCapability(cap);
        cap_idx++;
    }

    if (!found_modern || !common_base) {
        IExec->DebugPrintF("[test_modern] ERROR: No COMMON_CFG capability found\n");
        if (locked) dev->Unlock();
        IExec->DropInterface((struct Interface *)IPCI);
        IExec->CloseLibrary(ExpansionBase);
        return 1;
    }

    IExec->DebugPrintF("[test_modern]\n[test_modern] VirtIO 1.0 Modern device initialized.\n");
    IExec->DebugPrintF("[test_modern] COMMON_CFG  at virt=0x%08lX phys=0x%08lX\n", common_base, common_phys);
    IExec->DebugPrintF("[test_modern] DEVICE_CFG  at virt=0x%08lX\n", device_cfg_base);
    IExec->DebugPrintF("[test_modern] NOTIFY_CFG  at virt=0x%08lX  mult=%lu\n\n", notify_cfg_base, notify_off_mult);

    /* ============================================================
     * MMIO Access via stwbrx/lwbrx (native Pegasos2 method)
     * ============================================================ */
    {
        volatile uint32 *base32 = (volatile uint32 *)common_base;
        volatile uint8  *base8  = (volatile uint8  *)common_base;
        uint32 feat_lo, feat_hi;

        /* Reset device */
        MMIO_W8(base8 + VIRTIO_PCI_COMMON_STATUS, 0x00);
        IExec->DebugPrintF("[test_modern] STEP 1: Reset (STATUS=0x00)\n");

        /* Wait for reset to be acknowledged */
        uint32 tries = 0;
        uint8 status;
        do {
            status = MMIO_R8(base8 + VIRTIO_PCI_COMMON_STATUS);
            tries++;
        } while (status != 0 && tries < 1000);

        if (status != 0) {
            IExec->DebugPrintF("[test_modern] WARNING: Reset did not complete (status=0x%02lX)\n", (uint32)status);
        }

        /* Set ACKNOWLEDGE + DRIVER status bits */
        MMIO_W8(base8 + VIRTIO_PCI_COMMON_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
        MMIO_W8(base8 + VIRTIO_PCI_COMMON_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
        IExec->DebugPrintF("[test_modern] STEP 2: Set ACKNOWLEDGE + DRIVER\n\n");

        /* Read device features via DFSELECT */
        IExec->DebugPrintF("[test_modern] STEP 3: Read Device Features\n");
        IExec->DebugPrintF("[test_modern]   Writing DFSELECT=0...\n");
        MMIO_W32(base32 + VIRTIO_PCI_COMMON_DFSELECT / 4, 0);
        feat_lo = MMIO_R32(base32 + VIRTIO_PCI_COMMON_DF / 4);
        IExec->DebugPrintF("[test_modern]   Device Features (lo) = 0x%08lX\n", feat_lo);

        IExec->DebugPrintF("[test_modern]   Writing DFSELECT=1...\n");
        MMIO_W32(base32 + VIRTIO_PCI_COMMON_DFSELECT / 4, 1);
        feat_hi = MMIO_R32(base32 + VIRTIO_PCI_COMMON_DF / 4);
        IExec->DebugPrintF("[test_modern]   Device Features (hi) = 0x%08lX\n\n", feat_hi);

        /* Decode and display ALL features based on detected device type */
        print_common_features(feat_lo, feat_hi);
        IExec->DebugPrintF("\n");
        
        /* Call appropriate device-specific feature decoder */
        switch (device_type) {
        case VIRTIO_DEV_NET:
            print_network_features(feat_lo, feat_hi);
            break;
        case VIRTIO_DEV_BLOCK:
            print_block_features(feat_lo, feat_hi);
            break;
        case VIRTIO_DEV_SCSI:
            print_scsi_features(feat_lo, feat_hi);
            break;
        case VIRTIO_DEV_GPU:
            print_gpu_features(feat_lo, feat_hi);
            break;
        case VIRTIO_DEV_9P:
            print_9p_features(feat_lo, feat_hi);
            break;
        case VIRTIO_DEV_FILESYSTEM:
            print_fs_features(feat_lo, feat_hi);
            break;
        case VIRTIO_DEV_IOMMU:
            print_iommu_features(feat_lo, feat_hi);
            break;
        default:
            IExec->DebugPrintF("[test_modern] Device-specific features: Device type %lu not yet implemented\n", (uint32)device_type);
            break;
        }

        /* Accept feature set per VirtIO spec requirements.
         * Common features (bits 24-40): VIRTIO_F_VERSION_1 (required), VIRTIO_F_EVENT_IDX, VIRTIO_F_INDIRECT_DESC
         * Device-specific features: vary by device type (SCSI, Block, Network, GPU, 9P, IOMMU)
         */
        IExec->DebugPrintF("[test_modern]\n[test_modern] STEP 4: Accept Features (device-aware)\n");
        MMIO_W32(base32 + VIRTIO_PCI_COMMON_DFSELECTG / 4, 0);
        
        uint32 drv_feat_lo = 0;
        if (feat_lo & (1UL << VIRTIO_F_INDIRECT_DESC))
            drv_feat_lo |= (1UL << VIRTIO_F_INDIRECT_DESC);
        if (feat_lo & (1UL << VIRTIO_F_EVENT_IDX))
            drv_feat_lo |= (1UL << VIRTIO_F_EVENT_IDX);
        
        /* Accept device-specific features based on device type */
        switch (device_type) {
        case VIRTIO_DEV_SCSI:
            /* SCSI-specific features we want to accept */
            if (feat_lo & (1UL << VIRTIO_SCSI_F_INOUT))
                drv_feat_lo |= (1UL << VIRTIO_SCSI_F_INOUT);
            if (feat_lo & (1UL << VIRTIO_SCSI_F_HOTPLUG))
                drv_feat_lo |= (1UL << VIRTIO_SCSI_F_HOTPLUG);
            if (feat_lo & (1UL << VIRTIO_SCSI_F_CHANGE))
                drv_feat_lo |= (1UL << VIRTIO_SCSI_F_CHANGE);
            break;
        case VIRTIO_DEV_BLOCK:
            /* Block device features we want to accept */
            if (feat_lo & (1UL << VIRTIO_BLK_F_BLK_SIZE))
                drv_feat_lo |= (1UL << VIRTIO_BLK_F_BLK_SIZE);
            if (feat_lo & (1UL << VIRTIO_BLK_F_FLUSH))
                drv_feat_lo |= (1UL << VIRTIO_BLK_F_FLUSH);
            if (feat_lo & (1UL << VIRTIO_BLK_F_TOPOLOGY))
                drv_feat_lo |= (1UL << VIRTIO_BLK_F_TOPOLOGY);
            if (feat_lo & (1UL << VIRTIO_BLK_F_GEOMETRY))
                drv_feat_lo |= (1UL << VIRTIO_BLK_F_GEOMETRY);
            if (feat_lo & (1UL << VIRTIO_BLK_F_SEG_MAX))
                drv_feat_lo |= (1UL << VIRTIO_BLK_F_SEG_MAX);
            break;
        case VIRTIO_DEV_NET:
            /* Network device features we want to accept for probe/observation */
            if (feat_lo & (1UL << VIRTIO_NET_F_MAC))
                drv_feat_lo |= (1UL << VIRTIO_NET_F_MAC);
            if (feat_lo & (1UL << VIRTIO_NET_F_STATUS))
                drv_feat_lo |= (1UL << VIRTIO_NET_F_STATUS);
            if (feat_lo & (1UL << VIRTIO_NET_F_MTU))
                drv_feat_lo |= (1UL << VIRTIO_NET_F_MTU);
            if (feat_lo & (1UL << VIRTIO_NET_F_CTRL_VQ))
                drv_feat_lo |= (1UL << VIRTIO_NET_F_CTRL_VQ);
            if (feat_lo & (1UL << VIRTIO_NET_F_CSUM))
                drv_feat_lo |= (1UL << VIRTIO_NET_F_CSUM);
            if (feat_lo & (1UL << VIRTIO_NET_F_MRG_RXBUF))
                drv_feat_lo |= (1UL << VIRTIO_NET_F_MRG_RXBUF);
            break;
        case VIRTIO_DEV_GPU:
            if (feat_lo & (1UL << VIRTIO_GPU_F_EDID))
                drv_feat_lo |= (1UL << VIRTIO_GPU_F_EDID);
            if (feat_lo & (1UL << VIRTIO_GPU_F_VIRGL))
                drv_feat_lo |= (1UL << VIRTIO_GPU_F_VIRGL);
            break;
        case VIRTIO_DEV_FILESYSTEM:
            if (feat_lo & (1UL << VIRTIO_FS_F_NOTIFICATION))
                drv_feat_lo |= (1UL << VIRTIO_FS_F_NOTIFICATION);
            break;
        case VIRTIO_DEV_9P:
        case VIRTIO_DEV_IOMMU:
            /* For other devices, just accept common features for now */
            break;
        }
        
        MMIO_W32(base32 + VIRTIO_PCI_COMMON_DFG / 4, drv_feat_lo);
        IExec->DebugPrintF("[test_modern]   Accepted driver features (lo): 0x%08lX\n", drv_feat_lo);

        MMIO_W32(base32 + VIRTIO_PCI_COMMON_DFSELECTG / 4, 1);
        uint32 drv_feat_hi = 0;
        /* VERSION_1 is REQUIRED for modern devices */
        if (feat_hi & (1UL << (VIRTIO_F_VERSION_1 - 32)))
            drv_feat_hi |= (1UL << (VIRTIO_F_VERSION_1 - 32));
        /* Other common features */
        if (feat_hi & (1UL << (VIRTIO_F_RING_PACKED - 32)))
            drv_feat_hi |= (1UL << (VIRTIO_F_RING_PACKED - 32));
        if (feat_hi & (1UL << (VIRTIO_F_IN_ORDER - 32)))
            drv_feat_hi |= (1UL << (VIRTIO_F_IN_ORDER - 32));
        
        MMIO_W32(base32 + VIRTIO_PCI_COMMON_DFG / 4, drv_feat_hi);
        IExec->DebugPrintF("[test_modern]   Accepted driver features (hi): 0x%08lX\n", drv_feat_hi);

        /* Set FEATURES_OK status */
        IExec->DebugPrintF("[test_modern]\n[test_modern] STEP 5: Set FEATURES_OK\n");
        MMIO_W8(base8 + VIRTIO_PCI_COMMON_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
        status = MMIO_R8(base8 + VIRTIO_PCI_COMMON_STATUS);
        IExec->DebugPrintF("[test_modern]   Status after FEATURES_OK: 0x%02lX\n", (uint32)status);

        if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
            IExec->DebugPrintF("[test_modern]   ERROR: Device rejected features (FEATURES_OK not set)\n");
        } else {
            /* Read num_queues */
            MMIO_W32(base32 + VIRTIO_PCI_COMMON_DFSELECT / 4, 0);  /* reset DFSELECT */
            uint8 nq_lo = MMIO_R8(base8 + VIRTIO_PCI_COMMON_NUMQ);
            uint8 nq_hi = MMIO_R8(base8 + VIRTIO_PCI_COMMON_NUMQ + 1);
            uint16 num_queues = (uint16)nq_lo | ((uint16)nq_hi << 8);
            IExec->DebugPrintF("[test_modern]\n[test_modern] STEP 6: Device Configuration\n");
            IExec->DebugPrintF("[test_modern]   num_queues: %lu\n", (uint32)num_queues);

            /* Set DRIVER_OK to complete initialization */
            IExec->DebugPrintF("[test_modern]\n[test_modern] STEP 7: Set DRIVER_OK\n");
            MMIO_W8(base8 + VIRTIO_PCI_COMMON_STATUS,
                    VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                    VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
            status = MMIO_R8(base8 + VIRTIO_PCI_COMMON_STATUS);
            IExec->DebugPrintF("[test_modern]   Final status: 0x%02lX (expect 0x0F)\n", (uint32)status);

            if (status == 0x0F) {
                IExec->DebugPrintF("[test_modern]\n[test_modern] SUCCESS: VirtIO Modern initialization complete\n");
            } else {
                IExec->DebugPrintF("[test_modern]\n[test_modern] WARNING: VirtIO Modern initialization incomplete\n");
            }

            /* STEP 8: Read device-specific configuration from DEVICE_CFG region.
             * DEVICE_CFG is only valid after DRIVER_OK (spec sec 4.1.4.4).
             * All fields are little-endian; assemble from bytes via MMIO_R8/MMIO_R32.
             */
            if (device_cfg_base) {
                volatile uint8  *dcfg8  = (volatile uint8  *)device_cfg_base;
                volatile uint32 *dcfg32 = (volatile uint32 *)device_cfg_base;

                IExec->DebugPrintF("[test_modern]\n[test_modern] STEP 8: Device-Specific Configuration (DEVICE_CFG @ 0x%08lX)\n",
                    device_cfg_base);

                switch (device_type) {

                case VIRTIO_DEV_BLOCK:
                    /* virtio_blk_config layout (QEMU implementation, VirtIO spec 5.2.4):
                     *  +0  capacity            uint64 LE  (total sectors, always present)
                     *  +8  size_max            uint32 LE  (if BLK_F_SIZE_MAX)
                     * +12  seg_max             uint32 LE  (if BLK_F_SEG_MAX)
                     * +16  geometry.cylinders  uint16 LE  (if BLK_F_GEOMETRY)
                     * +18  geometry.heads      uint8
                     * +19  geometry.sectors    uint8
                     * +20  blk_size            uint32 LE  (if BLK_F_BLK_SIZE)
                     * +24  physical_block_exp  uint8      (if BLK_F_TOPOLOGY)
                     * +25  alignment_offset    uint8
                     * +26  min_io_size         uint16 LE
                     * +28  opt_io_size         uint32 LE
                     * +32  writeback           uint8
                     * +33  unused0             uint8
                     * +34  num_queues          uint16 LE  (QEMU overlaps rotation_rate here in older builds)
                     * +36  max_discard_sectors uint32 LE
                     * +40  max_discard_seg     uint32 LE
                     * +44  discard_sector_alignment uint32 LE
                     * +48  max_write_zeroes_sectors uint32 LE
                     * +52  max_write_zeroes_seg     uint32 LE
                     * +56  write_zeroes_may_unmap   uint8
                     */
                    {
                        uint32 cap_lo = MMIO_R32(dcfg32 + 0);  /* capacity low 32 bits */
                        uint32 cap_hi = MMIO_R32(dcfg32 + 1);  /* capacity high 32 bits */
                        IExec->DebugPrintF("[test_modern]   capacity:    0x%08lX_%08lX sectors (%lu sectors)\n",
                            cap_hi, cap_lo, cap_lo);

                        if (feat_lo & (1UL << VIRTIO_BLK_F_SIZE_MAX)) {
                            uint32 size_max = MMIO_R32(dcfg32 + 2);
                            IExec->DebugPrintF("[test_modern]   size_max:    %lu bytes (max segment size)\n", size_max);
                        }
                        if (feat_lo & (1UL << VIRTIO_BLK_F_SEG_MAX)) {
                            uint32 seg_max = MMIO_R32(dcfg32 + 3);
                            IExec->DebugPrintF("[test_modern]   seg_max:     %lu (max segments per request)\n", seg_max);
                        }
                        if (feat_lo & (1UL << VIRTIO_BLK_F_GEOMETRY)) {
                            uint8 cyl_lo   = MMIO_R8(dcfg8 + 16);
                            uint8 cyl_hi   = MMIO_R8(dcfg8 + 17);
                            uint16 cyl     = (uint16)cyl_lo | ((uint16)cyl_hi << 8);
                            uint8 heads    = MMIO_R8(dcfg8 + 18);
                            uint8 sectors  = MMIO_R8(dcfg8 + 19);
                            IExec->DebugPrintF("[test_modern]   geometry:    cyl=%lu heads=%lu sectors=%lu\n",
                                (uint32)cyl, (uint32)heads, (uint32)sectors);
                        }
                        if (feat_lo & (1UL << VIRTIO_BLK_F_BLK_SIZE)) {
                            uint32 blk_size = MMIO_R32(dcfg32 + 5);  /* +20 */
                            IExec->DebugPrintF("[test_modern]   blk_size:    %lu bytes (logical block size)\n", blk_size);
                        }
                        if (feat_lo & (1UL << VIRTIO_BLK_F_TOPOLOGY)) {
                            /* topology struct at +24 (8 bytes):
                             * +24 physical_block_exp (u8)
                             * +25 alignment_offset   (u8)
                             * +26 min_io_size         (u16 LE)
                             * +28 opt_io_size         (u32 LE)
                             */
                            uint8  pbe    = MMIO_R8(dcfg8 + 24);
                            uint8  aoff   = MMIO_R8(dcfg8 + 25);
                            uint8  mis_lo = MMIO_R8(dcfg8 + 26);
                            uint8  mis_hi = MMIO_R8(dcfg8 + 27);
                            uint16 min_io = (uint16)mis_lo | ((uint16)mis_hi << 8);
                            uint32 opt_io = MMIO_R32(dcfg32 + 7);  /* +28 */
                            IExec->DebugPrintF("[test_modern]   topology:    physical_block_exp=%lu alignment_offset=%lu\n",
                                (uint32)pbe, (uint32)aoff);
                            IExec->DebugPrintF("[test_modern]                min_io_size=%lu opt_io_size=%lu\n",
                                (uint32)min_io, opt_io);
                        }
                        /* writeback at +32 (always present if CONFIG_WCE negotiated) */
                        if (feat_lo & (1UL << VIRTIO_BLK_F_CONFIG_WCE)) {
                            uint8 wb = MMIO_R8(dcfg8 + 32);
                            IExec->DebugPrintF("[test_modern]   writeback:   %lu (%s)\n",
                                (uint32)wb, wb ? "writeback" : "writethrough");
                        }
                        /* num_queues at +34 (only if BLK_F_MQ negotiated) */
                        if (feat_lo & (1UL << VIRTIO_BLK_F_MQ)) {
                            uint8  nq_lo = MMIO_R8(dcfg8 + 34);
                            uint8  nq_hi = MMIO_R8(dcfg8 + 35);
                            uint16 nq    = (uint16)nq_lo | ((uint16)nq_hi << 8);
                            IExec->DebugPrintF("[test_modern]   num_queues:  %lu\n", (uint32)nq);
                        }
                    }
                    break;

                case VIRTIO_DEV_SCSI:
                    /* virtio_scsi_config layout (VirtIO spec 5.6.4):
                     *  +0  num_queues     uint32 LE
                     *  +4  seg_max        uint32 LE
                     *  +8  max_sectors    uint32 LE
                     * +12  cmd_per_lun    uint32 LE
                     * +16  event_info_size uint32 LE
                     * +20  sense_size     uint32 LE
                     * +24  cdb_size       uint32 LE
                     * +28  max_channel    uint16 LE
                     * +30  max_target     uint16 LE
                     * +32  max_lun        uint32 LE
                     */
                    {
                        uint32 scsi_num_q      = MMIO_R32(dcfg32 + 0);  /* +0  num_queues */
                        uint32 seg_max         = MMIO_R32(dcfg32 + 1);  /* +4  seg_max */
                        uint32 max_sectors     = MMIO_R32(dcfg32 + 2);  /* +8  max_sectors */
                        uint32 cmd_per_lun     = MMIO_R32(dcfg32 + 3);  /* +12 cmd_per_lun */
                        uint32 event_info_size = MMIO_R32(dcfg32 + 4);  /* +16 event_info_size */
                        uint32 sense_size      = MMIO_R32(dcfg32 + 5);  /* +20 sense_size */
                        uint32 cdb_size        = MMIO_R32(dcfg32 + 6);  /* +24 cdb_size */
                        uint8  mc_lo           = MMIO_R8(dcfg8 + 28);
                        uint8  mc_hi           = MMIO_R8(dcfg8 + 29);
                        uint16 max_channel     = (uint16)mc_lo | ((uint16)mc_hi << 8);
                        uint8  mt_lo           = MMIO_R8(dcfg8 + 30);
                        uint8  mt_hi           = MMIO_R8(dcfg8 + 31);
                        uint16 max_target      = (uint16)mt_lo | ((uint16)mt_hi << 8);
                        uint32 max_lun         = MMIO_R32(dcfg32 + 8);  /* +32 max_lun */
                        IExec->DebugPrintF("[test_modern]   num_queues:       %lu\n", scsi_num_q);
                        IExec->DebugPrintF("[test_modern]   seg_max:          %lu\n", seg_max);
                        IExec->DebugPrintF("[test_modern]   max_sectors:      %lu\n", max_sectors);
                        IExec->DebugPrintF("[test_modern]   cmd_per_lun:      %lu\n", cmd_per_lun);
                        IExec->DebugPrintF("[test_modern]   event_info_size:  %lu\n", event_info_size);
                        IExec->DebugPrintF("[test_modern]   sense_size:       %lu\n", sense_size);
                        IExec->DebugPrintF("[test_modern]   cdb_size:         %lu\n", cdb_size);
                        IExec->DebugPrintF("[test_modern]   max_channel:      %lu\n", (uint32)max_channel);
                        IExec->DebugPrintF("[test_modern]   max_target:       %lu\n", (uint32)max_target);
                        IExec->DebugPrintF("[test_modern]   max_lun:          %lu\n", max_lun);
                    }
                    break;

                case VIRTIO_DEV_NET:
                    /* virtio_net_config layout (VirtIO 1.2 spec section 5.1.4):
                     *  +0   mac[6]               uint8[6]  (if NET_F_MAC)
                     *  +6   status               uint16 LE (if NET_F_STATUS)
                     *  +8   max_virtqueue_pairs  uint16 LE (if NET_F_MQ or NET_F_RSS)
                     * +10   mtu                  uint16 LE (if NET_F_MTU)
                     * +12   speed                uint32 LE (if NET_F_SPEED_DUPLEX)
                     * +16   duplex               uint8     (if NET_F_SPEED_DUPLEX)
                     *
                     * Virtqueues: receiveq0=0, transmitq0=1, ..., receiveqN=2(N-1), transmitqN=2(N-1)+1
                     * controlq = 2N (only if NET_F_CTRL_VQ)
                     * N = max_virtqueue_pairs if NET_F_MQ or NET_F_RSS negotiated, else N=1
                     */
                    {
                        if (feat_lo & (1UL << VIRTIO_NET_F_MAC)) {
                            uint8 mac0 = MMIO_R8(dcfg8 + 0);
                            uint8 mac1 = MMIO_R8(dcfg8 + 1);
                            uint8 mac2 = MMIO_R8(dcfg8 + 2);
                            uint8 mac3 = MMIO_R8(dcfg8 + 3);
                            uint8 mac4 = MMIO_R8(dcfg8 + 4);
                            uint8 mac5 = MMIO_R8(dcfg8 + 5);
                            IExec->DebugPrintF("[test_modern]   mac:              %02lX:%02lX:%02lX:%02lX:%02lX:%02lX\n",
                                (uint32)mac0, (uint32)mac1, (uint32)mac2,
                                (uint32)mac3, (uint32)mac4, (uint32)mac5);
                        }
                        if (feat_lo & (1UL << VIRTIO_NET_F_STATUS)) {
                            uint8 st_lo = MMIO_R8(dcfg8 + 6);
                            uint8 st_hi = MMIO_R8(dcfg8 + 7);
                            uint16 net_status = (uint16)st_lo | ((uint16)st_hi << 8);
                            IExec->DebugPrintF("[test_modern]   status:           0x%04lX (%s)\n",
                                (uint32)net_status,
                                (net_status & 1) ? "LINK_UP" : "LINK_DOWN");
                        }
                        /* max_virtqueue_pairs present if MQ or RSS negotiated */
                        if ((feat_lo & (1UL << VIRTIO_NET_F_MQ)) ||
                            (feat_hi & (1UL << (VIRTIO_NET_F_RSS - 32)))) {
                            uint8  mvp_lo = MMIO_R8(dcfg8 + 8);
                            uint8  mvp_hi = MMIO_R8(dcfg8 + 9);
                            uint16 mvp    = (uint16)mvp_lo | ((uint16)mvp_hi << 8);
                            IExec->DebugPrintF("[test_modern]   max_virtq_pairs:  %lu\n", (uint32)mvp);
                        }
                        if (feat_lo & (1UL << VIRTIO_NET_F_MTU)) {
                            uint8 mtu_lo = MMIO_R8(dcfg8 + 10);
                            uint8 mtu_hi = MMIO_R8(dcfg8 + 11);
                            uint16 mtu   = (uint16)mtu_lo | ((uint16)mtu_hi << 8);
                            IExec->DebugPrintF("[test_modern]   mtu:              %lu bytes\n", (uint32)mtu);
                        }
                        if (feat_hi & (1UL << (VIRTIO_NET_F_SPEED_DUPLEX - 32))) {
                            uint32 speed  = MMIO_R32(dcfg32 + 3);  /* +12 */
                            uint8  duplex = MMIO_R8(dcfg8 + 16);
                            IExec->DebugPrintF("[test_modern]   speed:            %lu Mbps\n", speed);
                            IExec->DebugPrintF("[test_modern]   duplex:           %lu (%s)\n",
                                (uint32)duplex, duplex ? "full" : "half");
                        }
                    }
                    break;

                case VIRTIO_DEV_GPU:
                    /* virtio_gpu_config layout (VirtIO spec section 5.7.4):
                     *  +0  events_read   uint32 LE  (pending event flags from device)
                     *  +4  events_clear  uint32 LE  (write to clear pending events)
                     *  +8  num_scanouts  uint32 LE  (max number of scanout displays)
                     * +12  num_capsets   uint32 LE  (number of supported capability sets)
                     */
                    {
                        uint32 events_read  = MMIO_R32(dcfg32 + 0);  /* +0  events_read */
                        uint32 events_clear = MMIO_R32(dcfg32 + 1);  /* +4  events_clear */
                        uint32 num_scanouts = MMIO_R32(dcfg32 + 2);  /* +8  num_scanouts */
                        uint32 num_capsets  = MMIO_R32(dcfg32 + 3);  /* +12 num_capsets */
                        IExec->DebugPrintF("[test_modern]   events_read:      0x%08lX\n", events_read);
                        IExec->DebugPrintF("[test_modern]   events_clear:     0x%08lX\n", events_clear);
                        IExec->DebugPrintF("[test_modern]   num_scanouts:     %lu\n", num_scanouts);
                        IExec->DebugPrintF("[test_modern]   num_capsets:      %lu\n", num_capsets);
                    }
                    break;

                case VIRTIO_DEV_FILESYSTEM:
                    /* virtio_fs_config layout (VirtIO spec section 5.11.4):
                     *  +0   tag[36]              char[36]  UTF-8 name, NUL-padded (not NUL-terminated if full)
                     * +36   num_request_queues   uint32 LE (always present)
                     * +40   notify_buf_size      uint32 LE (only if VIRTIO_FS_F_NOTIFICATION negotiated)
                     *
                     * Virtqueues: 0=hiprio, 1=notification (if FS_F_NOTIFICATION), 2..n=request queues
                     */
                    {
                        char tag[37];
                        int i;
                        for (i = 0; i < 36; i++)
                            tag[i] = (char)MMIO_R8(dcfg8 + i);
                        tag[36] = '\0';
                        uint8  nrq_b0 = MMIO_R8(dcfg8 + 36);
                        uint8  nrq_b1 = MMIO_R8(dcfg8 + 37);
                        uint8  nrq_b2 = MMIO_R8(dcfg8 + 38);
                        uint8  nrq_b3 = MMIO_R8(dcfg8 + 39);
                        uint32 num_request_queues = (uint32)nrq_b0 |
                                                    ((uint32)nrq_b1 << 8) |
                                                    ((uint32)nrq_b2 << 16) |
                                                    ((uint32)nrq_b3 << 24);
                        IExec->DebugPrintF("[test_modern]   tag:                \"%s\"\n", tag);
                        IExec->DebugPrintF("[test_modern]   num_request_queues: %lu\n", num_request_queues);
                        if (feat_lo & (1UL << VIRTIO_FS_F_NOTIFICATION)) {
                            uint32 nbs = MMIO_R32(dcfg32 + 10);  /* +40 */
                            IExec->DebugPrintF("[test_modern]   notify_buf_size:    %lu bytes\n", nbs);
                        }
                    }
                    break;

                default:
                    IExec->DebugPrintF("[test_modern]   (no device-specific config decoder for this device type)\n");
                    break;
                }
            } else {
                IExec->DebugPrintF("[test_modern]\n[test_modern] STEP 8: No DEVICE_CFG capability found — skipping\n");
            }
        }
    }

    IExec->DebugPrintF("[test_modern]\n");
    if (locked) dev->Unlock();
    IExec->DropInterface((struct Interface *)IPCI);
    IExec->CloseLibrary(ExpansionBase);
    return 0;
}
