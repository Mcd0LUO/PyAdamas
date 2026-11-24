#!/usr/bin/env python3
"""按每次操作归一化对比两份 run_bench.py 的 JSON 结果。

ratio = base 的 ns/op ÷ after 的 ns/op，>1 表示 after 更快。
之所以要归一化：run_bench 会按 CPython 实测时间重新校准 N，两份结果的 N 可能不同，
直接比 net ms 会把「多跑了一倍循环」误判成变慢。

用法：
    python3 bench/cmp_bench.py --base bench/report.json --after /tmp/bench_after.json
    python3 bench/cmp_bench.py --base a.json --after b.json --threshold 0.1 --md /tmp/cmp.md
"""
import argparse
import json
import math


def load(path):
    data = json.load(open(path, encoding="utf-8"))
    rows = {}
    for r in data.get("rows", []):
        if "error" not in r:
            rows[r["name"]] = r
    return data, rows


def ns_per_op(row):
    n = row.get("n") or 0
    t = row.get("net_pyadamas")
    if not n or not t:
        return None
    return t / n * 1e9


def fmt(x, nd=2):
    return "-" if x is None else ("%%.%df" % nd) % x


def main():
    ap = argparse.ArgumentParser(description="按每次操作归一化对比两份基准结果")
    ap.add_argument("--base", required=True)
    ap.add_argument("--after", required=True)
    ap.add_argument("--threshold", type=float, default=0.08,
                    help="自身变化超过该比例才标记（默认 0.08）")
    ap.add_argument("--head", type=int, default=0,
                    help="只看变化最大的 N 行（0=全部）")
    ap.add_argument("--md", default="", help="同时写一份 Markdown 表")
    args = ap.parse_args()

    db, b = load(args.base)
    da, a = load(args.after)
    missing = [n for n in b if n not in a] + [n for n in a if n not in b]

    rows = []
    for n, rb in b.items():
        ra = a.get(n)
        if ra is None:
            continue
        pb, pa = ns_per_op(rb), ns_per_op(ra)
        if pb is None or pa is None:
            continue
        rows.append({
            "name": n,
            "ratio": pb / pa,
            "pb": pb,
            "pa": pa,
            "nb": rb.get("n"),
            "na": ra.get("n"),
            "sb": rb.get("speedup"),
            "sa": ra.get("speedup"),
        })
    if not rows:
        print("没有可对比的用例")
        return 1
    rows.sort(key=lambda r: r["ratio"])

    shown = rows
    if args.head > 0 and args.head < len(rows):
        half = args.head // 2
        shown = rows[:half] + rows[-(args.head - half):]

    header = ("%-18s %8s %8s  %11s %11s  %6s" %
              ("用例", "base", "after", "base ns/op", "after ns/op", "自身"))
    print(header)
    print("-" * 78)
    for r in shown:
        mark = ""
        if r["ratio"] >= 1 + args.threshold:
            mark = " 快"
        elif r["ratio"] <= 1 - args.threshold:
            mark = " 慢"
        nnote = ""
        if r["nb"] != r["na"]:
            nnote = "  N %s→%s" % (r["nb"], r["na"])
        print("%-18s %7s× %7s×  %11.2f %11.2f  %5.2f×%s%s" % (
            r["name"], fmt(r["sb"], 1), fmt(r["sa"], 1),
            r["pb"], r["pa"], r["ratio"], mark, nnote))

    faster = sum(1 for r in rows if r["ratio"] >= 1 + args.threshold)
    slower = sum(1 for r in rows if r["ratio"] <= 1 - args.threshold)
    flat = len(rows) - faster - slower
    geo = math.exp(sum(math.log(r["ratio"]) for r in rows) / len(rows))
    ndiff = sum(1 for r in rows if r["nb"] != r["na"])
    print("-" * 78)
    print("用例 %d：快 %d，慢 %d，持平 %d；自身变化几何平均 %.3f×；N 不同 %d 个"
          % (len(rows), faster, slower, flat, geo, ndiff))
    if missing:
        print("只在一侧出现：%s" % ", ".join(missing))

    if args.md:
        lines = [
            "# 基准对比：%s → %s" % (args.base, args.after),
            "",
            "| 用例 | base 净加速 | after 净加速 | base ns/op | after ns/op | 自身变化 | N |",
            "|---|---:|---:|---:|---:|---:|---|",
        ]
        for r in rows:
            lines.append("| `%s` | %s× | %s× | %.2f | %.2f | %.2f× | %s → %s |" % (
                r["name"], fmt(r["sb"], 1), fmt(r["sa"], 1),
                r["pb"], r["pa"], r["ratio"], r["nb"], r["na"]))
        lines += ["", "用例 %d，自身变化几何平均 **%.3f×**；快 %d / 慢 %d / 持平 %d。"
                  % (len(rows), geo, faster, slower, flat)]
        open(args.md, "w", encoding="utf-8").write("\n".join(lines) + "\n")
        print("Markdown: %s" % args.md)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
