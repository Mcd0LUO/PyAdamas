#!/usr/bin/env python3
"""差分测试：随机生成程序，分别用 py23 和 CPython 跑，逐字节比较 stdout。

    python3 tests/diff/run_diff.py                      # 默认 3 个种子
    python3 tests/diff/run_diff.py --seeds 20           # 更多随机性
    python3 tests/diff/run_diff.py --only float --seeds 5
    python3 tests/diff/run_diff.py --py23 build/release/py23

py23 的退出码必须是 0（编译期错误视为失败，不是「不匹配」），stdout 必须与
CPython 完全相同；stderr 不比较（panic 消息是我们自己的）。

退出码：0 全过，1 有差异或编译失败。
"""
import argparse
import pathlib
import random
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import generators  # noqa: E402


def run_one(py23, src, workdir):
    path = workdir / "case.py"
    path.write_text(src, encoding="utf-8")
    try:
        a = subprocess.run([py23, "run", str(path)], capture_output=True, text=True, timeout=120)
        b = subprocess.run([sys.executable, str(path)], capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return "timeout", "", ""
    if a.returncode != 0:
        return "compile", a.stderr, src
    if a.stdout != b.stdout:
        return "mismatch", (a.stdout, b.stdout), src
    return "ok", "", ""


def show(kind, detail, src):
    print(f"  ✗ {kind}")
    if kind == "compile":
        print("    " + (detail.strip().splitlines() or [""])[0][:200])
    elif kind == "mismatch":
        got, want = detail
        for i, (x, y) in enumerate(zip(got.splitlines(), want.splitlines())):
            if x != y:
                print(f"    line {i}: py23={x!r} cpython={y!r}")
                break
        else:
            print(f"    py23 {len(got.splitlines())} 行 / cpython {len(want.splitlines())} 行")
    for line in src.splitlines()[:12]:
        print("    | " + line)


def main():
    ap = argparse.ArgumentParser(description="py23 差分测试")
    ap.add_argument("--py23", default="build/dev/py23", help="py23 可执行文件路径")
    ap.add_argument("--seeds", type=int, default=3, help="随机种子个数（种子 = 1..N）")
    ap.add_argument("--only", default="", help="只跑某个生成器：" + ", ".join(generators.GENERATORS))
    ap.add_argument("--cases", type=int, default=0, help="覆盖每个生成器的用例数")
    args = ap.parse_args()

    py23 = str(pathlib.Path(args.py23).resolve())
    if not pathlib.Path(py23).exists():
        sys.exit(f"找不到 py23：{py23}")

    names = [args.only] if args.only else list(generators.GENERATORS)
    for n in names:
        if n not in generators.GENERATORS:
            sys.exit(f"未知生成器：{n}")

    bad = 0
    total = 0
    with tempfile.TemporaryDirectory(prefix="py23-diff-") as td:
        workdir = pathlib.Path(td)
        for seed in range(1, args.seeds + 1):
            for name in names:
                gen, default_cases = generators.GENERATORS[name]
                cases = args.cases or default_cases
                rng = random.Random(seed)
                for case in range(cases):
                    src = gen(rng, rng.randint(5, 15))
                    total += 1
                    kind, detail, src2 = run_one(py23, src, workdir)
                    if kind != "ok":
                        print(f"seed {seed} / {name} / case {case}")
                        show(kind, detail, src2)
                        bad += 1
                        if bad >= 5:
                            print("差异太多，先停在这里")
                            print(f"\n结果：{total} 个程序，{bad} 个失败")
                            return 1
    print(f"结果：{total} 个程序，{bad} 个失败")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
