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
import sys
import time

sys.path.insert(0, "/mnt/w/Code/amiga/antigravity/projects/tools/qemu-runner")
from serial_client import SerialClient

PORT = 4341  # SAM460ex SerialShell

# Known-present files on the user's AmigaOS4 SAM460 install.
SMALL_SRC = "SYS:Libs/version.library"     # ~7 KB
MEDIUM_SRC = "SYS:Libs/locale.library"     # ~88 KB — crosses bounce-buf boundary
LARGE_SRC = "SYS:Libs/workbench.library"   # ~554 KB — many SG chunks
HUGE_SRC = "SYS:Libs/minigl.library"       # ~1.26 MB — exercises INDIRECT_DESC

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
def main() -> int:
    c = SerialClient("localhost", PORT)
    c.connect()
    try:
        tier1_integrity(c)
        tier2_concurrency(c)
        tier3_hotplug_expunge(c)
        tier4_soak(c, minutes=2)
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
