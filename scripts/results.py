#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Turn raw serial-console logs from the emulated board into result tables.

    results.py crash <logdir> <out_prefix>     crash-matrix cells -> .csv + .md
    results.py bench <logfile> <out_prefix>    ringbench CSV lines -> raw/summary .csv + .md
    results.py sizes <kodir> <modsizes_log> <out_prefix>
"""
import csv
import glob
import os
import re
import statistics
import subprocess
import sys

MODULES = ["rustring", "cring", "rustring_naive", "cring_naive"]
SCENARIOS = [
    ("peek_past_data", "PEEK 5 bytes at offset 8 (only 10 stored)"),
    ("peek_oob", "PEEK 4096 bytes from a 64-byte buffer"),
    ("peek_off_overflow", "PEEK offset 2^64-1"),
    ("peek_len_overflow", "PEEK length 2^64-1"),
    ("resize_zero", "RESIZE to 0, then PEEK"),
    ("resize_huge", "RESIZE to 2^60 bytes"),
    ("resize_busy", "RESIZE while data is stored"),
    ("bad_pointers", "NULL / kernel / read-only user pointers"),
    ("fuzz", "20 000 random operations"),
]
ERRNO = {0: "ok", 11: "EAGAIN", 12: "ENOMEM", 14: "EFAULT", 16: "EBUSY", 22: "EINVAL", 25: "ENOTTY"}


def read_log(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace").replace("\r", "")


def classify(text, mod, sc):
    """Return (outcome, detail) for one crash-matrix cell."""
    if "Kernel panic - not syncing" in text:
        why = re.search(r"(range end index \d+ out of range for slice of length \d+"
                        r"|attempt to [a-z ]+ with overflow"
                        r"|attempt to calculate the remainder with a divisor of zero"
                        r"|called `Result::unwrap\(\)` on an `Err` value[^\n]*"
                        r"|Unable to handle kernel [^\n]*?at virtual address [0-9a-f]+"
                        r"|Internal error: [^\n]*)", text)
        return "KERNEL PANIC", (why.group(1).strip() if why else "")
    cell = re.search(r"CELL\[[^\]]+\]: rc=(\d+) out_bytes=(\d+) out_nonzero=(\d+) device=(\w+) stats=([^\n]*)", text)
    splat = re.search(r"SPLAT\[[^\]]+\]: kasan=(\d+) warn=(\d+) oops=(\d+) sleep=(\d+)", text)
    if not cell:
        return "NO RESULT (timeout)", ""
    rc, nbytes, nz, dev, stats = int(cell[1]), int(cell[2]), int(cell[3]), cell[4], cell[5]
    kasan, warn, oops = (int(splat[1]), int(splat[2]), int(splat[3])) if splat else (0, 0, 0)
    used = re.search(r"used (\d+)", stats)
    used = int(used[1]) if used else None

    notes = []
    if kasan:
        notes.append("KASAN report")
    if warn:
        notes.append("kernel WARNING")
    if oops:
        notes.append("Oops")
    if dev == "hung":
        notes.append("device hung")

    if sc in ("bad_pointers", "fuzz"):
        ok = rc == 0
        tail = stats.split("|")[-1].strip()
        out = "all rejected cleanly" if (ok and sc == "bad_pointers") else ("PASS" if ok else "FAIL")
        return out, "; ".join([tail] + notes) if tail else "; ".join(notes)
    if rc in (14, 16, 22, 12) and not (kasan or oops):
        out = f"{ERRNO[rc]} (rejected)"
        if warn:
            out = f"{ERRNO[rc]} + WARNING"
        return out, "; ".join(notes)
    if rc == 0:
        if sc == "resize_busy" and used == 0:
            return "ACCEPTED: stored data silently discarded", "; ".join(notes)
        if sc == "peek_oob" and nz > 10:
            return f"LEAK: {nbytes} bytes returned, {nz - 10} non-zero bytes of other kernel memory", "; ".join(notes)
        if sc.startswith("peek"):
            return f"ACCEPTED: returned {nbytes} bytes of out-of-range data", "; ".join(notes)
        return "ACCEPTED (no error)", "; ".join(notes)
    if rc == 139:
        return "Oops: caller killed", "; ".join(notes)
    return f"rc={rc}", "; ".join(notes)


def cmd_crash(logdir, prefix):
    rows = []
    for sc, desc in SCENARIOS:
        for mod in MODULES:
            path = os.path.join(logdir, f"{mod}.{sc}.log")
            if not os.path.exists(path):
                continue
            outcome, detail = classify(read_log(path), mod, sc)
            rows.append({"scenario": sc, "attack": desc, "module": mod, "outcome": outcome, "detail": detail})
    with open(prefix + ".csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["scenario", "attack", "module", "outcome", "detail"])
        w.writeheader()
        w.writerows(rows)
    with open(prefix + ".md", "w") as f:
        f.write("| Attack | " + " | ".join(f"`{m}`" for m in MODULES) + " |\n")
        f.write("|---|" + "---|" * len(MODULES) + "\n")
        for sc, desc in SCENARIOS:
            cells = []
            for mod in MODULES:
                r = next((x for x in rows if x["scenario"] == sc and x["module"] == mod), None)
                if not r:
                    cells.append("—")
                    continue
                c = f"**{r['outcome']}**" if r["outcome"].startswith(("KERNEL PANIC", "LEAK", "ACCEPTED", "Oops")) else r["outcome"]
                if r["detail"]:
                    c += f"<br><sub>{r['detail']}</sub>"
                cells.append(c)
            f.write(f"| {desc} | " + " | ".join(cells) + " |\n")
    print(open(prefix + ".md").read())


def cmd_bench(logfile, prefix):
    text = read_log(logfile)
    raw = [l.split(",")[1:] for l in text.splitlines() if l.startswith("CSV,")]
    with open(prefix + "-raw.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["kind", "dev", "chunk", "run", "ops", "bytes", "ns"])
        w.writerows(raw)

    groups = {}
    for kind, dev, chunk, run, ops, nbytes, ns in raw:
        dev = os.path.basename(dev)
        key = (kind, int(chunk))
        if kind == "lat":
            val = (float(nbytes), float(ns))          # p50, p99 in ns
        else:
            val = (int(nbytes) / int(ns) * 1e3,       # MB/s (10^6 bytes)
                   int(ns) / int(ops))                # ns per op (rw: per write+read pair)
        groups.setdefault(key, {}).setdefault(dev, []).append(val)

    def ms(xs):
        return (statistics.mean(xs), statistics.stdev(xs) if len(xs) > 1 else 0.0)

    out = []
    for (kind, chunk), devs in sorted(groups.items()):
        r = {d: [ms([v[0] for v in vs]), ms([v[1] for v in vs])] for d, vs in devs.items()}
        rust, c = r.get("rustring"), r.get("cring")
        if not (rust and c):
            continue
        if kind == "lat":
            out.append([kind, chunk, "p50 ns", f"{rust[0][0]:.0f}", f"{c[0][0]:.0f}", f"{rust[0][0] / c[0][0]:.3f}"])
            out.append([kind, chunk, "p99 ns", f"{rust[1][0]:.0f}", f"{c[1][0]:.0f}", f"{rust[1][0] / c[1][0]:.3f}"])
        else:
            out.append([kind, chunk, "MB/s", f"{rust[0][0]:.1f} ± {rust[0][1]:.1f}",
                        f"{c[0][0]:.1f} ± {c[0][1]:.1f}", f"{rust[0][0] / c[0][0]:.3f}"])
    with open(prefix + "-summary.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["kind", "chunk", "metric", "rustring", "cring", "rust/c"])
        w.writerows(out)
    names = {"rw": "write+read, 1 thread", "spsc": "producer/consumer, 2 threads", "lat": "latency, 64 B write+read"}
    with open(prefix + "-summary.md", "w") as f:
        f.write("| Workload | Chunk | Metric | `rustring` | `cring` | Rust / C |\n|---|---|---|---|---|---|\n")
        for kind, chunk, metric, a, b, ratio in out:
            f.write(f"| {names[kind]} | {chunk} B | {metric} | {a} | {b} | {ratio} |\n")
    print(open(prefix + "-summary.md").read())


def cmd_sizes(kodir, modlog, prefix):
    mem = {}
    for m in re.finditer(r"MODSIZE,(\w+),(\d+)", read_log(modlog)):
        mem[m[1]] = int(m[2])
    rows = []
    for mod in MODULES + ["rr_hello"]:
        ko = os.path.join(kodir, mod + ".ko")
        if not os.path.exists(ko):
            continue
        b = subprocess.run(["llvm-size", "--format=berkeley", ko], capture_output=True, text=True).stdout.split("\n")[1].split()
        rows.append([mod, os.path.getsize(ko), b[0], b[1], b[2], mem.get(mod, "")])
    with open(prefix + ".csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["module", "ko_file_bytes", "text", "data", "bss", "in_kernel_bytes"])
        w.writerows(rows)
    with open(prefix + ".md", "w") as f:
        f.write("| Module | `.ko` file | text | data | bss | in-kernel (`/proc/modules`) |\n|---|---|---|---|---|---|\n")
        for r in rows:
            f.write(f"| `{r[0]}` | {r[1]:,} B | {int(r[2]):,} | {int(r[3]):,} | {int(r[4]):,} | {r[5]:,} B |\n"
                    if r[5] != "" else f"| `{r[0]}` | {r[1]:,} B | {r[2]} | {r[3]} | {r[4]} | — |\n")
    print(open(prefix + ".md").read())


if __name__ == "__main__":
    {"crash": cmd_crash, "bench": cmd_bench, "sizes": cmd_sizes}[sys.argv[1]](*sys.argv[2:])
