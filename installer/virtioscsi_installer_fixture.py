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
messages, and behaviour.  The page idioms and the Kicklayout edit are
expanded from `installergen.presets` -- the field-tested templates
shared by all of this author's driver installers.

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
zip, not from SYS:Kickstart/Kicklayout.  The readme tells such users
to add the module line to the zip instead.
"""

from installergen import (
    Project, Page, PageKind, Package, PackageKind, PostInstallAction,
    LocaleString, LocaleRef, Handler,
)
from installergen.presets import (
    README_BUTTON_LOCALE, InsertAfterLast, welcome_with_readme,
    finish_page, system_edit_helper, system_edit_exit_handler,
)


# NOTE: the Installation Utility's page text is PLAIN TEXT only and the
# label does NOT scroll -- keep pages inside the ~20-rendered-line lint
# budget and defer detail to the bundled readme.  Formatting follows
# Hyperion's own Update installers: leading blank line, paragraph
# spacing, quoted file and button names, explicit navigation cue.
locale = [
    LocaleString(
        "MSG_WELCOME",
        "\nWelcome to the installation of the VirtIO SCSI device "
        "driver.\n\n"
        "virtioscsi.device exposes QEMU VirtIO SCSI virtual disks to "
        "AmigaOS 4.1 Final Edition as standard block devices, on the "
        "AmigaOne, Pegasos2, and SAM460ex QEMU machines.\n\n"
        "The following changes will be made to your system:\n\n"
        "    1.  virtioscsi.device will be copied to \"SYS:Kickstart\"\n\n"
        "    2.  \"SYS:Kickstart/Kicklayout\" will be updated to load "
        "the driver during startup (backup: \"Kicklayout.bak\")\n\n"
        "A system restart completes the installation.  Click "
        "\"View Readme\" below for manual installation details, the "
        "QEMU device setup, and general instructions on use.\n\n\n"
        "Press \"Next\" to continue."),
    README_BUTTON_LOCALE,
    LocaleString(
        "MSG_FINISH",
        "\nThe installation has finished.\n\n"
        "virtioscsi.device has been copied to \"SYS:Kickstart\" and "
        "\"SYS:Kickstart/Kicklayout\" has been updated (backup: "
        "\"Kicklayout.bak\").  The driver activates on the next "
        "system restart.\n\n"
        "The QEMU device setup and BBoot/kickstart.zip notes are in "
        "the README_os4depot.txt file in this drawer.\n\n\n"
        "Press \"Finish\" to exit the installation."),
    LocaleString(
        "MSG_REBOOT",
        "Restart the system now (required to activate the driver)"),
]


# Welcome page with the View Readme button (proven preset).
welcome_page = welcome_with_readme(
    LocaleRef("MSG_WELCOME"), "README_os4depot.txt")

# Kicklayout edit: append the MODULE line directly after the last
# existing device-driver entry so the new driver loads alongside the
# other disk drivers (proven preset: idempotent, .bak backup, LF-only).
update_kicklayout = system_edit_helper(
    "SYS:Kickstart/Kicklayout",
    "MODULE Kickstart/virtioscsi.device",
    InsertAfterLast(contains=".device"),
)

# The edit runs when the INSTALL page is left in the forward direction
# -- i.e. after the file copy has completed.
install_page = Page(
    var_name="installPage",
    kind=PageKind.INSTALL,
    exit_handler=system_edit_exit_handler(
        "SYS:Kickstart/Kicklayout",
        "MODULE Kickstart/virtioscsi.device",
        "virtioscsi.device installer",
        "manually, after the existing device driver lines:",
    ),
)

finish = finish_page(LocaleRef("MSG_FINISH"))


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
    pages=[welcome_page, install_page, finish],
    packages=[driver_package],
    post_install_actions=[reboot_action],
)
