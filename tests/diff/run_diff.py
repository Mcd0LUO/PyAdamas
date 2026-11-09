import concurrent.futures
import dataclasses
import os
import pathlib
import random
import subprocess
import sys
import tempfile
import argparse

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import generators  # noqa: E402


@dataclasses.dataclass
class Case:
    seed: int
    name: str
    idx: int
    src: str
    path: pathlib.Path


def run_one(py23, case):
    try:
        a = subprocess.run([py23, "run", str(case.path)], capture_output=True,
                           text=True, timeout=120)
        b = subprocess.run([sys.executable, str(case.path)], capture_output=True,
                           text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return "timeout", "", case.src
    if a.returncode != 0:
        return "compile", a.stderr, case.src
    if a.stdout != b.stdout:
        return "mismatch", (a.stdout, b.stdout), case.src
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
    ap.add_argument("--jobs", type=int, default=0,
                    help="并行进程数（默认 CPU 数；1 表示串行）")
    args = ap.parse_args()

    py23 = str(pathlib.Path(args.py23).resolve())
    if not pathlib.Path(py23).exists():
        sys.exit(f"找不到 py23：{py23}")

    names = [args.only] if args.only else list(generators.GENERATORS)
    for n in names:
        if n not in generators.GENERATORS:
            sys.exit(f"未知生成器：{n}")

    jobs = args.jobs or (len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity")
                         else (os.cpu_count() or 1))
    jobs = max(1, min(jobs, 32))

    with tempfile.TemporaryDirectory(prefix="py23-diff-") as td:
        td = pathlib.Path(td)
        cases = []
        nser = 0
        for seed in range(1, args.seeds + 1):
            for name in names:
                gen, default_cases = generators.GENERATORS[name]
                count = args.cases or default_cases
                rng = random.Random(seed)
                for case in range(count):
                    path = td / f"case_{nser}.py"
                    src = gen(rng, rng.randint(5, 15))
                    path.write_text(src, encoding="utf-8")
                    cases.append(Case(seed, name, case, src, path))
                    nser += 1

        bad = 0
        done = 0
        total = len(cases)
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            futs = {pool.submit(run_one, py23, c): c for c in cases}
            for fut in concurrent.futures.as_completed(futs):
                c = futs[fut]
                kind, detail, src = fut.result()
                done += 1
                if kind != "ok":
                    print(f"seed {c.seed} / {c.name} / case {c.idx}")
                    show(kind, detail, src)
                    bad += 1
                    if bad >= 5:
                        print("差异太多，先停在这里")
                        break
        print(f"结果：{done} 个程序，{bad} 个失败")
        return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
