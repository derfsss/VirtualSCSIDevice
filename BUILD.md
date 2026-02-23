# Building VirtualSCSIDevice

This project uses a cross-compiler targeting AmigaOS 4.1 PowerPC. The recommended build environment is **WSL2** with the **Docker** image `walkero/amigagccondocker:os4-gcc11`.

## Build Command (WSL2 / Docker)

Run the following command from the project root directory in a WSL2 terminal:

```bash
docker run --rm -v $(pwd):/src -w /src walkero/amigagccondocker:os4-gcc11 make clean all
```

### Troubleshooting Volume Mapping
If you encounter `invalid mode: /src` or similar errors, ensure you are using absolute paths for the volume mapping, especially if running from a script or non-interactive shell:

```bash
docker run --rm -v /mnt/w/Code/amiga/antigravity/projects/VirtualSCSIDevice:/src -w /src walkero/amigagccondocker:os4-gcc11 make clean all
```

## Binary Location
Upon a successful build, the following files will be created:
- `build/virtioscsi.device`: The AmigaOS 4.1 device driver.
- `build/test_inquiry`: A standalone test utility for SCSI inquiry.

## Manual Testing
To test the driver manually on a running system without a reboot:
1. Load the device: `LoadSeg build/virtioscsi.device` (use `InitResident` if needed).
2. Use `Mounter` to announce manually or check `Media Toolbox`.
