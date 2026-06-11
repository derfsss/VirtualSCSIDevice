#
# VirtioSCSIInstallerLocale - locale wrapper
# Auto-generated -- do not edit; regenerate from the fixture module.
#

import catalog


class VirtioSCSIInstallerLocale:
    strings = {}
    cat = None

    MSG_WELCOME = 1
    MSG_FINISH = 2
    MSG_REBOOT = 3

    def __init__(self, language="", catalogName='VirtioSCSI.catalog', builtinLanguage='english'):
        self.strings[self.MSG_WELCOME] = '\nWelcome to the installation of the VirtIO SCSI device driver.\n\nvirtioscsi.device exposes QEMU VirtIO SCSI virtual disks to AmigaOS 4.1 Final Edition as standard block devices, on the AmigaOne, Pegasos2, and SAM460ex QEMU machines.  It is intended for AmigaOS systems running inside QEMU and serves no purpose on real hardware.\n\nThe following changes will be made to your system:\n\n    1.  virtioscsi.device will be copied to "SYS:Kickstart"\n\n    2.  "SYS:Kickstart/Kicklayout" will be updated to load the driver during startup; the previous configuration will be preserved as "Kicklayout.bak"\n\nA system restart is required to complete the installation.\n\n\nPress "Next" to continue.'
        self.strings[self.MSG_FINISH] = '\nThe installation completed successfully.\n\nvirtioscsi.device has been copied to "SYS:Kickstart" and "SYS:Kickstart/Kicklayout" has been updated.  The previous configuration was preserved as "Kicklayout.bak".  The driver will be activated by the next system restart.\n\nPlease ensure QEMU provides a VirtIO SCSI controller.  The same device works on all supported machines (AmigaOne, Pegasos2, and SAM460ex) -- the driver auto-detects the best transport at boot:\n\n    -device virtio-scsi-pci,id=scsi0\n\nand attach drives to it:\n\n    -drive file=disk.img,if=none,id=vd0,format=raw\n    -device scsi-hd,drive=vd0,bus=scsi0.0,channel=0,scsi-id=0,lun=0\n\nIf your system boots via BBoot with a kickstart.zip, add the line "MODULE Kickstart/virtioscsi.device" to the Kicklayout inside that zip instead.\n\nPlease note: when restarting from within QEMU, the virtual machine may power off instead of restarting.  Should this occur, simply start QEMU again.\n\n\nPress "Finish" to exit the installation.'
        self.strings[self.MSG_REBOOT] = 'Restart the system now (required to activate the driver)'

        try:
            self.cat = catalog.OpenCatalog(catalogName, language, builtinLanguage)
        except:
            self.cat = None

    def GetString(self, id):
        if self.cat != None:
            return self.cat.GetString(id, self.strings[id])
        return self.strings[id]
