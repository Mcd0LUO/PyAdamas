#!/usr/bin/env python3
"""rebench.py —— re 基准的 A/B 驱动（交替 + 奇偶轮交换 + 绑核 + 多轮中位）。

bench/re/bench_re.py 一次跑完所有用例并逐行打印 "用例 ms 计数"。本脚本用
两个编译器各编译一份该基准，交替运行、取每个用例的中位数，给出变化百分比。

  python3 tools/rebench.py --base build/base/pyadamas --cur build/pyadamas
  python3 tools/rebench.py --base /tmp/wt-re-base/build/pyadamas \
      --cur /tmp/wt-re/build/pyadamas --reps 9 --case findall_class

注意：
  - 两侧都必须用各自编译器 --rebuild 生成可执行文件（modcache 的键不含
    编译器版本，不加 --rebuild 会命中旧产物、量出完全相同的二进制）；
  - 绑定到 cpuset 里第一个可用核（多租户机器上不绑核方差 ±20%）；
  - 交替 + 奇偶轮交换顺序，抵消系统漂移；报中位数与 p25。
"""
import argparse
import os
import random
import statistics
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH = os.path.join(ROOT, "bench", "re", "bench_re.py")
OUT = "/tmp/pyadamas-rebench"


def pin_cpu() -> str:
    with open("/proc/self/status") as f:
        for line in f:
            if line.startswith("Cpus_allowed_list:"):
                first = line.split()[1].split(",")[0]
                return first.split("-")[0]
    return "0"


def build(binp: str, src: str, out: str) -> None:
    r = subprocess.run([binp, "build", "-O2", "--rebuild", src, "-o", out],
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if r.returncode != 0:
        sys.exit("build failed: %s\n%s" % (out, r.stderr.decode()[:500]))


def run(execu: str, cpu: str) -> dict:
    r = subprocess.run(["taskset", "-c", cpu, execu], capture_output=True, timeout=600)
    out = {}
    for line in r.stdout.decode().splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        try:
            out[parts[0]] = float(parts[1])
        except ValueError:
            continue
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True, help="基线 pyadamas 可执行文件")
    ap.add_argument("--cur", required=True, help="当前 pyadamas 可执行文件")
    ap.add_argument("--reps", type=int, default=7, help="交替轮数（默认 7）")
    ap.add_argument("--case", action="append", default=None,
                    help="只比较指定用例（可重复；默认全部）")
    args = ap.parse_args()

    os.makedirs(OUT, exist_ok=True)
    build(args.base, BENCH, OUT + "/a")
    build(args.cur, BENCH, OUT + "/b")
    cpu = pin_cpu()

    # 正确性闸门：用例内打印的匹配计数必须一致（耗时列不比较）。
    def counts(exe):
        return {line.rsplit(" ", 2)[0]: line.rsplit(" ", 1)[1]
                for line in subprocess.run(["taskset", "-c", cpu, exe],
                                           capture_output=True).stdout.decode().splitlines()
                if len(line.split()) == 3}

    if counts(OUT + "/a") != counts(OUT + "/b"):
        sys.exit("两侧输出不一致：先修正确性，再谈性能")

    res = {}
    for rep in range(args.reps):
        order = ["a", "b"] if rep % 2 == 0 else ["b", "a"]
        for side in order:
            got = run(OUT + "/" + side, cpu)
            for name, ms in got.items():
                res.setdefault(name, {"a": [], "b": []})[side].append(ms)

    cases = args.case or sorted(res)
    total_a = total_b = 0.0
    print("%-22s %10s %10s %9s %9s" % ("case", "base", "cur", "delta", "p25d"))
    for name in cases:
        a = sorted(res[name]["a"])
        b = sorted(res[name]["b"])
        am, bm = statistics.median(a), statistics.median(b)
        ap, bp = a[len(a) // 4], b[len(b) // 4]
        total_a += am
        total_b += bm
        print("%-22s %9.3fms %9.3fms %+8.1f%% %+8.1f%%"
              % (name, am, bm, (bm - am) * 100 / am, (bp - ap) * 100 / ap))
    print("%-22s %9.3fms %9.3fms %+8.1f%%"
          % ("TOTAL", total_a, total_b, (total_b - total_a) * 100 / total_a))


if __name__ == "__main__":
    main()
