#!/usr/bin/env python3
"""Daily-corpus benchmark harness (CPython tooling, not part of the corpus).

For each bench/daily/[abcdk]*.py script:
  * pyadamas check (subset legality)
  * correctness: CPython vs pyadamas (-O2 --rebuild) stdout byte-compare (cmp)
  * timing: pinned to the first cpuset core, multi-round median, alternating
    CPython/pyadamas order per round + shuffled script order (anti-noise).
    Records CPython run, pyadamas compile, pyadamas run, compile+run, speedup.

Env: REPS (rounds, default 9), BURST (runs averaged per measurement, default 3),
     CORE (taskset core, default 24), PA (compiler path), PY (python path).
Outputs JSON next to this file and a Markdown table on stdout.
"""
import glob
import json
import os
import random
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.dirname(HERE)
PA = os.environ.get("PA", "/tmp/wt-daily/build/pyadamas")
PY = os.environ.get("PY", "python3")
REPS = int(os.environ.get("REPS", "9"))
BURST = int(os.environ.get("BURST", "3"))
CORE = os.environ.get("CORE", "24")
SCRATCH = os.environ.get("SCRATCH", "/tmp/daily-run")

ARGV = {
    "a1_cli_calc": ["add", "3", "4", "mul", "2.5", "4", "div", "10", "4"],
    "a3_text_transform": ["slug", "Hello-World", "Foo_Bar", "BazQux"],
    "a5_todo_cli": ["priority"],
}


def loadavg() -> str:
    with open("/proc/loadavg") as f:
        return f.read().split()[0:3] and " ".join(f.read().split()[:3]) if False else " ".join(open("/proc/loadavg").read().split()[:3])


def pin(cmd: list[str]) -> list[str]:
    return ["taskset", "-c", CORE] + cmd


def timed(cmd: list[str], cwd: str, burst: int) -> float:
    ts = []
    for _ in range(burst):
        t0 = time.perf_counter_ns()
        subprocess.run(cmd, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        ts.append((time.perf_counter_ns() - t0) / 1e6)
    return statistics.median(ts)


def run_capture(cmd: list[str], cwd: str) -> tuple[bytes, int]:
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return p.stdout, p.returncode


def main() -> None:
    files = sorted(glob.glob(os.path.join(CORPUS, "[abcdk]*.py")))
    names = [os.path.basename(f)[:-3] for f in files]
    os.makedirs(SCRATCH, exist_ok=True)
    la_before = loadavg()
    results = []
    # warmup (page cache / predictor), unmeasured
    for f, name in zip(files, names):
        wd = os.path.join(SCRATCH, name)
        os.makedirs(wd, exist_ok=True)
        argv = ARGV.get(name, [])
        binp = os.path.join(wd, "bin")
        subprocess.run([PA, "build", "--rebuild", "-O2", "-o", binp, f],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(pin([binp] + argv), cwd=wd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(pin([PY, f] + argv), cwd=wd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # correctness + check
    meta = {}
    for f, name in zip(files, names):
        wd = os.path.join(SCRATCH, name)
        argv = ARGV.get(name, [])
        binp = os.path.join(wd, "bin")
        chk = subprocess.run([PA, "check", f], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        cpy_out, cpy_rc = run_capture([PY, f] + argv, wd)
        bld = subprocess.run([PA, "build", "--rebuild", "-O2", "-o", binp, f],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        pa_out, pa_rc = run_capture([binp] + argv, wd)
        same = (cpy_out == pa_out) and (cpy_rc == pa_rc == 0)
        meta[name] = {
            "check_rc": chk.returncode,
            "build_rc": bld.returncode,
            "cpy_rc": cpy_rc,
            "pa_rc": pa_rc,
            "same": same,
            "known": name.startswith("k"),
        }
        if not same:
            with open(os.path.join(wd, "ref.out"), "wb") as fo:
                fo.write(cpy_out)
            with open(os.path.join(wd, "got.out"), "wb") as fo:
                fo.write(pa_out)
    # timing: shuffled order per round, alternating cpy/pa position
    samples = {name: {"cpy": [], "compile": [], "pa": []} for name in names}
    rng = random.Random(20261119)
    for rnd in range(REPS):
        order = list(zip(files, names))
        rng.shuffle(order)
        for f, name in order:
            wd = os.path.join(SCRATCH, name)
            argv = ARGV.get(name, [])
            binp = os.path.join(wd, "bin")
            cpy_cmd = pin([PY, f] + argv)
            comp_cmd = pin([PA, "build", "--rebuild", "-O2", "-o", binp, f])
            pa_cmd = pin([binp] + argv)
            if rnd % 2 == 0:
                c = timed(cpy_cmd, wd, BURST)
                k = timed(comp_cmd, wd, 1)
                p = timed(pa_cmd, wd, BURST)
            else:
                k = timed(comp_cmd, wd, 1)
                p = timed(pa_cmd, wd, BURST)
                c = timed(cpy_cmd, wd, BURST)
            samples[name]["cpy"].append(c)
            samples[name]["compile"].append(k)
            samples[name]["pa"].append(p)
    la_after = loadavg()
    for name in names:
        c = statistics.median(samples[name]["cpy"])
        k = statistics.median(samples[name]["compile"])
        p = statistics.median(samples[name]["pa"])
        kr = k + p
        m = meta[name]
        m.update({
            "cpy_run_ms": round(c, 3),
            "compile_ms": round(k, 3),
            "pa_run_ms": round(p, 3),
            "compile_run_ms": round(kr, 3),
            "speedup_run": round(c / p, 3) if p > 0 else None,
            "speedup_compile_run": round(c / kr, 3) if kr > 0 else None,
            "cpy_min_ms": round(min(samples[name]["cpy"]), 3),
            "pa_min_ms": round(min(samples[name]["pa"]), 3),
        })
        results.append({"name": name, **m})
    out = {
        "env": {
            "python": subprocess.run([PY, "--version"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.decode().strip(),
            "pa": PA,
            "core": CORE,
            "reps": REPS,
            "burst": BURST,
            "loadavg_before": la_before,
            "loadavg_after": la_after,
            "nproc": os.cpu_count(),
        },
        "results": results,
    }
    with open(os.path.join(HERE, "results.json"), "w") as fo:
        json.dump(out, fo, indent=2)
    # markdown table
    print("| script | cat | check | out | cpy run | compile | pa run | comp+run | speedup(run) | speedup(c+r) |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for r in results:
        cat = r["name"][0].upper()
        outst = "SAME" if r["same"] else ("KNOWN" if r["known"] else "DIFF")
        ck = "ok" if r["check_rc"] == 0 else "FAIL"
        print(f"| {r['name']} | {cat} | {ck} | {outst} | {r['cpy_run_ms']} | {r['compile_ms']} | {r['pa_run_ms']} | {r['compile_run_ms']} | {r['speedup_run']} | {r['speedup_compile_run']} |")
    print(f"\nenv: {out['env']}")


if __name__ == "__main__":
    main()
