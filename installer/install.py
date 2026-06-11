#
# VirtIO SCSI Device Driver Install install.py
# $VER: VirtIO SCSI Device Driver Install 1.12 (11.06.2026)
# Auto-generated -- do not edit; regenerate from the fixture module.
#

from installer import *
from VirtioSCSIInstallerLocale import *
import amiga
import os

loc = VirtioSCSIInstallerLocale()

def updateKicklayout():
    kl = "SYS:Kickstart/Kicklayout"
    module_line = "MODULE Kickstart/virtioscsi.device"
    try:
        f = open(kl, "rb")
        data = f.read()
        f.close()
    except IOError:
        return "could not read " + kl
    lines = data.split("\n")
    for ln in lines:
        if ln.strip() == module_line:
            return None        # already installed
    last_dev = -1
    last_mod = -1
    for i in range(len(lines)):
        stripped = lines[i].strip()
        if stripped.startswith("MODULE"):
            last_mod = i
            if stripped.find(".device") != -1:
                last_dev = i
    insert_at = last_dev
    if insert_at == -1:
        insert_at = last_mod
    if insert_at == -1:
        return "no MODULE lines found in " + kl
    out = lines[:insert_at + 1] + [module_line] + lines[insert_at + 1:]
    try:
        b = open(kl + ".bak", "wb")
        b.write(data)
        b.close()
    except IOError:
        pass                   # backup is best-effort
    try:
        f = open(kl, "wb")
        f.write("\n".join(out))
        f.close()
    except IOError:
        return "could not write " + kl
    return None

##############################################
# welcomePage
welcomePage = NewPage(GUI)

def readmeLaunch(page, id):
    amiga.system('notepad *>NIL: "README_os4depot.txt"')
    return True

StartGUI(welcomePage)
BeginGroup(GROUP_VERTICAL)
AddLabel(label=loc.GetString(loc.MSG_WELCOME), align=ALIGN_LEFT, weight=6)
BeginGroup(GROUP_HORIZONTAL, weight=0)
AddSpace(weight=1)
AddButton(label=loc.GetString(loc.MSG_README_BUTTON), frame=BUTTON_FRAME, onclick=readmeLaunch, weight=10)
AddSpace(weight=1)
EndGroup()
AddSpace(weight=1)
EndGroup()
EndGUI()

##############################################
# installPage
installPage = NewPage(INSTALL)

def installExitHandler(page_nr, direction):
    if direction != 1:
        return True
    err = updateKicklayout()
    if err:
        try:
            import asl
            asl.MessageBox("virtioscsi.device installer",
                "Kicklayout update failed: " + err + "\n\n"
                "Please add this line to SYS:Kickstart/Kicklayout\n"
                "manually, after the existing device driver lines:\n\n"
                "MODULE Kickstart/virtioscsi.device",
                "OK")
        except StandardError:
            pass
    return True
SetObject(installPage, "exithandler", installExitHandler)

##############################################
# Post-install actions

def rebootHandler():
    amiga.system("reboot SYNC")
    return True

AddPostInstallAction(
    name='Reboot',
    description=loc.GetString(loc.MSG_REBOOT),
    visible=True,
    default=True,
    callback=rebootHandler,
    )

##############################################
# finishPage
finishPage = NewPage(FINISH)
SetString(finishPage, 'message', loc.GetString(loc.MSG_FINISH))

##############################################
# Top-level packages (always registered)

_pkg = AddPackage(FILEPACKAGE,
    name='VirtIO SCSI device driver',
    files=['content/virtioscsi.device'],
    alternatepath="SYS:Kickstart"
    )

##############################################
# Run the installer
RunInstaller()
