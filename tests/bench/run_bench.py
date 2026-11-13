#!/usr/bin/env python3
"""标准库基准：同一份用例分别交给 pyadamas 与 CPython 运行，取最优耗时。

用法（在仓库根目录）：
    python3 tests/bench/run_bench.py --pyadamas build/dev/pyadamas
可选：
    --repeat N     每个程序跑 N 次取最小值（默认 3）
    --filter STR   只跑文件名包含 STR 的用例
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CASES = Path(__file__).resolve().parent / "cases"


def run_once(cmd):
    t0 = time.perf_counter()
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    dt = time.perf_counter() - t0
    if r.returncode != 0:
        raise RuntimeError(r.stderr.decode("utf-8", "replace").strip()[:300] or
                           "exit %d" % r.returncode)
    return dt


def best_of(cmd, repeat):
    return min(run_once(cmd) for _ in range(repeat))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pyadamas", default=str(ROOT / "build/dev/pyadamas"))
    ap.add_argument("--cpython", default=sys.executable)
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--filter", default="")
    args = ap.parse_args()

    cases = sorted(CASES.glob("*.py"))
    if args.filter:
        cases = [c for c in cases if args.filter in c.name]
    if not cases:
        print("没有匹配的用例")
        return 1

    outdir = tempfile.mkdtemp(prefix="pa_bench_")
    print(f"{'case':22} {'pyadamas':>10} {'cpython':>10} {'speedup':>9}")
    print("-" * 55)
    failed = 0
    for case in cases:
        prog = os.path.join(outdir, case.stem)
        try:
            subprocess.run([args.pyadamas, "build", "-o", prog, str(case)],
                           check=True, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE)
            tp = best_of([prog], args.repeat)
            tc = best_of([args.cpython, str(case)], args.repeat)
        except Exception as e:  # noqa: BLE001
            failed += 1
            print(f"{case.stem:22} FAILED: {e}")
            continue
        speedup = tc / tp if tp > 0 else 0.0
        print(f"{case.stem:22} {tp * 1000:9.1f}ms {tc * 1000:9.1f}ms {speedup:8.2f}x")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
