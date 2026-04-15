#!/usr/bin/env python3
"""
virtioscsi.device v1.9 automated test matrix.

Runs one QEMU per AmigaOS 4.1 FE target machine, using the user's real
boot disks (copied to /tmp — originals untouched). Updates the
kickstart.zip inside /tmp with the v1.9 build under test, boots to
Workbench, waits for SerialShell (port 4321), and runs a standard
battery of checks via the qemu-runner Python client.

Machines covered:
  - amigaone: BBoot + kickstart.zip via -device loader,addr=0x600000
  - pegasos2: BBoot + kickstart.zip via -initrd
  - sam460ex: native QEMU u-boot boot (no kickstart update path in
    this first pass — user's boot disk holds the driver)

Outputs per-machine pass/fail summary + captures serial debug log.

Run:
    python3 /tmp/run_test_matrix.py [--build debug|release] [--machines a1,peg2,s460]
"""
from __future__ import annotations
import argparse
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
import zipfile

# ----- constants --------------------------------------------------------
QEMU_BIN = "/tmp/qemu/build/qemu-system-ppc"
# We make per-machine renamed copies so parallel claude sessions'
# `killall qemu-system-ppc` can't murder us.
BBOOT_SRC = "/mnt/c/Users/rich_/.kyvos/bboot"
DRIVER_DEBUG = "/mnt/w/Code/amiga/antigravity/projects/VirtualSCSIDevice/build/virtioscsi.device"
TEST_INQUIRY = "/mnt/w/Code/amiga/antigravity/projects/VirtualSCSIDevice/build/test_inquiry"
SCSI_RAW = "/mnt/e/Emulators/QEMU/QEMU_Machines/scsi.raw"
RUNNER_PY = "/mnt/w/Code/amiga/antigravity/projects/tools/qemu-runner"

MACHINES = {
    "a1": {
        "name": "AmigaOne",
        "qemu_machine": "amigaone",
        "src_dir": "/mnt/e/Emulators/QEMU/QEMU_Machines/base_a1_backup",
        "workdir": "/tmp/test_a1",
        "has_kickstart_zip": True,
        "kickstart_loader": "device",  # via -device loader,addr=0x600000
        "ssh_forward_port": 4321,
        "monitor_port": 4322,
    },
    "peg2": {
        "name": "Pegasos2",
        "qemu_machine": "pegasos2",
        "src_dir": "/mnt/e/Emulators/QEMU/QEMU_Machines/backup-peg2-upd3",
        "workdir": "/tmp/test_peg2",
        "has_kickstart_zip": True,
        "kickstart_loader": "initrd",  # via -initrd
        "ssh_forward_port": 4331,      # different port so we can run all in parallel
        "monitor_port": 4332,
    },
    "s460": {
        "name": "SAM460ex",
        "qemu_machine": "sam460ex",
        "src_dir": "/mnt/e/Emulators/QEMU/QEMU_Machines/backup-sam460-upd3",
        "workdir": "/tmp/test_s460",
        "has_kickstart_zip": False,
        "kickstart_loader": None,
        "ssh_forward_port": 4341,
        "monitor_port": 4342,
    },
}

# SerialShell port is hardcoded inside the guest to 4321, but we forward
# to a different host port per machine so we can run in parallel.


# ----- helpers ----------------------------------------------------------
def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def copy_source(m: dict) -> None:
    """Populate workdir from the user's source directory."""
    src = m["src_dir"]
    wd = m["workdir"]
    log(f"[{m['name']}] preparing {wd} ...")
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd, exist_ok=True)

    for fn in ("hd0.qcow2",):
        if os.path.exists(os.path.join(src, fn)):
            log(f"[{m['name']}]   copy {fn} ...")
            shutil.copy2(os.path.join(src, fn), os.path.join(wd, fn))

    if m["has_kickstart_zip"]:
        shutil.copy2(os.path.join(src, "kickstart.zip"),
                     os.path.join(wd, "kickstart.zip"))
        shutil.copy2(BBOOT_SRC, os.path.join(wd, "bboot"))

    # Rename binary to dodge parallel `killall qemu-system-ppc`
    qemu_copy = os.path.join(wd, f"qemu-ppc-{list(MACHINES).pop(0) if False else ''}")
    qemu_copy = os.path.join(wd, "qemu-ppc-myrun")
    shutil.copy2(QEMU_BIN, qemu_copy)
    os.chmod(qemu_copy, 0o755)


def update_kickstart(m: dict, driver_src: str) -> None:
    """Replace Kickstart/virtioscsi.device inside kickstart.zip with driver_src,
    and ensure diskboot.config lists 'virtioscsi.device 8 3' so diskboot.kmod
    scans the device for partitions (required for RDB-based auto-mount)."""
    if not m["has_kickstart_zip"]:
        log(f"[{m['name']}] no kickstart.zip — skipping driver injection")
        return
    zip_path = os.path.join(m["workdir"], "kickstart.zip")
    tmp_dir = os.path.join(m["workdir"], "ks_extract")
    shutil.rmtree(tmp_dir, ignore_errors=True)
    os.makedirs(tmp_dir, exist_ok=True)

    with zipfile.ZipFile(zip_path, "r") as z:
        z.extractall(tmp_dir)

    dst = os.path.join(tmp_dir, "Kickstart", "virtioscsi.device")
    if not os.path.exists(os.path.dirname(dst)):
        os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(driver_src, dst)

    # Ensure diskboot.config mounts our virtio-scsi partitions.  Without this
    # line, diskboot.kmod never scans the device for RDB partitions and the
    # disk won't appear on Workbench — test_inquiry would still pass, but
    # the volume-level check would not.
    dbc_path = os.path.join(tmp_dir, "Kickstart", "diskboot.config")
    if os.path.exists(dbc_path):
        with open(dbc_path, "r", errors="replace") as f:
            dbc = f.read()
        if "virtioscsi.device" not in dbc:
            if not dbc.endswith("\n"):
                dbc += "\n"
            dbc += "virtioscsi.device 8 3\n"
            with open(dbc_path, "w") as f:
                f.write(dbc)
            log(f"[{m['name']}] added 'virtioscsi.device 8 3' to diskboot.config")

    # Rebuild the zip
    os.remove(zip_path)
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for root, _, files in os.walk(tmp_dir):
            for f in files:
                full = os.path.join(root, f)
                arc = os.path.relpath(full, tmp_dir)
                z.write(full, arc)

    log(f"[{m['name']}] kickstart.zip updated with {os.path.basename(driver_src)} "
        f"({os.path.getsize(driver_src):,} B)")


def build_qemu_cmd(m: dict) -> list[str]:
    """Construct QEMU command line per machine."""
    wd = m["workdir"]
    qemu_bin = os.path.join(wd, "qemu-ppc-myrun")
    serial_log = os.path.join(wd, "serial_full.log")
    monitor_sock = os.path.join(wd, "monitor.sock")

    cmd = [qemu_bin,
           "-L", "/tmp/qemu/pc-bios",   # so VOF etc. resolve in our uninstalled build
           "-M", m["qemu_machine"],
           "-m", "2048M",
           "-rtc", "base=localtime",
           "-accel", "tcg",
           "-vga", "none",
           "-display", "none",
           "-serial", f"file:{serial_log}",
           "-monitor", f"unix:{monitor_sock},server,nowait",
           "-name", m["name"]]
    # -append only valid when -kernel is also set (guest-loaded via BBoot)
    if m["kickstart_loader"]:
        cmd += ["-append", "serial debuglevel=1"]

    hd0 = os.path.join(wd, "hd0.qcow2")

    # Only the boot disk is needed.  work.qcow2 and USB-FAT are not used by
    # any stress test and cause write-lock contention if more than one run is
    # live at once (work.qcow2 is shared across all three machines).  Keep
    # the emulated hardware surface to the minimum that still boots AmigaOS
    # cleanly.
    if m["qemu_machine"] == "sam460ex":
        # SAM460 still needs ONE sii3112 because u-boot probes ide.1 for a CD.
        # Without it, u-boot hangs at 'IDE: bus 1' during boot.
        cmd += ["-device", "sii3112",
                "-drive", "if=none,id=cd",
                "-device", "ide-cd,unit=0,drive=cd,bus=ide.1",
                "-drive", f"if=none,id=hd0,file={hd0},format=qcow2",
                "-device", "ide-hd,unit=0,drive=hd0,bus=ide.0"]
    else:
        # AmigaOne / Pegasos2: motherboard IDE is enough on its own.
        cmd += ["-drive", f"if=none,id=hd0,file={hd0},format=qcow2",
                "-device", "ide-hd,unit=0,drive=hd0,bus=ide.0",
                "-drive", "if=none,id=cd",
                "-device", "ide-cd,unit=1,drive=cd,bus=ide.1"]

    # Network with hostfwd for SerialShell.  AmigaOne/Pegasos2 pin the NIC
    # to specific PCI slots via addr=; SAM460 doesn't.
    net_addr = ",addr=0x0a" if m["qemu_machine"] != "sam460ex" else ""
    cmd += ["-device", f"rtl8139{net_addr},netdev=nic,romfile=",
            "-netdev", f"user,id=nic,hostname={m['name']},hostfwd=tcp::{m['ssh_forward_port']}-:4321"]

    # Sound: all three use es1370; only AmigaOne/Pegasos2 pin its address
    sound_addr = ",addr=0x09" if m["qemu_machine"] in ("amigaone", "pegasos2") else ""
    cmd += ["-device", f"es1370{sound_addr}"]

    # SM501 / siliconmotion.chip: only AmigaOne and Pegasos2 (not SAM460)
    if m["qemu_machine"] != "sam460ex":
        cmd += ["-device", "sm501"]

    # BBoot kickstart injection
    if m["kickstart_loader"] == "device":
        cmd += ["-kernel", os.path.join(wd, "bboot"),
                "-device", f"loader,addr=0x600000,file={os.path.join(wd, 'kickstart.zip')}"]
    elif m["kickstart_loader"] == "initrd":
        cmd += ["-kernel", os.path.join(wd, "bboot"),
                "-initrd", os.path.join(wd, "kickstart.zip")]
    # sam460ex: no loader — boots hd0 directly via built-in u-boot

    # virtio-scsi test device (snapshot=on so scsi.raw is never written)
    cmd += ["-device", "virtio-scsi-pci,id=scsi0",
            "-drive", f"file={SCSI_RAW},if=none,id=vd0,format=raw,snapshot=on",
            "-device", "scsi-hd,drive=vd0,bus=scsi0.0,channel=0,scsi-id=0,lun=0"]

    return cmd


def wait_for_serialshell(port: int, timeout: int = 240) -> bool:
    """Poll TCP port; return True when connectable, False on timeout."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("localhost", port), timeout=1) as s:
                # Best-effort: if we got a connection the listener is up
                _ = s.recv(32)   # drain READY marker if any
                return True
        except OSError:
            time.sleep(3)
    return False


def run_checks(m: dict) -> dict:
    """Query the guest via SerialShell. Return dict with pass/fail per check."""
    # Run in a subprocess so stdlib path doesn't conflict with our helpers
    runner = f"""
import sys
sys.path.insert(0, r'{RUNNER_PY}')
from serial_client import SerialClient
c = SerialClient('localhost', {m['ssh_forward_port']})
c.connect()
try:
    outs = {{}}
    outs['version'] = c.send_command('version virtioscsi.device full', timeout=20)
    outs['avail']   = c.send_command('avail', timeout=10)
    # Driver-level I/O
    c.upload_file(r'{TEST_INQUIRY}', 'RAM:test_inquiry')
    outs['inquiry'] = c.send_command('RAM:test_inquiry 0 0', timeout=40)
    # Filesystem mount + round-trip I/O on the virtio-scsi volume.
    # scsi.raw has an RDB with one SFS partition labelled 'SCSI'.
    # diskboot.config entry (added by update_kickstart) causes diskboot.kmod
    # to scan our device and mount that partition as DH2:/SCSI: on every boot.
    outs['info'] = c.send_command('info', timeout=15)
    # Round-trip write / read / verify on the mounted volume.
    outs['fs_write'] = c.send_command('echo 42-v19-fs-rt >SCSI:fs_rt.txt', timeout=15)
    outs['fs_read']  = c.send_command('type SCSI:fs_rt.txt', timeout=15)
    outs['fs_list']  = c.send_command('list SCSI:fs_rt.txt', timeout=15)
    outs['fs_del']   = c.send_command('delete SCSI:fs_rt.txt', timeout=15)
finally:
    c.close()
for k,v in outs.items():
    print(f'=== {{k}} ===')
    print(v)
"""
    r = subprocess.run([sys.executable, "-c", runner],
                       capture_output=True, text=True, timeout=120)
    return {"returncode": r.returncode, "stdout": r.stdout, "stderr": r.stderr}


def parse_checks(out: str) -> dict:
    """Scan stdout for pass/fail markers."""
    result = {
        "driver_1_9":          "virtioscsi.device 1.9" in out,
        "inquiry_opened":      "Successfully opened virtioscsi.device" in out,
        "inquiry_devicequery": "NSCMD_DEVICEQUERY succeeded" in out,
        "inquiry_inquiry":     "SCSI INQUIRY succeeded" in out,
        "inquiry_capacity":    "SCSI READ CAPACITY succeeded" in out,
        # Volume-level: the RDB+SFS partition on scsi.raw surfaces as
        # volume "SCSI" when diskboot.kmod picks up our device.
        "volume_mounted":      "SCSI:" in out and "[Mounted]" in out,
        # Filesystem round-trip: write a known token and read it back.
        "fs_roundtrip":        "42-v19-fs-rt" in out,
        # list of the just-written file shows it on disk.
        "fs_listed":           "fs_rt.txt" in out,
    }
    return result


def parse_serial_log(path: str) -> dict:
    """Confirm the v1.9 correctness fixes fired in the boot log."""
    try:
        txt = open(path, "rb").read().decode("latin1", errors="replace")
    except OSError:
        return {}
    return {
        "driver_loaded":   "virtioscsi.device 1.9" in txt,
        "bar5_fix":        "BAR5 high DWORD is 0xffffffff" in txt,
        "mmio_probe_ok":   "MMIO probe OK" in txt,
        "modern_mode":     "VirtIO mode: MODERN" in txt,
        "features_lo":     "lo=0x30000006" in txt or "lo=0x20000006" in txt or "lo=0x30000004" in txt or "Driver features" in txt,
        "unit_registered": "Registered unit 0 (T0 L0)" in txt,
    }


# ----- per-machine runner -----------------------------------------------
def run_machine(key: str, driver_src: str) -> dict:
    m = MACHINES[key]
    log(f"==== {m['name']} ====")
    copy_source(m)
    update_kickstart(m, driver_src)

    cmd = build_qemu_cmd(m)
    stderr = open(os.path.join(m["workdir"], "qemu_stderr.txt"), "w")
    log(f"[{m['name']}] launching QEMU ...")
    proc = subprocess.Popen(cmd, stderr=stderr, stdout=subprocess.DEVNULL,
                            start_new_session=True)
    try:
        log(f"[{m['name']}] waiting for SerialShell on :{m['ssh_forward_port']} (max 4 min) ...")
        up = wait_for_serialshell(m["ssh_forward_port"], timeout=240)
        if not up:
            return {"machine": m["name"], "result": "BOOT_TIMEOUT",
                    "serial_log": parse_serial_log(os.path.join(m["workdir"], "serial_full.log"))}
        log(f"[{m['name']}] SerialShell up — running checks")
        checks = run_checks(m)
        parsed = parse_checks(checks.get("stdout", ""))
        sl = parse_serial_log(os.path.join(m["workdir"], "serial_full.log"))
        ok = all(parsed.values()) and sl.get("driver_loaded", False)
        return {"machine": m["name"],
                "result": "PASS" if ok else "FAIL",
                "checks": parsed, "serial_log": sl,
                "raw_stdout": checks.get("stdout", "")[-2000:],
                "raw_stderr": checks.get("stderr", "")[:500]}
    finally:
        # Quit via QEMU monitor
        try:
            with socket.create_connection(("localhost", m["monitor_port"]), timeout=2) as s:
                s.sendall(b"quit\n")
        except OSError:
            pass
        # Also try unix monitor (what we're actually using)
        try:
            import socket as sk
            u = sk.socket(sk.AF_UNIX, sk.SOCK_STREAM)
            u.connect(os.path.join(m["workdir"], "monitor.sock"))
            u.sendall(b"quit\n")
            u.close()
        except OSError:
            pass
        # Ensure death
        try:
            os.killpg(proc.pid, signal.SIGTERM)
            proc.wait(timeout=5)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        stderr.close()


# ----- main -------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--machines", default="a1,peg2,s460",
                    help="comma-separated subset of a1,peg2,s460")
    ap.add_argument("--driver", default=DRIVER_DEBUG,
                    help="path to virtioscsi.device to inject")
    args = ap.parse_args()

    keys = [k.strip() for k in args.machines.split(",") if k.strip()]
    results = {}
    for k in keys:
        if k not in MACHINES:
            log(f"skipping unknown machine: {k}")
            continue
        try:
            results[k] = run_machine(k, args.driver)
        except Exception as e:
            results[k] = {"machine": k, "result": "EXCEPTION", "error": str(e)}
        log(f"[{MACHINES[k]['name']}] => {results[k].get('result')}")

    log("============ SUMMARY ============")
    for k, r in results.items():
        log(f"{MACHINES[k]['name']:10s}  {r.get('result')}")
        for k2, v in (r.get("checks") or {}).items():
            log(f"    check: {k2:28s}  {'OK' if v else 'MISS'}")
        for k2, v in (r.get("serial_log") or {}).items():
            log(f"    serial: {k2:28s}  {'OK' if v else 'MISS'}")


if __name__ == "__main__":
    main()
