"""Installer-script fixture for the virtioscsi.device installer.

Produces an AmigaOS 4.1 FE `Installation Utility` script (Python 2.5)
that:

  1. copies virtioscsi.device into SYS:Kickstart/
  2. appends `MODULE Kickstart/virtioscsi.device` to
     SYS:Kickstart/Kicklayout, immediately AFTER the last existing
     device-driver MODULE line (e.g. a1ide.device.kmod /
     peg2ide.device.kmod) -- with a Kicklayout.bak backup, LF-only
     line endings, and idempotency
  3. offers a reboot on the finish page

install.py + VirtioSCSIInstallerLocale.py are emitted from this
fixture by an in-house installer-script generator and committed, so
building the distribution archive needs no extra tooling.  This
fixture is the authoritative description of the installer's pages,
messages, and behaviour.

Archive layout consumed by the script (see `make dist`):

    VirtualSCSIDevice/
      install.py
      VirtioSCSIInstallerLocale.py
      content/virtioscsi.device

IMPORTANT: the installer must be launched with the drawer as the
current directory (double-clicking the install.py icon does this; from
a shell, CD into the drawer first) -- the package uses drawer-relative
content/ paths, exactly like the OS's own update installers.

NOTE for BBoot / kickstart.zip setups: those load Kickstart from the
zip, not from SYS:Kickstart/Kicklayout.  The finish page tells such
users to add the module line to the zip instead.
"""

from installergen import (
    Project, Page, PageKind, Package, PackageKind, PostInstallAction,
    LocaleString, LocaleRef,
)
from installergen.model import Handler


# NOTE: the Installation Utility's page text is PLAIN TEXT only --
# formatting follows the conventions of Hyperion's own Update
# installers: leading blank line, paragraph spacing, indented numbered
# steps, quoted file and button names, and an explicit navigation cue.
locale = [
    LocaleString(
        "MSG_WELCOME",
        "\nWelcome to the installation of the VirtIO SCSI device "
        "driver.\n\n"
        "virtioscsi.device exposes QEMU VirtIO SCSI virtual disks to "
        "AmigaOS 4.1 Final Edition as standard block devices, on the "
        "AmigaOne, Pegasos2, and SAM460ex QEMU machines.  It is "
        "intended for AmigaOS systems running inside QEMU and serves "
        "no purpose on real hardware.\n\n"
        "The following changes will be made to your system:\n\n"
        "    1.  virtioscsi.device will be copied to \"SYS:Kickstart\"\n\n"
        "    2.  \"SYS:Kickstart/Kicklayout\" will be updated to load "
        "the driver during startup; the previous configuration will be "
        "preserved as \"Kicklayout.bak\"\n\n"
        "A system restart is required to complete the installation.\n\n\n"
        "Press \"Next\" to continue."),
    LocaleString(
        "MSG_FINISH",
        "\nThe installation completed successfully.\n\n"
        "virtioscsi.device has been copied to \"SYS:Kickstart\" and "
        "\"SYS:Kickstart/Kicklayout\" has been updated.  The previous "
        "configuration was preserved as \"Kicklayout.bak\".  The driver "
        "will be activated by the next system restart.\n\n"
        "Please ensure QEMU provides a VirtIO SCSI controller.  The "
        "same device works on all supported machines (AmigaOne, "
        "Pegasos2, and SAM460ex) -- the driver auto-detects the best "
        "transport at boot:\n\n"
        "    -device virtio-scsi-pci,id=scsi0\n\n"
        "and attach drives to it:\n\n"
        "    -drive file=disk.img,if=none,id=vd0,format=raw\n"
        "    -device scsi-hd,drive=vd0,bus=scsi0.0,channel=0,"
        "scsi-id=0,lun=0\n\n"
        "If your system boots via BBoot with a kickstart.zip, add the "
        "line \"MODULE Kickstart/virtioscsi.device\" to the Kicklayout "
        "inside that zip instead.\n\n"
        "Please note: when restarting from within QEMU, the virtual "
        "machine may power off instead of restarting.  Should this "
        "occur, simply start QEMU again.\n\n\n"
        "Press \"Finish\" to exit the installation."),
    LocaleString(
        "MSG_REBOOT",
        "Restart the system now (required to activate the driver)"),
]


# Appends the MODULE line to Kicklayout, directly after the last
# existing device-driver MODULE entry so the new driver loads alongside
# the other disk drivers.  Pure Python 2.5; binary file modes keep the
# LF-only line endings Kickstart loaders require.  Returns an error
# string, or None on success (including the already-installed case).
update_kicklayout = Handler(
    name="updateKicklayout",
    params=[],
    body=(
        "kl = \"SYS:Kickstart/Kicklayout\"\n"
        "module_line = \"MODULE Kickstart/virtioscsi.device\"\n"
        "try:\n"
        "    f = open(kl, \"rb\")\n"
        "    data = f.read()\n"
        "    f.close()\n"
        "except IOError:\n"
        "    return \"could not read \" + kl\n"
        "lines = data.split(\"\\n\")\n"
        "for ln in lines:\n"
        "    if ln.strip() == module_line:\n"
        "        return None        # already installed\n"
        "last_dev = -1\n"
        "last_mod = -1\n"
        "for i in range(len(lines)):\n"
        "    stripped = lines[i].strip()\n"
        "    if stripped.startswith(\"MODULE\"):\n"
        "        last_mod = i\n"
        "        if stripped.find(\".device\") != -1:\n"
        "            last_dev = i\n"
        "insert_at = last_dev\n"
        "if insert_at == -1:\n"
        "    insert_at = last_mod\n"
        "if insert_at == -1:\n"
        "    return \"no MODULE lines found in \" + kl\n"
        "out = lines[:insert_at + 1] + [module_line] + lines[insert_at + 1:]\n"
        "try:\n"
        "    b = open(kl + \".bak\", \"wb\")\n"
        "    b.write(data)\n"
        "    b.close()\n"
        "except IOError:\n"
        "    pass                   # backup is best-effort\n"
        "try:\n"
        "    f = open(kl, \"wb\")\n"
        "    f.write(\"\\n\".join(out))\n"
        "    f.close()\n"
        "except IOError:\n"
        "    return \"could not write \" + kl\n"
        "return None\n"
    ),
)


welcome_page = Page(
    var_name="welcomePage",
    kind=PageKind.WELCOME,
    strings={"message": LocaleRef("MSG_WELCOME")},
)

# The Kicklayout edit runs when the INSTALL page is left in the forward
# direction -- i.e. after the file copy has completed.  Errors are
# reported via asl.MessageBox with manual-fix instructions; the wizard
# still completes so the copied driver isn't left half-installed silently.
install_page = Page(
    var_name="installPage",
    kind=PageKind.INSTALL,
    exit_handler=Handler(
        name="installExitHandler",
        params=["page_nr", "direction"],
        body=(
            "if direction != 1:\n"
            "    return True\n"
            "err = updateKicklayout()\n"
            "if err:\n"
            "    try:\n"
            "        import asl\n"
            "        asl.MessageBox(\"virtioscsi.device installer\",\n"
            "            \"Kicklayout update failed: \" + err + \"\\n\\n\"\n"
            "            \"Please add this line to SYS:Kickstart/Kicklayout\\n\"\n"
            "            \"manually, after the existing device driver lines:\\n\\n\"\n"
            "            \"MODULE Kickstart/virtioscsi.device\",\n"
            "            \"OK\")\n"
            "    except StandardError:\n"
            "        pass\n"
            "return True\n"
        ),
    ),
)

finish_page = Page(
    var_name="finishPage",
    kind=PageKind.FINISH,
    strings={"message": LocaleRef("MSG_FINISH")},
)


driver_package = Package(
    name="VirtIO SCSI device driver",
    files=["content/virtioscsi.device"],
    kind=PackageKind.FILEPACKAGE,
    # Fixed destination: Kickstart modules must land on the boot
    # volume regardless of any user preference, so there is no
    # DESTINATION page and the path is hardwired.  Emitted verbatim,
    # hence the embedded quotes.
    alternatepath="\"SYS:Kickstart\"",
    register_in="top",
)

reboot_action = PostInstallAction(
    name="Reboot",
    description=LocaleRef("MSG_REBOOT"),
    visible=True,
    default=True,
    callback=Handler(
        name="rebootHandler",
        params=[],
        body="amiga.system(\"reboot SYNC\")\nreturn True\n",
    ),
)


project = Project(
    name="VirtIO SCSI Device Driver Install",
    short_name="VirtioSCSI",
    version="1.12",
    date="11.06.2026",
    locale_strings=locale,
    helpers=[update_kicklayout],
    pages=[welcome_page, install_page, finish_page],
    packages=[driver_package],
    post_install_actions=[reboot_action],
)
