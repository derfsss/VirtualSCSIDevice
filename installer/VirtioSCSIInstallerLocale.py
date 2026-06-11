#
# VirtioSCSIInstallerLocale - locale wrapper
# Auto-generated -- do not edit; regenerate from the fixture module.
#

import catalog


class VirtioSCSIInstallerLocale:
    strings = {}
    cat = None

    MSG_WELCOME = 1
    MSG_README_BUTTON = 2
    MSG_FINISH = 3
    MSG_REBOOT = 4

    def __init__(self, language="", catalogName='VirtioSCSI.catalog', builtinLanguage='english'):
        self.strings[self.MSG_WELCOME] = '\nWelcome to the installation of the VirtIO SCSI device driver.\n\nvirtioscsi.device exposes QEMU VirtIO SCSI virtual disks to AmigaOS 4.1 Final Edition as standard block devices, on the AmigaOne, Pegasos2, and SAM460ex QEMU machines.\n\nThe following changes will be made to your system:\n\n    1.  virtioscsi.device will be copied to "SYS:Kickstart"\n\n    2.  "SYS:Kickstart/Kicklayout" will be updated to load the driver during startup (backup: "Kicklayout.bak")\n\nA system restart completes the installation.  Click "View Readme" below for manual installation details, the QEMU device setup, and general instructions on use.\n\n\nPress "Next" to continue.'
        self.strings[self.MSG_README_BUTTON] = 'View Readme...'
        self.strings[self.MSG_FINISH] = '\nThe installation has finished.\n\nvirtioscsi.device has been copied to "SYS:Kickstart" and "SYS:Kickstart/Kicklayout" has been updated (backup: "Kicklayout.bak").  The driver activates on the next system restart.\n\nThe QEMU device setup and BBoot/kickstart.zip notes are in the README_os4depot.txt file in this drawer.\n\n\nPress "Finish" to exit the installation.'
        self.strings[self.MSG_REBOOT] = 'Restart the system now (required to activate the driver)'

        try:
            self.cat = catalog.OpenCatalog(catalogName, language, builtinLanguage)
        except:
            self.cat = None

    def GetString(self, id):
        if self.cat != None:
            return self.cat.GetString(id, self.strings[id])
        return self.strings[id]
