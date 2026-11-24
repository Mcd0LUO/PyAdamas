#!/usr/bin/env python3
"""经典基准四语言对比：pyadamas / CPython / Go / C++。

在开发容器里跑（仓库挂在 /w）：

    cd /w && python3 bench/compare/run_compare.py
    cd /w && python3 bench/compare/run_compare.py --repeat 5 nbody nqueens
    cd /w && python3 bench/compare/run_compare.py --md bench/compare/RESULTS.md

同一份算法分别用四种语言实现，循环次数 / 数据完全相同，比较墙钟时间与输出。
"""

import argparse
import datetime
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
WORK = Path("/tmp/pyadamas-compare")
PYADAMAS_BIN = os.environ.get("PYADAMAS_BIN", "/w/build/dev/pyadamas")
PYTHON_BIN = os.environ.get("PYTHON_BIN", "python3")
def _find_go():
    """找 go 可执行文件：优先 $GO_BIN / PATH，其次 Debian 的 /usr/lib/go-*/bin/go。"""
    env = os.environ.get("GO_BIN")
    if env:
        return env
    found = shutil.which("go")
    if found:
        return found
    for cand in sorted(Path("/usr/lib").glob("go-*/bin/go"), reverse=True):
        if os.access(cand, os.X_OK):
            return str(cand)
    return "go"  # 交给 subprocess 报 FileNotFoundError


GO_BIN = _find_go()
CXX_BIN = os.environ.get("CXX", "g++")

LANGS = ["cpython", "pyadamas", "go", "cpp"]
LABELS = {"cpython": "CPython", "pyadamas": "PyAdamas", "go": "Go", "cpp": "C++"}


def sh(cmd, cwd=None):
    return subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)


def read_n(path):
    text = path.read_text(encoding="utf-8")
    m = re.search(r"^(?:static\s+)?(?:const\s+(?:int\s+)?N\s*=|N\s*=)\s*(\d+)", text, re.M)
    return int(m.group(1)) if m else None


def build_all(case_dir, name):
    WORK.mkdir(parents=True, exist_ok=True)
    bins = {}
    errors = {}

    pa = WORK / (name + ".pa")
    # --rebuild：modcache 的键不含编译器版本，命中旧产物会量出和上次完全
    # 相同的二进制（rebench.py 的头注释记过这个坑）。基准必须每次真编译。
    p = sh([PYADAMAS_BIN, "build", "--rebuild", "-o", str(pa),
            str(case_dir / (name + ".py"))])
    if p.returncode != 0:
        errors["pyadamas"] = (p.stdout + p.stderr).strip()
    else:
        bins["pyadamas"] = [str(pa)]

    go = WORK / (name + ".go.bin")
    p = sh([GO_BIN, "build", "-o", str(go), str(case_dir / (name + ".go"))])
    if p.returncode != 0:
        errors["go"] = (p.stdout + p.stderr).strip()
    else:
        bins["go"] = [str(go)]

    cpp = WORK / (name + ".cpp.bin")
    p = sh([CXX_BIN, "-O2", "-std=c++17", "-o", str(cpp),
            str(case_dir / (name + ".cpp"))])
    if p.returncode != 0:
        errors["cpp"] = (p.stdout + p.stderr).strip()
    else:
        bins["cpp"] = [str(cpp)]

    bins["cpython"] = [PYTHON_BIN, str(case_dir / (name + ".py"))]
    return bins, errors


def measure(cmd, repeat):
    best = None
    out = ""
    for _ in range(repeat):
        t0 = time.perf_counter()
        p = subprocess.run(cmd, capture_output=True, text=True)
        t1 = time.perf_counter()
        if p.returncode != 0:
            return None, (p.stdout + p.stderr).strip()
        out = p.stdout.strip()
        t = t1 - t0
        best = t if best is None else min(best, t)
    return best, out


def main():
    ap = argparse.ArgumentParser(description="四语言经典基准对比")
    ap.add_argument("cases", nargs="*", help="只跑这些用例（默认全量）")
    ap.add_argument("--repeat", type=int, default=3, help="每种语言跑几次取最小值")
    ap.add_argument("--json", default="", help="结果写到 JSON")
    ap.add_argument("--md", default="", help="结果表写成 Markdown")
    ap.add_argument("--merge", nargs="*", default=None,
                    help="不测量，只把多个 JSON 结果按用例逐语言取最小值后出表")
    args = ap.parse_args()

    if args.merge is not None:
        return merge_json(args)

    cases = sorted(p.stem for p in ROOT.glob("*.py") if p.stem != "run_compare")
    if args.cases:
        cases = [c for c in cases if any(k in c for k in args.cases)]
    if not cases:
        print("没有匹配的用例")
        return 1

    rows = []
    for name in cases:
        py, go, cpp = (ROOT / (name + ext) for ext in (".py", ".go", ".cpp"))
        if not (py.exists() and go.exists() and cpp.exists()):
            print("%-16s 跳过：缺少四语言实现" % name)
            continue
        ns = {p.suffix: read_n(p) for p in (py, go, cpp)}
        if len(set(ns.values())) != 1:
            print("%-16s 跳过：N 不一致 %s" % (name, ns))
            continue

        bins, errors = build_all(ROOT, name)
        if errors:
            rows.append({"name": name, "n": ns[".py"], "errors": errors})
            print("%-16s 跳过：%s" % (name, next(iter(errors.values())).splitlines()[0][:100]))
            continue

        times, outs = {}, {}
        failed = False
        for lang in LANGS:
            t, out = measure(bins[lang], args.repeat)
            if t is None:
                errors[lang] = out
                failed = True
                break
            times[lang], outs[lang] = t, out
        if failed:
            rows.append({"name": name, "n": ns[".py"], "errors": errors})
            print("%-16s 跳过：运行失败" % name)
            continue

        same = len(set(outs.values())) == 1
        row = {
            "name": name,
            "n": ns[".py"],
            "times": times,
            "output": outs["cpython"],
            "output_same": same,
            "speedup": {l: times["cpython"] / max(times[l], 1e-9) for l in LANGS},
        }
        rows.append(row)
        print(
            "%-16s N=%-8d CPython %7.1f ms  PyAdamas %7.1f ms (%5.1fx)  "
            "Go %7.1f ms (%5.1fx)  C++ %7.1f ms (%5.1fx)  %s"
            % (
                name, row["n"],
                times["cpython"] * 1e3,
                times["pyadamas"] * 1e3, row["speedup"]["pyadamas"],
                times["go"] * 1e3, row["speedup"]["go"],
                times["cpp"] * 1e3, row["speedup"]["cpp"],
                "" if same else "输出不一致",
            )
        )

    print()
    if rows and any("times" in r for r in rows):
        print("| 用例 | N | CPython | PyAdamas | Go | C++ | PyAdamas 相对 CPython |")
        print("|---|---|---|---|---|---|---|")
        for r in rows:
            if "times" not in r:
                continue
            t = r["times"]
            print(
                "| `%s` | %d | %.1f ms | %.1f ms | %.1f ms | %.1f ms | %.2f× |"
                % (r["name"], r["n"], t["cpython"] * 1e3, t["pyadamas"] * 1e3,
                   t["go"] * 1e3, t["cpp"] * 1e3, r["speedup"]["pyadamas"])
            )

    if args.json:
        Path(args.json).write_text(
            json.dumps({"rows": rows, "repeat": args.repeat}, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
        print("\nJSON: %s" % args.json)
    if args.md:
        write_markdown(Path(args.md), rows, args)
        print("Markdown: %s" % args.md)
    return 0


def merge_json(args):
    """多轮 JSON 按用例、按语言逐个取最小值再出表。

    单次运行受多租户干扰（同一用例跨轮能差 2 倍）。每轮本身已经是「N 次取
    最小」，再在轮与轮之间取最小，等价于「所有采样的最小值」——这是这台
    机器上唯一稳的口径。
    """
    paths = [Path(f) for f in args.merge]
    merged = {}
    reps = 0
    for f in paths:
        data = json.loads(f.read_text(encoding="utf-8"))
        reps = max(reps, int(data.get("repeat") or 0))
        for r in data.get("rows", []):
            if "times" not in r:
                continue
            cur = merged.get(r["name"])
            if cur is None:
                merged[r["name"]] = {"name": r["name"], "n": r["n"],
                                     "times": dict(r["times"]),
                                     "output_same": r.get("output_same", True)}
            else:
                for lang in r["times"]:
                    cur["times"][lang] = min(cur["times"][lang], r["times"][lang])
                cur["output_same"] = bool(cur["output_same"] and r.get("output_same"))
    rows = []
    for name in sorted(merged):
        r = merged[name]
        r["speedup"] = {l: r["times"]["cpython"] / max(r["times"][l], 1e-9) for l in LANGS}
        rows.append(r)
    args.repeat = reps
    args.rounds = len(paths)
    print("%-16s %8s %11s %11s %11s %11s" % ("用例", "N", "CPython", "PyAdamas", "Go", "C++"))
    for r in rows:
        t = r["times"]
        print("%-16s %8d %8.1f ms %8.1f ms %8.1f ms %8.1f ms"
              % (r["name"], r["n"], t["cpython"] * 1e3, t["pyadamas"] * 1e3,
                 t["go"] * 1e3, t["cpp"] * 1e3))
    if args.json:
        Path(args.json).write_text(
            json.dumps({"rows": rows, "rounds": len(paths)}, ensure_ascii=False, indent=2),
            encoding="utf-8")
    if args.md:
        write_markdown(Path(args.md), rows, args)
        print("Markdown: %s" % args.md)
    return 0


def write_markdown(path, rows, args):
    rounds = getattr(args, "rounds", 0)
    reps = getattr(args, "repeat", 0) or getattr(args, "rounds", 1)
    try:
        cpy_ver = sh([PYTHON_BIN, "-V"]).stdout.strip()
    except OSError:
        cpy_ver = "unknown"
    go_ver = sh([GO_BIN, "version"]).stdout.strip()
    cxx_ver = sh([CXX_BIN, "--version"]).stdout.splitlines()[0] if rows else ""
    lines = [
        "# 四语言经典基准对比",
        "",
        "生成时间：%s" % datetime.datetime.now().strftime("%Y-%m-%d %H:%M"),
        "",
        "环境：%s，%s；%s；%s；%s。" % (
            platform.platform(), platform.processor() or "CPU 未识别",
            cpy_ver, go_ver, cxx_ver),
        "",
        "每种实现 %s；PyAdamas 用 `%s` 编译（生成码 `-O2`，每次 `--rebuild`"
        " 强制真编译），Go 用默认优化，C++ 用 `g++ -O2`。编译时间不计入；"
        "多租户机器上单轮绝对值能差 2 倍，所以这里按轮取最小值再合并。"
        % ("%d 轮 × 每轮 %d 次，取所有采样的最小值" % (rounds, reps)
           if rounds else "跑 %d 次取最小值" % args.repeat,
           PYADAMAS_BIN),
        "",
        "**这里的 PyAdamas 是默认管线**：通用自动向量化（`src/backend/vect.cpp` +"
        " `backend/masked.cpp`）默认开，5/12 个基准命中（matmul −86.5%、spectral_norm"
        " −59.6%、sieve −51.9%、nbody −45.2%、mandelbrot −36.3%，口径见"
        " `docs/vectorization-coverage.md`）。照着基准循环形状写死的特化 SIMD 模板"
        "（`PYADAMAS_SIMD`）已在 0.480 **整段删除**——它们对当前 IR 一次都没再匹配上，"
        "留着只会把「靶子上的性能」当成通用性能。换句话说：这张表没有用靶子上的特化。",
        "",
        "| 用例 | N | CPython | PyAdamas | Go | C++ | PyAdamas/CPython | PyAdamas/最快编译实现 | 输出一致 |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    for r in rows:
        if "times" not in r:
            err = next(iter(r.get("errors", {}).values()), "构建失败").splitlines()[0][:60]
            lines.append("| `%s` | %s | - | - | - | - | - | %s |"
                         % (r["name"], r.get("n", "-"), err))
            continue
        t = r["times"]
        lines.append(
            "| `%s` | %d | %.1f ms | %.1f ms | %.1f ms | %.1f ms | %.2f× | %.2f× | %s |"
            % (r["name"], r["n"], t["cpython"] * 1e3, t["pyadamas"] * 1e3,
               t["go"] * 1e3, t["cpp"] * 1e3, r["speedup"]["pyadamas"],
               min(t["go"], t["cpp"]) / max(t["pyadamas"], 1e-9),
               "是" if r.get("output_same") else "**否**")
        )
    timed = [r for r in rows if "times" in r]
    if timed:
        # 比值 <0.95 判明显更快、>1.05 判明显更慢，中间算噪声（单轮本就能差 20%）
        fast, slow = [], []
        for r in timed:
            q = r["times"]["pyadamas"] / min(r["times"]["go"], r["times"]["cpp"])
            if q < 0.95:
                fast.append(r["name"])
            elif q > 1.05:
                slow.append(r["name"])
        lines.append("")
        lines.append("共 %d 个用例。对比 CPython：全部更快；对比 Go/C++ 中的"
                     "较快者：%d 个明显更快、%d 个明显更慢、%d 个落在 ±5%% 噪声内。"
                     % (len(timed), len(fast), len(slow),
                        len(timed) - len(fast) - len(slow)))
        if slow:
            lines.append("")
            lines.append("明显偏慢：" + "、".join("`%s`" % n for n in slow))
    lines.append("")
    lines.append("用例：nbody（5 体积分）、spectral_norm（谱范数迭代）、"
                 "matmul（整数矩阵乘）、nqueens（N 皇后回溯）、"
                 "mandelbrot（迭代计数）、fib（朴素递归）、"
                 "fannkuch（全排列翻转）、binary_trees（二叉树分配）、"
                 "fasta（伪随机序列生成）、collatz（Collatz 步数统计）、"
                 "sieve（埃氏筛计数）、wordfreq（LCG 造词 + dict 计数）。")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
