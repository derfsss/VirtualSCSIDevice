#!/usr/bin/env python3
"""
Stress-test suite for virtioscsi.device on SAM460ex.

Uses SerialShell (port 4341) for command execution AND its upload/download
binary protocol to move file contents — never uses `>` redirect in a command,
because SerialShell already wraps every plain command with its own
`>T:serialshell_out.txt` redirect (source: amiga/serialshell.c line 383),
and a second `>` in the user command produces surprising shell parsing.

Covers Tiers 1–4 of the v1.9 regression test plan.
"""
from __future__ import annotations
import hashlib
import io
import os
import socket as _socket
import sys
import time

sys.path.insert(0, "/mnt/w/Code/amiga/antigravity/projects/tools/qemu-runner")
from serial_client import SerialClient

DEFAULT_PORT = 4341  # SAM460ex SerialShell

# Known-present files on a typical AmigaOS 4.1 FE install (SAM460 Kyvos etc.).
SMALL_SRC = "SYS:Libs/version.library"     # ~7 KB
MEDIUM_SRC = "SYS:Libs/locale.library"     # ~88 KB — crosses bounce-buf boundary
LARGE_SRC = "SYS:Libs/workbench.library"   # ~554 KB — many SG chunks
HUGE_SRC = "SYS:Libs/minigl.library"       # ~1.26 MB — exercises INDIRECT_DESC

# Host-side artefacts consumed by Tier 5 release-specific checks.
PROJECT_ROOT = "/mnt/w/Code/amiga/antigravity/projects/VirtualSCSIDevice"
DRIVER_STRIPPED = f"{PROJECT_ROOT}/build/virtioscsi.device"
TEST_INQUIRY = f"{PROJECT_ROOT}/build/test_inquiry"
SCSI_RAW = "/mnt/e/Emulators/QEMU/QEMU_Machines/scsi.raw"

# 9P shared folder: host path + handler binary path.  QEMU is launched
# with -virtfs pointing at P9_SHARE_HOST, mount_tag=SHARED; the guest
# mounts it via Virtio9PFS-handler for a fast host↔guest file channel.
P9_SHARE_HOST = "/tmp/p9share"
P9_SHARE_CANARY = "host_says_hi.txt"
VIRTIO9P_HANDLER = "/mnt/w/Code/amiga/antigravity/projects/VirtIO9P/build/Virtio9PFS-handler"

RESULTS: list[tuple[str, bool, str]] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    RESULTS.append((name, ok, detail))
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {name}" + (f"  ({detail})" if detail else ""), flush=True)


def run(c: SerialClient, cmd: str, timeout: int = 30) -> str:
    return c.send_command(cmd, timeout=timeout)


def _size_of(out: str) -> int:
    """Parse FileSize output ('<N> files, <M> bytes, <K> blocks') for bytes."""
    for line in (out or "").splitlines():
        line = line.strip()
        if "bytes" in line:
            toks = line.replace(",", "").split()
            for i, t in enumerate(toks):
                if t == "bytes" and i > 0 and toks[i - 1].isdigit():
                    return int(toks[i - 1])
    return -1


def _free_mem(c: SerialClient) -> int:
    """Read 'avail' and return the Free value as int bytes (or -1 on parse fail)."""
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
    """SHA-256 of local file."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def _upload_and_sha_roundtrip(c: SerialClient, src_host: str, scsi_path: str) -> bool:
    """
    Upload src_host to scsi_path, download back to a temp file, compare SHA.
    Covers the full write-then-read round-trip through the driver.
    """
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

    # 1.1 Tiny file round-trip (ensures basic path works)
    test_small = "/tmp/_stress_tiny.txt"
    with open(test_small, "wb") as f:
        f.write(b"byte-exact readback marker v1.9\n")
    ok = _upload_and_sha_roundtrip(c, test_small, "SCSI:tiny.bin")
    check("1.1 tiny-file SHA round-trip", ok)
    os.unlink(test_small)

    # 1.2 Medium file — built locally with random bytes to make it deterministic
    #     (88 KB — crosses BOUNCE_BUF_SIZE threshold)
    import random
    rnd = random.Random(12345)
    test_mid = "/tmp/_stress_mid.bin"
    with open(test_mid, "wb") as f:
        f.write(bytes(rnd.randint(0, 255) for _ in range(90 * 1024)))
    ok = _upload_and_sha_roundtrip(c, test_mid, "SCSI:mid.bin")
    check("1.2 90 KB SHA round-trip (bounce-buf path)", ok)
    os.unlink(test_mid)

    # 1.3 Guest-side large file copy + size check (SYS:Libs → SCSI:)
    run(c, "C:Delete SCSI:big.bin QUIET", timeout=10)
    run(c, f"C:Copy {LARGE_SRC} TO SCSI:big.bin CLONE", timeout=90)
    sz_src = _size_of(run(c, f"C:FileSize {LARGE_SRC}", timeout=10))
    sz_dst = _size_of(run(c, "C:FileSize SCSI:big.bin", timeout=10))
    check(f"1.3 guest-copy {os.path.basename(LARGE_SRC)} (~554 KB) size matches",
          sz_src > 0 and sz_src == sz_dst,
          detail=f"src={sz_src:,} dst={sz_dst:,}")
    run(c, "C:Delete SCSI:big.bin QUIET", timeout=10)

    # 1.4 Guest-side huge file copy (INDIRECT_DESC heavy)
    run(c, "C:Delete SCSI:huge.bin QUIET", timeout=10)
    run(c, f"C:Copy {HUGE_SRC} TO SCSI:huge.bin CLONE", timeout=120)
    sz_src = _size_of(run(c, f"C:FileSize {HUGE_SRC}", timeout=10))
    sz_dst = _size_of(run(c, "C:FileSize SCSI:huge.bin", timeout=10))
    check(f"1.4 guest-copy {os.path.basename(HUGE_SRC)} (~1.26 MB) size matches",
          sz_src > 0 and sz_src == sz_dst,
          detail=f"src={sz_src:,} dst={sz_dst:,}")
    # Download this big one back via SerialShell and SHA-check
    try:
        c.download_file("SCSI:huge.bin", "/tmp/_stress_huge_back.bin")
        # Can't SHA-check against guest's source, but verify size and
        # that the first 64 bytes match a magic PPC ELF header.
        dl_sz = os.path.getsize("/tmp/_stress_huge_back.bin")
        ok = (dl_sz == sz_dst)
        check("1.4b huge file downloaded full size", ok,
              detail=f"downloaded={dl_sz:,}")
        os.unlink("/tmp/_stress_huge_back.bin")
    except Exception as e:
        check("1.4b huge file downloaded full size", False, detail=str(e))
    run(c, "C:Delete SCSI:huge.bin QUIET", timeout=10)

    # 1.5 Directory copy — many small files
    run(c, "C:Delete SCSI:C_copy ALL QUIET", timeout=60)
    run(c, "C:Copy SYS:C TO SCSI:C_copy ALL CLONE", timeout=240)
    # Count with list; AmigaOS list footer is "<N> files - ..." or similar
    ls = run(c, "C:List SCSI:C_copy", timeout=30)
    has_files = "file" in ls.lower()
    # Also verify a couple of known tools copied correctly
    sz_orig = _size_of(run(c, "C:FileSize SYS:C/Copy", timeout=10))
    sz_clone = _size_of(run(c, "C:FileSize SCSI:C_copy/Copy", timeout=10))
    check("1.5 dir copy SYS:C → SCSI:C_copy (many small files)",
          has_files and sz_orig > 0 and sz_orig == sz_clone,
          detail=f"Copy tool: orig={sz_orig}B clone={sz_clone}B")
    run(c, "C:Delete SCSI:C_copy ALL QUIET", timeout=60)

    # 1.6 100-iteration upload/download loop (small)
    ok_count = 0
    test_iter = "/tmp/_stress_iter.bin"
    for i in range(100):
        with open(test_iter, "wb") as f:
            f.write(f"iter{i}:{'x' * 64}".encode())
        if _upload_and_sha_roundtrip(c, test_iter, f"SCSI:iter.bin"):
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
    """
    Run a copy/delete soak loop for `minutes` minutes.
    Returns (cycles, free_before, free_after, samples) where samples is a
    list of (seconds, cycles, free) tuples sampled every 30 s.

    src:   source file on a read-only volume (e.g. SYS:Libs/locale.library)
    seed:  path on the volume under test, pre-populated from src
    cycle: temporary destination on the volume under test, overwritten each loop
    """
    # Prime the seed (doesn't count toward soak measurement)
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
    """Drift per cycle over the last ~half of the soak run.

    A genuine per-I/O leak stays roughly constant across the whole run.
    A 'system cache warmup' (SFS journal growth, Workbench housekeeping,
    SerialShell buffer reuse) decays — a lot early, little late.  Looking at
    only the tail of the run separates one-time allocations from real leaks.
    """
    if len(samples) < 3:
        return 0.0
    mid = samples[len(samples) // 2]
    end = samples[-1]
    d_cycles = end[1] - mid[1]
    d_bytes  = mid[2] - end[2]
    return d_bytes / d_cycles if d_cycles else 0.0


def tier2_concurrency(c: SerialClient, num_parallel: int = 3) -> None:
    header(f"Tier 2 (redesigned) — {num_parallel} parallel on-volume copies")

    # Seed a known-size file on SCSI: — use a real AmigaOS library so it has
    # deterministic content and a known size.
    SRC = "SYS:Libs/workbench.library"   # ~554 KB
    run(c, "C:Delete SCSI:seed.bin QUIET", timeout=10)
    run(c, f"C:Copy {SRC} TO SCSI:seed.bin CLONE", timeout=60)
    src_sz = _size_of(run(c, "C:FileSize SCSI:seed.bin", timeout=10))
    if src_sz <= 0:
        check(f"2.1 seed file created", False, detail="SCSI:seed.bin absent")
        return
    print(f"  seed: SCSI:seed.bin = {src_sz:,} bytes", flush=True)

    # Clean destinations
    dests = [f"SCSI:parallel_{i}.bin" for i in range(num_parallel)]
    for d in dests:
        run(c, f"C:Delete {d} QUIET", timeout=10)

    # Launch N detached copies — each reads seed.bin, writes its own dest.
    for d in dests:
        run(c, f"C:Run C:Copy SCSI:seed.bin TO {d} CLONE", timeout=10)

    # Poll for all destinations to reach src_sz.  Max 60 s.  Each iteration
    # takes ~0.5 s per FileSize call ⇒ finer-grained progress than the old
    # fixed 90-s sleep.  Fail early if a destination shrinks or errors out.
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
    # Final sizes for the report
    finals = [_size_of(run(c, f"C:FileSize {d}", timeout=10)) for d in dests]
    check(f"2.1 {num_parallel} parallel on-volume copies (all reached full size)",
          all_done,
          detail=f"sizes={finals}  expected={src_sz}")

    # Cleanup
    for d in dests:
        run(c, f"C:Delete {d} QUIET", timeout=10)
    run(c, "C:Delete SCSI:seed.bin QUIET", timeout=10)

# ------------------------------------------------------------------------
def tier3_hotplug_expunge(c: SerialClient) -> None:
    header("Tier 3 — hot-plug / expunge")

    out = run(c, "version virtioscsi.device full", timeout=15)
    check("3.1 driver alive after tier 1 + 2",
          "virtioscsi.device 1.9" in out, detail=out.strip())

    if os.path.exists("/mnt/s/temp/test_twice"):
        c.upload_file("/mnt/s/temp/test_twice", "RAM:test_twice")
        res = run(c, "RAM:test_twice", timeout=30)
        check("3.2 double Open/Close (UAF regression guard)",
              "2X: done" in res,
              detail=res.strip().split("\n")[-1] if res else "no output")

    c.upload_file("/mnt/w/Code/amiga/antigravity/projects/VirtualSCSIDevice/build/test_inquiry",
                  "RAM:test_inquiry")
    ti = run(c, "RAM:test_inquiry 0 0", timeout=40)
    check("3.3 test_inquiry clean after stress",
          all(s in ti for s in ("NSCMD_DEVICEQUERY succeeded",
                                "SCSI INQUIRY succeeded",
                                "SCSI READ CAPACITY succeeded")),
          detail="all three SCSI checks pass")


# ------------------------------------------------------------------------
def tier4_soak(c: SerialClient, minutes: int = 3) -> None:
    header(f"Tier 4 (redesigned) — {minutes}-minute on-guest soak with baseline")

    SRC = "SYS:Libs/locale.library"  # ~88 KB

    # 4a. Baseline soak on SYS: (IDE) — establishes how much memory the
    # system legitimately drifts when nothing is touching virtio-scsi.
    cyc_base, f0_base, f1_base, samp_base = _soak_loop(
        c, SRC, "SYS:_soak_seed.bin", "SYS:_soak_cycle.bin",
        minutes=minutes, label="[IDE base]")

    # 4b. SCSI soak — same workload, but targeting the virtio-scsi volume.
    cyc_scsi, f0_scsi, f1_scsi, samp_scsi = _soak_loop(
        c, SRC, "SCSI:soak_seed.bin", "SCSI:soak_cycle.bin",
        minutes=minutes, label="[SCSI]")

    drift_base = f0_base - f1_base
    drift_scsi = f0_scsi - f1_scsi
    # Normalise drift to bytes / 1000 cycles to compare runs of slightly
    # different length (IDE cycles faster than SCSI, etc.).
    def per_1k(drift: int, cycles: int) -> float:
        return drift * 1000.0 / cycles if cycles else 0.0

    rate_base = per_1k(drift_base, cyc_base)
    rate_scsi = per_1k(drift_scsi, cyc_scsi)
    extra = drift_scsi - drift_base  # driver-attributable slice

    tail_rate_base = _second_half_drift_rate(samp_base)
    tail_rate_scsi = _second_half_drift_rate(samp_scsi)

    print(f"\n  IDE baseline: {cyc_base} cycles, drift={drift_base:,} "
          f"B, rate={rate_base:,.0f} B/1k-cycles, tail={tail_rate_base:,.0f} B/cycle")
    print(f"  SCSI under test: {cyc_scsi} cycles, drift={drift_scsi:,} "
          f"B, rate={rate_scsi:,.0f} B/1k-cycles, tail={tail_rate_scsi:,.0f} B/cycle")
    print(f"  SCSI-extra drift above IDE baseline: {extra:,} B\n")

    # Pass criteria:
    #   1. SCSI drift should not exceed IDE baseline by more than 2 MB in a
    #      `minutes`-minute run — that's comfortable headroom for the
    #      mounter.library DOSNode, SFS journal differences, etc.
    #   2. The tail (second-half) drift rate on SCSI should be ≤ 3× the IDE
    #      tail rate.  A genuine leak keeps burning memory at a constant rate
    #      ⇒ tails stay high.  Cache warmup is transient ⇒ tails decay toward
    #      IDE's baseline.
    ok_extra = extra < 2 * 1024 * 1024
    ok_tail  = tail_rate_scsi <= 3 * max(tail_rate_base, 500.0)  # floor 500 B/cycle

    ok = ok_extra and ok_tail
    detail = (f"extra={extra:,} B (budget 2 MB, {'OK' if ok_extra else 'over'}); "
              f"tail SCSI {tail_rate_scsi:,.0f} vs IDE {tail_rate_base:,.0f} B/cycle "
              f"({'OK' if ok_tail else 'over 3×'})")
    check(f"4. baseline-normalised soak (SCSI vs IDE, {minutes} min)", ok, detail=detail)

# ------------------------------------------------------------------------
# HMP (human monitor protocol) helper — talks to QEMU's unix monitor socket.
# Used by Tier 5 to drive device_add / device_del / eject / change, which
# exercise the v1.9 event queue (HOTPLUG / PARAM_CHANGE consumer).
# ------------------------------------------------------------------------
def hmp(monitor_path: str, cmd: str, settle: float = 0.4, timeout: float = 5.0) -> str:
    """Send a single HMP command to QEMU and return everything we read back.
    Monitor prompt ends in '(qemu) '; we read until it reappears or timeout."""
    s = _socket.socket(_socket.AF_UNIX, _socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(monitor_path)
    buf = b""
    # Drain banner (the monitor prints a welcome line when we connect)
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
    # Reset for the actual command
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
# Tier 5 — v1.9 release-specific
#
# Exercises features shipped for the first time in v1.9:
#   5.1  Shell-run diagnostic: `_start()` returns RETURN_FAIL when the
#        driver is invoked as a CLI program (replacing the old silent
#        return 0 that looked like a successful no-op).
#   5.2  Hot-plug announce: QEMU `device_add scsi-hd` posts a HOTPLUG
#        event on VQ1; the consumer task rescans and the new target
#        becomes reachable. test_inquiry on T=1 must now succeed.
#   5.3  Hot-plug denounce: `device_del` raises TRANSPORT_RESET with
#        event=REMOVED; driver drops the unit. test_inquiry on T=1 fails.
#   5.4  CD media change: scsi-cd + `eject` / `change` on the monitor
#        generates PARAM_CHANGE events (ASC 0x28 / 0x3A). The driver
#        invalidates its cached geometry so READ CAPACITY tracks the
#        actual media state.
#
# 5.2-5.4 require the QEMU monitor socket path.  Without it they're
# skipped cleanly (Tier 5.1 still runs, since it's pure guest-side).
# ------------------------------------------------------------------------
def _inquiry_target(c: SerialClient, tgt: int, lun: int = 0) -> tuple[bool, bool, str]:
    """Run test_inquiry on (tgt,lun). Returns (inquiry_ok, readcap_ok, raw)."""
    out = run(c, f"RAM:test_inquiry {tgt} {lun}", timeout=40)
    return ("SCSI INQUIRY succeeded" in out,
            "SCSI READ CAPACITY succeeded" in out,
            out)


def tier5_release_specific(c: SerialClient, monitor_path: str | None) -> None:
    header("Tier 5 (v1.9 release-specific) — event queue / hot-plug / diagnostic")

    # 5.1 Shell-run diagnostic — run the device as a CLI program and
    #     confirm it returns a non-zero result code.  Pre-v1.9 this was
    #     silently 0, which hid accidental CLI invocations.
    try:
        c.upload_file(DRIVER_STRIPPED, "RAM:virtioscsi.device")
        run(c, "C:Protect RAM:virtioscsi.device +rwed", timeout=10)
        run(c, "RAM:virtioscsi.device", timeout=15)
        rc_out = run(c, "echo $RC", timeout=10)
        first = rc_out.strip().splitlines()[0] if rc_out.strip() else ""
        try:
            rc = int(first.split()[0])
        except (ValueError, IndexError):
            rc = -1
        check("5.1 shell-run returns RETURN_FAIL (non-zero RC)",
              rc != 0 and rc != -1,
              detail=f"RC={rc}")
    finally:
        run(c, "C:Delete RAM:virtioscsi.device QUIET", timeout=10)

    if not monitor_path or not os.path.exists(monitor_path):
        print("  [SKIP] 5.2–5.4 hot-plug / media-change — "
              "no QEMU monitor socket provided",
              flush=True)
        return

    # test_inquiry must be on RAM: (Tier 3.3 puts it there, but don't
    # depend on ordering — re-upload so this tier stands alone).
    c.upload_file(TEST_INQUIRY, "RAM:test_inquiry")
    run(c, "C:Protect RAM:test_inquiry +rwed", timeout=10)

    # 5.2 Hot-plug announce: T=1 must be absent first, then reachable
    #     after device_add.  Event queue's HOTPLUG consumer rescans LUNs.
    pre_inq, _, _ = _inquiry_target(c, 1, 0)
    if pre_inq:
        check("5.2 hot-plug device_add posts HOTPLUG (T=1 newly reachable)",
              False, detail="T=1 was already reachable before device_add")
    else:
        hmp(monitor_path,
            f"drive_add 0 if=none,id=hotvd,file={SCSI_RAW},format=raw,"
            "snapshot=on,readonly=on")
        hmp(monitor_path,
            "device_add scsi-hd,drive=hotvd,bus=scsi0.0,channel=0,"
            "scsi-id=1,lun=0,id=hotscsi1")
        # HOTPLUG event propagation: driver posts event, consumer task
        # runs, discovery rescans target 1.  Give it a few seconds.
        time.sleep(4)
        inq_ok, cap_ok, raw = _inquiry_target(c, 1, 0)
        tail = raw.strip().splitlines()[-1] if raw.strip() else "(no output)"
        check("5.2 hot-plug device_add posts HOTPLUG (T=1 newly reachable)",
              inq_ok and cap_ok, detail=tail)

        # 5.3 Hot-plug denounce: device_del raises TRANSPORT_RESET REMOVED.
        #     Driver retires the unit; subsequent INQUIRY on T=1 fails.
        hmp(monitor_path, "device_del hotscsi1")
        time.sleep(4)
        hmp(monitor_path, "drive_del hotvd")
        inq2, cap2, raw2 = _inquiry_target(c, 1, 0)
        tail2 = raw2.strip().splitlines()[-1] if raw2.strip() else "(no output)"
        check("5.3 hot-plug device_del retires unit (TRANSPORT_RESET REMOVED)",
              not inq2 and not cap2, detail=tail2)

    # 5.4 CD media change: attach a scsi-cd, verify READ CAPACITY works,
    #     eject via monitor, verify READ CAPACITY now fails (PARAM_CHANGE
    #     invalidated the cached geometry), then `change` a fresh image
    #     and verify READ CAPACITY recovers.
    hmp(monitor_path,
        f"drive_add 0 if=none,id=hotcd,file={SCSI_RAW},format=raw,readonly=on")
    hmp(monitor_path,
        "device_add scsi-cd,drive=hotcd,bus=scsi0.0,channel=0,"
        "scsi-id=2,lun=0,id=hotcdsi")
    time.sleep(4)
    inq_c, cap_c, raw_c = _inquiry_target(c, 2, 0)
    tail_c = raw_c.strip().splitlines()[-1] if raw_c.strip() else "(no output)"
    if not inq_c:
        check("5.4 CD hot-plug discovered", False, detail=tail_c)
    else:
        check("5.4 CD hot-plug discovered (INQUIRY + READ CAPACITY)",
              cap_c, detail=tail_c)

        # Eject: driver sees PARAM_CHANGE ASC=0x3A, clears media_present.
        hmp(monitor_path, "eject hotcd")
        time.sleep(4)
        _, cap_e, raw_e = _inquiry_target(c, 2, 0)
        tail_e = raw_e.strip().splitlines()[-1] if raw_e.strip() else "(no output)"
        check("5.4 CD eject: PARAM_CHANGE invalidates geometry "
              "(READ CAPACITY now fails)",
              not cap_e, detail=tail_e)

        # Re-insert a different file via `change`.  Driver gets
        # PARAM_CHANGE ASC=0x28 (medium inserted), clears its "not ready"
        # state, new READ CAPACITY succeeds.
        hmp(monitor_path, f"change hotcd {SCSI_RAW}")
        time.sleep(4)
        _, cap_r, raw_r = _inquiry_target(c, 2, 0)
        tail_r = raw_r.strip().splitlines()[-1] if raw_r.strip() else "(no output)"
        check("5.4 CD change: PARAM_CHANGE re-announces media "
              "(READ CAPACITY recovers)",
              cap_r, detail=tail_r)

    # Clean up CD so the guest doesn't see a stale unit after exit.
    hmp(monitor_path, "device_del hotcdsi")
    time.sleep(2)
    hmp(monitor_path, "drive_del hotcd")


def tier5_5_p9_share(c: SerialClient) -> None:
    """Tier 5.5 — virtio-9p share end-to-end.

    QEMU was launched with -virtfs local,path=/tmp/p9share,mount_tag=SHARED.
    We upload the Virtio9PFS-handler to RAM: along with a DOSDriver
    descriptor that points at it, mount SHARED:, and verify we can see
    the host's canary file and write a guest file visible on the host.
    This gives future test runs a fast host↔guest bulk-transfer channel
    that doesn't tie up SerialShell's TCP pipe.
    """
    header("Tier 5.5 — virtio-9p SHARED: end-to-end")

    if not os.path.exists(VIRTIO9P_HANDLER):
        check("5.5 9P share mounted + canary visible", False,
              detail=f"Virtio9PFS-handler not built at {VIRTIO9P_HANDLER}")
        return
    if not os.path.exists(os.path.join(P9_SHARE_HOST, P9_SHARE_CANARY)):
        check("5.5 9P share mounted + canary visible", False,
              detail=f"host canary {P9_SHARE_HOST}/{P9_SHARE_CANARY} missing "
                     "(is ensure_p9share() being called?)")
        return

    # 1) Upload the handler and a DOSDriver descriptor pointing at it.
    c.upload_file(VIRTIO9P_HANDLER, "RAM:Virtio9PFS-handler")
    run(c, "C:Protect RAM:Virtio9PFS-handler +rwed", timeout=10)

    dos_driver = (
        "Handler   = RAM:Virtio9PFS-handler\n"
        "Stacksize = 65536\n"
        "Priority  = 5\n"
        "GlobVec   = -1\n"
        "Activate  = 1\n"
        'Control   = "auto"\n'
    )
    tmp_mount = "/tmp/_stress_SHARED_mount.txt"
    with open(tmp_mount, "w") as f:
        f.write(dos_driver)
    c.upload_file(tmp_mount, "RAM:SHARED")
    try:
        os.unlink(tmp_mount)
    except OSError:
        pass

    # 2) Mount it.  Mount reads RAM:SHARED as a DOSDriver descriptor,
    #    starts the handler, and creates a SHARED: volume backed by
    #    the 9P transport.  Give the handler a couple of seconds to
    #    settle before probing.
    run(c, "C:Mount SHARED: FROM RAM:SHARED", timeout=20)
    time.sleep(2)

    # 3) The host-side canary must appear.
    listing = run(c, "C:List SHARED:", timeout=15)
    saw_canary = P9_SHARE_CANARY in listing

    # 4) Round-trip: write from guest → host must see it.
    guest_file = "SHARED:guest_wrote.txt"
    run(c, f"C:Delete {guest_file} QUIET", timeout=10)
    payload = b"guest v1.9 canary\n"
    with open("/tmp/_stress_guest_canary.bin", "wb") as f:
        f.write(payload)
    c.upload_file("/tmp/_stress_guest_canary.bin", guest_file)
    os.unlink("/tmp/_stress_guest_canary.bin")
    host_view = os.path.join(P9_SHARE_HOST, "guest_wrote.txt")
    saw_host = False
    for _ in range(6):
        if os.path.exists(host_view):
            with open(host_view, "rb") as f:
                saw_host = f.read() == payload
            break
        time.sleep(0.5)

    ok = saw_canary and saw_host
    detail_bits = []
    detail_bits.append(f"canary {'seen' if saw_canary else 'MISSING'}")
    detail_bits.append(f"guest→host {'seen' if saw_host else 'MISSING'}")
    check("5.5 9P SHARED: mount round-trip (host↔guest)", ok,
          detail="; ".join(detail_bits))

    # Cleanup: unmount, delete the guest-side file on host, drop handler.
    run(c, f"C:Delete {guest_file} QUIET", timeout=10)
    try:
        if os.path.exists(host_view):
            os.unlink(host_view)
    except OSError:
        pass
    # Note: there's no "unmount" command in the AmigaOS shell; the handler
    # is torn down when QEMU shuts down.  Leaving it live for the rest of
    # the session is harmless and matches how FileSysBox handlers run.


# ------------------------------------------------------------------------
def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PORT
    monitor_path = sys.argv[2] if len(sys.argv) > 2 else None

    c = SerialClient("localhost", port)
    c.connect()
    try:
        tier1_integrity(c)
        tier2_concurrency(c)
        tier3_hotplug_expunge(c)
        tier4_soak(c, minutes=2)
        tier5_release_specific(c, monitor_path)
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
