#!/usr/bin/env python3
"""逐个内置函数的 pyadamas / CPython 对比 bench。

在容器里跑（仓库挂在 /w）：

    cd /w && python3 bench/run_bench.py              # 全量
    cd /w && python3 bench/run_bench.py 13_sum 32_sorted   # 只跑指定用例
    cd /w && python3 bench/run_bench.py --repeat 5 --target 0.4

做法：
  1. 先按 CPython 的实测时间把每个用例的 N 校准到 --target 秒左右；
  2. 同一份源码分别喂给 CPython 和 pyadamas（pyadamas 走 build + 跑原生二进制，
     编译时间不计入）；
  3. 取多次运行的最小值（噪声只会往上走）；
  4. 再用 2N 跑一次 pyadamas，看耗时是不是线性——不线性就说明循环体被
     优化掉了（LICM 提出 / 常量折叠 / DCE），这时候倍数没有意义；
  5. 顺带比对两边的标准输出，确认跑的是同一件事。
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CASES_DIR = ROOT / "builtins"
WORK = Path("/tmp/pyadamasbench")
PYADAMAS_BIN = os.environ.get("PYADAMAS_BIN", "/w/build/dev/pyadamas")
PYTHON_BIN = os.environ.get("PYTHON_BIN", "python3")

# 结果本来就该不一样的用例：只比速度不比输出
SKIP_OUTPUT_CHECK = {"44_hash", "46_id"}


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def set_n(src, n):
    return re.sub(r"^N = \d+", "N = %d" % n, src, count=1, flags=re.M)


def digest(path):
    """输出摘要：行数 + 最后一行。打印型用例不会把内存吃满。"""
    data = Path(path).read_bytes()
    lines = data.split(b"\n")
    while lines and lines[-1] == b"":
        lines.pop()
    last = lines[-1].decode("utf-8", "replace") if lines else ""
    return len(lines), last


def timed_run(cmd, out_path):
    """跑一次，返回墙钟秒数；失败返回 (None, stderr)。"""
    t0 = time.perf_counter()
    with open(out_path, "wb") as f:
        p = subprocess.run(cmd, stdout=f, stderr=subprocess.PIPE)
    t1 = time.perf_counter()
    if p.returncode != 0:
        return None, p.stderr.decode("utf-8", "replace")
    return t1 - t0, ""


def measure(cmd, repeat, out_path):
    """跑 repeat 次取最小值。"""
    best = None
    err = ""
    for _ in range(repeat):
        t, e = timed_run(cmd, out_path)
        if t is None:
            return None, e
        err = e
        best = t if best is None else min(best, t)
    return best, err


def calibrate(src, name, target, work):
    """按 CPython 实测时间缩放 N。"""
    n = int(re.search(r"^N = (\d+)", src, re.M).group(1))
    best = None
    for _ in range(5):
        py = work / (name + ".py")
        py.write_text(set_n(src, n), encoding="utf-8")
        t, err = timed_run([PYTHON_BIN, str(py)], work / "calib.out")
        if t is None:
            return None, err
        if best is None or t < best[0]:
            best = (t, n)
        if t >= target * 0.7:
            return n, t
        if t > target * 3:
            # 已经偏大，往回缩一点
            n = max(1, int(n * max(0.3, target / t)))
        else:
            n = int(n * max(2.0, min(8.0, target / max(t, 1e-4))))
        if n > 500_000_000:
            break
    return best[1], best[0]


def build(pyadamas, py, binpath):
    p = sh([pyadamas, "build", "-o", str(binpath), str(py)])
    if p.returncode != 0:
        return p.stdout + p.stderr
    return None


def case_note(path):
    """用例文件第一行是 `# <名字> —— <说明>`。"""
    first = path.read_text(encoding="utf-8").splitlines()[0]
    if "——" in first:
        return first.split("——", 1)[1].strip()
    return first.lstrip("# ").strip()


def bench_case(path, target, repeat, work, baseline):
    name = path.stem
    src = path.read_text(encoding="utf-8")
    row = {"name": name, "note": case_note(path)}

    n, cal_t = calibrate(src, name, target, work)
    if n is None:
        row["error"] = "CPython 跑失败：" + cal_t.strip()[:300]
        return row
    row["n"] = n

    py = work / (name + ".py")
    py.write_text(set_n(src, n), encoding="utf-8")

    # --- CPython ---
    t_cpy, err = measure([PYTHON_BIN, str(py)], repeat, work / (name + ".cpy.out"))
    if t_cpy is None:
        row["error"] = "CPython 跑失败：" + err.strip()[:300]
        return row

    # --- pyadamas ---
    binpath = work / (name + ".bin")
    berr = build(PYADAMAS_BIN, py, binpath)
    if berr:
        row["error"] = "pyadamas 编译失败：" + berr.strip()[:500]
        return row
    t_py, err = measure([str(binpath)], repeat, work / (name + ".p23.out"))
    if t_py is None:
        row["error"] = "pyadamas 运行失败：" + err.strip()[:300]
        return row

    # --- 线性检查：2N 的耗时应该接近 2 倍 ---
    py2 = work / (name + ".2n.py")
    py2.write_text(set_n(src, n * 2), encoding="utf-8")
    bin2 = work / (name + ".2n.bin")
    berr = build(PYADAMAS_BIN, py2, bin2)
    t2 = None
    if not berr:
        t2, _ = measure([str(bin2)], max(2, repeat // 2), work / (name + ".p23b.out"))
    row["n2"] = n * 2
    row["t_pa_2n"] = t2

    # --- 输出比对 ---
    row["out_same"] = None
    if name not in SKIP_OUTPUT_CHECK:
        c = digest(work / (name + ".cpy.out"))
        p = digest(work / (name + ".p23.out"))
        row["out_same"] = c == p
        row["out_cpy"] = c[1][:120]
        row["out_p23"] = p[1][:120]

    base_c = baseline.get("cpy", 0.0)
    base_p = baseline.get("p23", 0.0)
    row["t_cpy"] = t_cpy
    row["t_pyadamas"] = t_py
    row["net_cpy"] = max(t_cpy - base_c, 1e-9)
    row["net_pyadamas"] = max(t_py - base_p, 1e-9)
    row["speedup"] = row["net_cpy"] / row["net_pyadamas"]
    row["speedup_raw"] = t_cpy / max(t_py, 1e-9)
    if t2 is not None and t_py > 0:
        row["linear"] = (t2 / max(t_py, 1e-9)) > 1.4
    return row


def main():
    ap = argparse.ArgumentParser(description="pyadamas 内置函数逐个 bench")
    ap.add_argument("cases", nargs="*", help="只跑这些用例（文件名或前缀）")
    ap.add_argument("--repeat", type=int, default=3, help="每个用例跑几次取最小值")
    ap.add_argument("--target", type=float, default=0.35, help="CPython 侧的校准目标秒数")
    ap.add_argument("--json", default="", help="把结果写到 JSON 文件")
    ap.add_argument("--md", default="", help="把结果表写成 Markdown 文件")
    ap.add_argument("--from-json", default="", help="不重跑，直接用已有的 JSON 重新生成表格")
    args = ap.parse_args()

    if not Path(PYADAMAS_BIN).exists():
        if not args.from_json:
            print("找不到 pyadamas 可执行文件：%s（用 PYADAMAS_BIN 指定）" % PYADAMAS_BIN)
            return 1
    WORK.mkdir(parents=True, exist_ok=True)

    if args.from_json:
        # 不重跑，只用已有结果重新出表
        data = json.loads(Path(args.from_json).read_text(encoding="utf-8"))
        base = data.get("baseline", {})
        rows = data.get("rows", [])
        if args.md:
            write_markdown(Path(args.md), base, rows, args)
            print("Markdown: %s" % args.md)
        return 0

    files = sorted(CASES_DIR.glob("*.py"))
    if args.cases:
        files = [f for f in files if any(c in f.stem for c in args.cases)]
    if not files:
        print("没有匹配的用例")
        return 1

    # 启动开销基线
    base_path = CASES_DIR / "00_baseline.py"
    base = {}
    if base_path.exists():
        b = bench_case(base_path, args.target, args.repeat, WORK, {"cpy": 0.0, "p23": 0.0})
        base = {"cpy": b.get("t_cpy", 0.0), "p23": b.get("t_pyadamas", 0.0)}
    print("启动基线：CPython %.2f ms，pyadamas %.2f ms" % (base["cpy"] * 1e3, base["p23"] * 1e3))
    print()

    rows = []
    for f in files:
        if f.stem == "00_baseline":
            continue
        t0 = time.perf_counter()
        row = bench_case(f, args.target, args.repeat, WORK, base)
        row["wall"] = time.perf_counter() - t0
        rows.append(row)
        if "error" in row:
            print("%-18s 跳过：%s" % (row["name"], row["error"].splitlines()[0][:120]))
        else:
            print(
                "%-18s N=%-9d CPython %8.2f ms   pyadamas %8.3f ms   净 %7.1f×  %s"
                % (
                    row["name"],
                    row["n"],
                    row["t_cpy"] * 1e3,
                    row["t_pyadamas"] * 1e3,
                    row["speedup"],
                    "" if row.get("out_same") is not False else "输出不一致!",
                )
            )

    print()
    print("| 用例 | N | CPython (ms) | pyadamas (ms) | 净加速 | 线性 |")
    print("|---|---|---|---|---|---|")
    for r in rows:
        if "error" in r:
            print("| `%s` | - | - | - | - | %s |" % (r["name"], r["error"].splitlines()[0][:80]))
            continue
        lin = "是" if r.get("linear") else "**否**"
        print(
            "| `%s` | %d | %.2f | %.3f | %.1f× | %s |"
            % (r["name"], r["n"], r["t_cpy"] * 1e3, r["t_pyadamas"] * 1e3, r["speedup"], lin)
        )

    if args.json:
        Path(args.json).write_text(
            json.dumps({"baseline": base, "rows": rows}, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
        print("\nJSON: %s" % args.json)

    if args.md:
        write_markdown(Path(args.md), base, rows, args)
        print("Markdown: %s" % args.md)
    return 0


def builtin_name(case):
    """用例名 -> 内置函数名。用例名带后缀的（print_int 等）要还原。"""
    over = {
        "01_print_int": "print",
        "02_print_str": "print",
        "48_print_sep": "print",
        "49_genexp_range": "（生成器表达式）",
    }
    if case in over:
        return over[case]
    return case.split("_", 1)[-1]


def write_markdown(path, base, rows, args):
    import datetime
    import platform

    try:
        cpy_ver = subprocess.run(
            [PYTHON_BIN, "-V"], capture_output=True, text=True
        ).stdout.strip()
    except OSError:
        cpy_ver = "unknown"
    lines = []
    lines.append("# 内置函数逐个 bench 的结果")
    lines.append("")
    lines.append("生成时间：%s" % datetime.datetime.now().strftime("%Y-%m-%d %H:%M"))
    lines.append("")
    lines.append(
        "环境：%s，%s，pyadamas 为 dev preset（`-O0` 生成代码），对手是同容器内的 %s。"
        % (platform.platform(), platform.processor() or "CPU 未识别", cpy_ver)
    )
    lines.append("")
    lines.append(
        "启动开销基线：CPython %.2f ms，pyadamas %.2f ms。表里的「净加速」已经扣掉这部分；"
        % (base.get("cpy", 0) * 1e3, base.get("p23", 0) * 1e3)
    )
    lines.append("每次取 %d 轮运行的最小值，循环次数按 CPython 实测时间校准到 %.2f 秒左右。"
                 % (args.repeat, args.target))
    lines.append("")
    lines.append("| 用例 | 内置 | 说明 | N | CPython (ms) | pyadamas (ms) | 净加速 | 线性 |")
    lines.append("|---|---|---|---|---|---|---|---|")
    for r in rows:
        if "error" in r:
            lines.append(
                "| `%s` | %s | %s | - | - | - | - | %s |"
                % (r["name"], builtin_name(r["name"]), r.get("note", ""),
                   r["error"].splitlines()[0][:60])
            )
            continue
        lin = "是" if r.get("linear") else "**否**"
        lines.append(
            "| `%s` | `%s` | %s | %d | %.2f | %.3f | %.1f× | %s |"
            % (r["name"], builtin_name(r["name"]), r.get("note", ""), r["n"],
               r["t_cpy"] * 1e3, r["t_pyadamas"] * 1e3, r["speedup"], lin)
        )
    lines.append("")
    lines.append("「线性」是用 2N 再跑一次 pyadamas 得到的：耗时没跟着翻倍，说明循环体被")
    lines.append("优化掉了（提到循环外 / 常量折叠 / 死代码删除），这时候倍数只能当")
    lines.append("「优化器收益」看，不能当「内置函数成本」看。")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
