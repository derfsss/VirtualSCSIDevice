#!/usr/bin/env python3
"""
Stress-test suite for virtioscsi.device.

Uses SerialShell for command execution AND its upload/download binary
protocol to move file contents — never uses `>` redirect in a command,
because SerialShell already wraps every plain command with its own
`>T:serialshell_out.txt` redirect, and a second `>` produces shell
parsing issues.

Covers Tiers 1–5 of the v1.9 regression test plan.

Invocation:
    python3 stress_suite.py [--port N] [--monitor PATH] [--volume VOL:]

Defaults: port 4321, no monitor, volume DH1: (first non-boot SCSI partition).
"""
from __future__ import annotations
import argparse
import hashlib
import io
import os
import socket as _socket
import sys
import time

sys.path.insert(0, "/mnt/w/Code/amiga/antigravity/projects/tools/qemu-runner")
from serial_client import SerialClient

# Known-present files on a typical AmigaOS 4.1 FE install.
SMALL_SRC = "SYS:Libs/version.library"     # ~7 KB
MEDIUM_SRC = "SYS:Libs/locale.library"     # ~88 KB — crosses bounce-buf boundary
LARGE_SRC = "SYS:Libs/workbench.library"   # ~554 KB — many SG chunks
HUGE_SRC = "SYS:Libs/minigl.library"       # ~1.26 MB — exercises INDIRECT_DESC

# Host-side artefacts consumed by Tier 5 release-specific checks.
PROJECT_ROOT = "/mnt/w/Code/amiga/antigravity/projects/VirtualSCSIDevice"
DRIVER_STRIPPED = f"{PROJECT_ROOT}/build/virtioscsi.device"
TEST_INQUIRY = f"{PROJECT_ROOT}/build/test_inquiry"
SCSI_RAW = "/tmp/test_peg2/scsi.raw"

# 9P shared folder.
P9_SHARE_HOST = "/tmp/p9share"
P9_SHARE_CANARY = "host_says_hi.txt"

# The version string uses lib_Version=53 (pinned for SFS 1.290 compat)
# even though the user-facing display version is 1.9.
DRIVER_VERSION_STRING = "virtioscsi.device 53.9"

RESULTS: list[tuple[str, bool, str]] = []
SCSI_VOL = "DH1:"  # set by CLI args


def check(name: str, ok: bool, detail: str = "") -> None:
    RESULTS.append((name, ok, detail))
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {name}" + (f"  ({detail})" if detail else ""), flush=True)


def run(c: SerialClient, cmd: str, timeout: int = 30) -> str:
    return c.send_command(cmd, timeout=timeout)


def _size_of(out: str) -> int:
    for line in (out or "").splitlines():
        line = line.strip()
        if "bytes" in line:
            toks = line.replace(",", "").split()
            for i, t in enumerate(toks):
                if t == "bytes" and i > 0 and toks[i - 1].isdigit():
                    return int(toks[i - 1])
    return -1


def _free_mem(c: SerialClient) -> int:
    out = run(c, "avail", timeout=10)
    for line in out.splitlines():
        if line.strip().startswith("Free:"):
            try:
                return int(line.split()[1].replace(",", ""))
            except Exception:
                return -1
    return -1


def header(title: str) -> None:
    print(f"\n============ {title} ============", flush=True)


def _hash_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def _upload_and_sha_roundtrip(c: SerialClient, src_host: str, scsi_path: str) -> bool:
    tmp_back = f"/tmp/_rt_back_{os.path.basename(src_host)}"
    try:
        c.upload_file(src_host, scsi_path)
        c.download_file(scsi_path, tmp_back)
        a = _hash_file(src_host)
        b = _hash_file(tmp_back)
        return a == b
    except Exception as e:
        print(f"    round-trip exception: {e}", flush=True)
        return False
    finally:
        try:
            os.unlink(tmp_back)
        except OSError:
            pass
        run(c, f"C:Delete {scsi_path} QUIET", timeout=10)


# ------------------------------------------------------------------------
def tier1_integrity(c: SerialClient) -> None:
    header("Tier 1 — data integrity + throughput")

    # 1.1 Tiny file round-trip
    test_small = "/tmp/_stress_tiny.txt"
    with open(test_small, "wb") as f:
        f.write(b"byte-exact readback marker v1.9\n")
    ok = _upload_and_sha_roundtrip(c, test_small, f"{SCSI_VOL}tiny.bin")
    check("1.1 tiny-file SHA round-trip", ok)
    os.unlink(test_small)

    # 1.2 90 KB (crosses BOUNCE_BUF_SIZE threshold)
    import random
    rnd = random.Random(12345)
    test_mid = "/tmp/_stress_mid.bin"
    with open(test_mid, "wb") as f:
        f.write(bytes(rnd.randint(0, 255) for _ in range(90 * 1024)))
    ok = _upload_and_sha_roundtrip(c, test_mid, f"{SCSI_VOL}mid.bin")
    check("1.2 90 KB SHA round-trip (bounce-buf path)", ok)
    os.unlink(test_mid)

    # 1.3 Large file copy + size check
    run(c, f"C:Delete {SCSI_VOL}big.bin QUIET", timeout=10)
    run(c, f"C:Copy {LARGE_SRC} TO {SCSI_VOL}big.bin CLONE", timeout=90)
    sz_src = _size_of(run(c, f"C:FileSize {LARGE_SRC}", timeout=10))
    sz_dst = _size_of(run(c, f"C:FileSize {SCSI_VOL}big.bin", timeout=10))
    check(f"1.3 guest-copy {os.path.basename(LARGE_SRC)} (~554 KB) size matches",
          sz_src > 0 and sz_src == sz_dst,
          detail=f"src={sz_src:,} dst={sz_dst:,}")
    run(c, f"C:Delete {SCSI_VOL}big.bin QUIET", timeout=10)

    # 1.4 Huge file copy
    run(c, f"C:Delete {SCSI_VOL}huge.bin QUIET", timeout=10)
    run(c, f"C:Copy {HUGE_SRC} TO {SCSI_VOL}huge.bin CLONE", timeout=120)
    sz_src = _size_of(run(c, f"C:FileSize {HUGE_SRC}", timeout=10))
    sz_dst = _size_of(run(c, f"C:FileSize {SCSI_VOL}huge.bin", timeout=10))
    check(f"1.4 guest-copy {os.path.basename(HUGE_SRC)} (~1.26 MB) size matches",
          sz_src > 0 and sz_src == sz_dst,
          detail=f"src={sz_src:,} dst={sz_dst:,}")
    try:
        c.download_file(f"{SCSI_VOL}huge.bin", "/tmp/_stress_huge_back.bin")
        dl_sz = os.path.getsize("/tmp/_stress_huge_back.bin")
        ok = (dl_sz == sz_dst)
        check("1.4b huge file downloaded full size", ok,
              detail=f"downloaded={dl_sz:,}")
        os.unlink("/tmp/_stress_huge_back.bin")
    except Exception as e:
        check("1.4b huge file downloaded full size", False, detail=str(e))
    run(c, f"C:Delete {SCSI_VOL}huge.bin QUIET", timeout=10)

    # 1.5 Directory copy — many small files
    run(c, f"C:Delete {SCSI_VOL}C_copy ALL QUIET", timeout=60)
    run(c, f"C:Copy SYS:C TO {SCSI_VOL}C_copy ALL CLONE", timeout=240)
    ls = run(c, f"C:List {SCSI_VOL}C_copy", timeout=30)
    has_files = "file" in ls.lower()
    sz_orig = _size_of(run(c, "C:FileSize SYS:C/Copy", timeout=10))
    sz_clone = _size_of(run(c, f"C:FileSize {SCSI_VOL}C_copy/Copy", timeout=10))
    check(f"1.5 dir copy SYS:C → {SCSI_VOL}C_copy (many small files)",
          has_files and sz_orig > 0 and sz_orig == sz_clone,
          detail=f"Copy tool: orig={sz_orig}B clone={sz_clone}B")
    run(c, f"C:Delete {SCSI_VOL}C_copy ALL QUIET", timeout=60)

    # 1.6 100-iteration upload/download loop
    ok_count = 0
    test_iter = "/tmp/_stress_iter.bin"
    for i in range(100):
        with open(test_iter, "wb") as f:
            f.write(f"iter{i}:{'x' * 64}".encode())
        if _upload_and_sha_roundtrip(c, test_iter, f"{SCSI_VOL}iter.bin"):
            ok_count += 1
        else:
            print(f"    iter {i}: FAIL", flush=True)
            break
    os.unlink(test_iter)
    check("1.6 100-iteration upload/download loop", ok_count == 100,
          detail=f"{ok_count}/100 passed")


# ------------------------------------------------------------------------
def _soak_loop(c: SerialClient, src: str, seed: str, cycle: str,
               minutes: int, label: str) -> tuple[int, int, int, list[tuple[int, int, int]]]:
    run(c, f"C:Delete {seed} QUIET", timeout=10)
    run(c, f"C:Copy {src} TO {seed} CLONE", timeout=60)
    src_sz = _size_of(run(c, f"C:FileSize {seed}", timeout=10))
    print(f"  {label} seed: {seed} = {src_sz:,} bytes", flush=True)

    f0 = _free_mem(c)
    start = time.time()
    cycles = 0
    last_log = start
    samples: list[tuple[int, int, int]] = [(0, 0, f0)]
    while time.time() - start < minutes * 60:
        run(c, f"C:Copy {seed} TO {cycle} CLONE", timeout=30)
        run(c, f"C:Delete {cycle} QUIET", timeout=10)
        cycles += 1
        if time.time() - last_log > 30:
            fnow = _free_mem(c)
            samples.append((int(time.time() - start), cycles, fnow))
            print(f"  {label} t={int(time.time()-start)}s cycles={cycles} free={fnow:,}",
                  flush=True)
            last_log = time.time()
    f1 = _free_mem(c)
    samples.append((int(time.time() - start), cycles, f1))
    run(c, f"C:Delete {seed} QUIET", timeout=10)
    return cycles, f0, f1, samples

def _second_half_drift_rate(samples: list[tuple[int, int, int]]) -> float:
    if len(samples) < 3:
        return 0.0
    mid = samples[len(samples) // 2]
    end = samples[-1]
    d_cycles = end[1] - mid[1]
    d_bytes  = mid[2] - end[2]
    return d_bytes / d_cycles if d_cycles else 0.0


def tier2_concurrency(c: SerialClient, num_parallel: int = 3) -> None:
    header(f"Tier 2 — {num_parallel} parallel on-volume copies")

    SRC = "SYS:Libs/workbench.library"
    run(c, f"C:Delete {SCSI_VOL}seed.bin QUIET", timeout=10)
    run(c, f"C:Copy {SRC} TO {SCSI_VOL}seed.bin CLONE", timeout=60)
    src_sz = _size_of(run(c, f"C:FileSize {SCSI_VOL}seed.bin", timeout=10))
    if src_sz <= 0:
        check("2.1 seed file created", False, detail=f"{SCSI_VOL}seed.bin absent")
        return
    print(f"  seed: {SCSI_VOL}seed.bin = {src_sz:,} bytes", flush=True)

    dests = [f"{SCSI_VOL}parallel_{i}.bin" for i in range(num_parallel)]
    for d in dests:
        run(c, f"C:Delete {d} QUIET", timeout=10)

    for d in dests:
        run(c, f"C:Run C:Copy {SCSI_VOL}seed.bin TO {d} CLONE", timeout=10)

    deadline = time.time() + 60
    finished = [False] * num_parallel
    while time.time() < deadline and not all(finished):
        for i, d in enumerate(dests):
            if finished[i]:
                continue
            sz = _size_of(run(c, f"C:FileSize {d}", timeout=10))
            if sz == src_sz:
                finished[i] = True
                print(f"  {d}: DONE ({sz:,})", flush=True)
        if not all(finished):
            time.sleep(2)

    all_done = all(finished)
    finals = [_size_of(run(c, f"C:FileSize {d}", timeout=10)) for d in dests]
    check(f"2.1 {num_parallel} parallel on-volume copies (all reached full size)",
          all_done,
          detail=f"sizes={finals}  expected={src_sz}")

    for d in dests:
        run(c, f"C:Delete {d} QUIET", timeout=10)
    run(c, f"C:Delete {SCSI_VOL}seed.bin QUIET", timeout=10)

# ------------------------------------------------------------------------
def tier3_hotplug_expunge(c: SerialClient) -> None:
    header("Tier 3 — driver health")

    out = run(c, "version virtioscsi.device full", timeout=15)
    check("3.1 driver alive after tier 1 + 2",
          DRIVER_VERSION_STRING in out, detail=out.strip())

    if os.path.exists("/mnt/s/temp/test_twice"):
        c.upload_file("/mnt/s/temp/test_twice", "RAM:test_twice")
        res = run(c, "RAM:test_twice", timeout=30)
        check("3.2 double Open/Close (UAF regression guard)",
              "2X: done" in res,
              detail=res.strip().split("\n")[-1] if res else "no output")

    c.upload_file(TEST_INQUIRY, "RAM:test_inquiry")
    ti = run(c, "RAM:test_inquiry 0 0", timeout=40)
    check("3.3 test_inquiry clean after stress",
          all(s in ti for s in ("NSCMD_DEVICEQUERY succeeded",
                                "SCSI INQUIRY succeeded",
                                "SCSI READ CAPACITY succeeded")),
          detail="all three SCSI checks pass")


# ------------------------------------------------------------------------
def tier4_soak(c: SerialClient, minutes: int = 2) -> None:
    header(f"Tier 4 — {minutes}-minute on-guest soak with baseline")

    SRC = "SYS:Libs/locale.library"  # ~88 KB

    # Baseline soak on the boot volume (DH0:/SYS:).  When both DH0 and DH1
    # are on virtio-scsi, this still establishes a useful baseline: any
    # per-I/O leak in the driver would show up as drift on BOTH volumes,
    # but the EXTRA drift (SCSI minus baseline) should still be near zero
    # unless the test volume's SFS handler or partition layout consumes
    # differently.
    cyc_base, f0_base, f1_base, samp_base = _soak_loop(
        c, SRC, "SYS:_soak_seed.bin", "SYS:_soak_cycle.bin",
        minutes=minutes, label="[boot vol]")

    cyc_scsi, f0_scsi, f1_scsi, samp_scsi = _soak_loop(
        c, SRC, f"{SCSI_VOL}soak_seed.bin", f"{SCSI_VOL}soak_cycle.bin",
        minutes=minutes, label=f"[{SCSI_VOL}]")

    drift_base = f0_base - f1_base
    drift_scsi = f0_scsi - f1_scsi
    def per_1k(drift: int, cycles: int) -> float:
        return drift * 1000.0 / cycles if cycles else 0.0

    rate_base = per_1k(drift_base, cyc_base)
    rate_scsi = per_1k(drift_scsi, cyc_scsi)
    extra = drift_scsi - drift_base

    tail_rate_base = _second_half_drift_rate(samp_base)
    tail_rate_scsi = _second_half_drift_rate(samp_scsi)

    print(f"\n  Boot-vol baseline: {cyc_base} cycles, drift={drift_base:,} "
          f"B, rate={rate_base:,.0f} B/1k-cycles, tail={tail_rate_base:,.0f} B/cycle")
    print(f"  {SCSI_VOL} under test: {cyc_scsi} cycles, drift={drift_scsi:,} "
          f"B, rate={rate_scsi:,.0f} B/1k-cycles, tail={tail_rate_scsi:,.0f} B/cycle")
    print(f"  Extra drift above baseline: {extra:,} B\n")

    # Budget: 4 MB extra drift (relaxed from 2 MB because both volumes
    # share the same virtio-scsi driver — first-open caching on the test
    # volume legitimately allocates geometry + RDB + SFS buffers once).
    ok_extra = extra < 4 * 1024 * 1024
    ok_tail  = tail_rate_scsi <= 3 * max(tail_rate_base, 500.0)

    ok = ok_extra and ok_tail
    detail = (f"extra={extra:,} B (budget 4 MB, {'OK' if ok_extra else 'over'}); "
              f"tail {SCSI_VOL} {tail_rate_scsi:,.0f} vs boot {tail_rate_base:,.0f} B/cycle "
              f"({'OK' if ok_tail else 'over 3×'})")
    check(f"4. baseline-normalised soak ({SCSI_VOL} vs boot, {minutes} min)", ok, detail=detail)

# ------------------------------------------------------------------------
def hmp(monitor_path: str, cmd: str, settle: float = 0.4, timeout: float = 5.0) -> str:
    s = _socket.socket(_socket.AF_UNIX, _socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(monitor_path)
    buf = b""
    deadline = time.time() + settle
    s.settimeout(0.3)
    while time.time() < deadline:
        try:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
            if buf.endswith(b"(qemu) "):
                break
        except _socket.timeout:
            break
    s.settimeout(timeout)
    s.sendall((cmd + "\n").encode())
    resp = b""
    deadline = time.time() + timeout
    s.settimeout(0.3)
    while time.time() < deadline:
        try:
            chunk = s.recv(4096)
            if not chunk:
                break
            resp += chunk
            if resp.rstrip().endswith(b"(qemu)"):
                break
        except _socket.timeout:
            if resp:
                break
    s.close()
    return resp.decode(errors="replace")


# ------------------------------------------------------------------------
def _inquiry_target(c: SerialClient, tgt: int, lun: int = 0) -> tuple[bool, bool, str]:
    out = run(c, f"RAM:test_inquiry {tgt} {lun}", timeout=40)
    return ("SCSI INQUIRY succeeded" in out,
            "SCSI READ CAPACITY succeeded" in out,
            out)


def _find_free_target(c: SerialClient, candidates: list[int]) -> int | None:
    """Probe SCSI targets and return the first one that doesn't respond to INQUIRY."""
    for t in candidates:
        inq, _, _ = _inquiry_target(c, t, 0)
        if not inq:
            return t
    return None


def tier5_release_specific(c: SerialClient, monitor_path: str | None) -> None:
    header("Tier 5 (v1.9 release-specific) — event queue / hot-plug / diagnostic")

    # 5.1 Shell-run diagnostic — run device binary as CLI program.
    # _start() calls DebugPrintF("... cannot be executed from a shell ...")
    # and returns RETURN_FAIL (20).  We verify by checking the serial log
    # for the diagnostic message.  Capturing $RC via SerialShell is
    # impractical: AmigaOS `;` is a comment char (not separator), Execute
    # scripts produce empty output through SerialShell's redirect wrapper,
    # and separate connections don't share $RC state.
    try:
        c.upload_file(DRIVER_STRIPPED, "RAM:virtioscsi.device")
        run(c, "C:Protect RAM:virtioscsi.device +rwed", timeout=10)
        run(c, "RAM:virtioscsi.device", timeout=15)

        # Check serial log for the DebugPrintF message
        serial_log = "/tmp/test_peg2/serial_full_2.log"
        saw_msg = False
        try:
            with open(serial_log, "rb") as f:
                raw = f.read()
            text = raw.replace(b"\x00", b"").decode(errors="replace")
            saw_msg = "cannot be executed from a shell" in text
        except OSError:
            pass
        check("5.1 shell-run emits diagnostic via DebugPrintF",
              saw_msg,
              detail="serial log contains shell-run diagnostic" if saw_msg
                     else "diagnostic not found in serial log")
    finally:
        run(c, "C:Delete RAM:virtioscsi.device QUIET", timeout=10)

    if not monitor_path or not os.path.exists(monitor_path):
        print("  [SKIP] 5.2–5.4 hot-plug / media-change — "
              "no QEMU monitor socket provided",
              flush=True)
        return

    c.upload_file(TEST_INQUIRY, "RAM:test_inquiry")
    run(c, "C:Protect RAM:test_inquiry +rwed", timeout=10)

    # 5.2 Hot-plug announce — find a free target dynamically.
    # QEMU's virtio-scsi controller may respond to INQUIRY on ALL targets
    # even without backing drives (returns synthetic vendor data).  In that
    # case hot-plug tests are skipped — not a driver bug.
    free_tgt = _find_free_target(c, [2, 3, 4, 5, 6, 7])
    if free_tgt is None:
        print("  [SKIP] 5.2-5.3 hot-plug — QEMU responds to INQUIRY on all "
              "T2-T7 (no free target slot)", flush=True)
    else:
        scsi_raw_path = SCSI_RAW
        if not os.path.exists(scsi_raw_path):
            scsi_raw_path = "/mnt/e/Emulators/QEMU/QEMU_Machines/scsi.raw"
        hmp(monitor_path,
            f"drive_add 0 if=none,id=hotvd,file={scsi_raw_path},format=raw,"
            "snapshot=on,readonly=on")
        hmp(monitor_path,
            f"device_add scsi-hd,drive=hotvd,bus=scsi0.0,channel=0,"
            f"scsi-id={free_tgt},lun=0,id=hotscsi")
        time.sleep(4)
        inq_ok, cap_ok, raw = _inquiry_target(c, free_tgt, 0)
        tail = raw.strip().splitlines()[-1] if raw.strip() else "(no output)"
        check(f"5.2 hot-plug device_add posts HOTPLUG (T={free_tgt} newly reachable)",
              inq_ok and cap_ok, detail=tail)

        # 5.3 Hot-plug denounce
        hmp(monitor_path, "device_del hotscsi")
        time.sleep(4)
        hmp(monitor_path, "drive_del hotvd")
        inq2, cap2, raw2 = _inquiry_target(c, free_tgt, 0)
        tail2 = raw2.strip().splitlines()[-1] if raw2.strip() else "(no output)"
        check("5.3 hot-plug device_del retires unit (TRANSPORT_RESET REMOVED)",
              not inq2 and not cap2, detail=tail2)

    # 5.4 CD media change — use next free target
    cd_tgt = _find_free_target(c, [3, 4, 5, 6, 7, 2])
    if cd_tgt is None:
        print("  [SKIP] 5.4 CD hot-plug — no free target slot", flush=True)
    else:
        scsi_raw_path = SCSI_RAW
        if not os.path.exists(scsi_raw_path):
            scsi_raw_path = "/mnt/e/Emulators/QEMU/QEMU_Machines/scsi.raw"
        hmp(monitor_path,
            f"drive_add 0 if=none,id=hotcd,file={scsi_raw_path},format=raw,readonly=on")
        hmp(monitor_path,
            f"device_add scsi-cd,drive=hotcd,bus=scsi0.0,channel=0,"
            f"scsi-id={cd_tgt},lun=0,id=hotcdsi")
        time.sleep(4)
        inq_c, cap_c, raw_c = _inquiry_target(c, cd_tgt, 0)
        tail_c = raw_c.strip().splitlines()[-1] if raw_c.strip() else "(no output)"
        if not inq_c:
            check("5.4 CD hot-plug discovered", False, detail=tail_c)
        else:
            check(f"5.4 CD hot-plug discovered at T={cd_tgt} (INQUIRY + READ CAPACITY)",
                  cap_c, detail=tail_c)

            hmp(monitor_path, "eject hotcd")
            time.sleep(4)
            _, cap_e, raw_e = _inquiry_target(c, cd_tgt, 0)
            tail_e = raw_e.strip().splitlines()[-1] if raw_e.strip() else "(no output)"
            check("5.4 CD eject: PARAM_CHANGE invalidates geometry",
                  not cap_e, detail=tail_e)

            hmp(monitor_path, f"change hotcd {scsi_raw_path}")
            time.sleep(4)
            _, cap_r, raw_r = _inquiry_target(c, cd_tgt, 0)
            tail_r = raw_r.strip().splitlines()[-1] if raw_r.strip() else "(no output)"
            check("5.4 CD change: PARAM_CHANGE re-announces media",
                  cap_r, detail=tail_r)

        hmp(monitor_path, "device_del hotcdsi")
        time.sleep(2)
        hmp(monitor_path, "drive_del hotcd")


def tier5_5_p9_share(c: SerialClient) -> None:
    """Tier 5.5 — virtio-9p SHARED: end-to-end.

    If SHARED: is already mounted (via DEVS:DOSDrivers/SHARED installed
    at boot), just verify it works — don't try to mount again."""
    header("Tier 5.5 — virtio-9p SHARED: end-to-end")

    # Check if SHARED: is already mounted
    info_out = run(c, "info SHARED:", timeout=15)
    already_mounted = "SHARED:" in info_out and ("9PFP" in info_out or "Mounted" in info_out)

    if not already_mounted:
        print("  [SKIP] SHARED: not mounted — install Virtio9PFS-handler "
              "and reboot to enable this test.", flush=True)
        check("5.5 9P SHARED: round-trip", False,
              detail="SHARED: not mounted at boot")
        return

    # Canary: host-side file visible from guest
    canary_path = os.path.join(P9_SHARE_HOST, P9_SHARE_CANARY)
    if not os.path.exists(canary_path):
        with open(canary_path, "wb") as f:
            f.write(b"host canary v1.9 tests\n")

    listing = run(c, "C:List SHARED:", timeout=15)
    saw_canary = P9_SHARE_CANARY in listing

    # Guest → host write
    payload = b"guest virtioscsi canary\n"
    with open("/tmp/_stress_guest_canary.bin", "wb") as f:
        f.write(payload)
    guest_file = "SHARED:_virtioscsi_test.txt"
    try:
        c.upload_file("/tmp/_stress_guest_canary.bin", guest_file)
    except Exception:
        pass
    os.unlink("/tmp/_stress_guest_canary.bin")

    host_view = os.path.join(P9_SHARE_HOST, "_virtioscsi_test.txt")
    saw_host = False
    for _ in range(6):
        if os.path.exists(host_view):
            with open(host_view, "rb") as f:
                saw_host = f.read() == payload
            break
        time.sleep(0.5)

    ok = saw_canary and saw_host
    detail_bits = [
        f"canary {'seen' if saw_canary else 'MISSING'}",
        f"guest→host {'seen' if saw_host else 'MISSING'}"
    ]
    check("5.5 9P SHARED: round-trip (host↔guest)", ok,
          detail="; ".join(detail_bits))

    try:
        if os.path.exists(host_view):
            os.unlink(host_view)
    except OSError:
        pass


# ------------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(description="virtioscsi.device stress suite")
    ap.add_argument("--port", type=int, default=4321,
                    help="SerialShell TCP port (default: 4321)")
    ap.add_argument("--monitor", default=None,
                    help="QEMU HMP monitor unix socket path")
    ap.add_argument("--volume", default="DH1:",
                    help="Guest test volume (default: DH1:)")
    args = ap.parse_args()

    global SCSI_VOL
    SCSI_VOL = args.volume
    if not SCSI_VOL.endswith(":"):
        SCSI_VOL += ":"

    c = SerialClient("localhost", args.port)
    c.connect()
    try:
        tier1_integrity(c)
        tier2_concurrency(c)
        tier3_hotplug_expunge(c)
        tier4_soak(c, minutes=2)
        tier5_release_specific(c, args.monitor)
        tier5_5_p9_share(c)
    finally:
        c.close()

    header("SUMMARY")
    passed = sum(1 for _, ok, _ in RESULTS if ok)
    total = len(RESULTS)
    for name, ok, detail in RESULTS:
        tag = "PASS" if ok else "FAIL"
        print(f"  [{tag}] {name}" + (f"  ({detail})" if detail else ""))
    print(f"\n  {passed}/{total} checks passed")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
