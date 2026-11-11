# 差分测试运行器。本脚本限制在 py23 能编译的子集内，可以直接用 py23 执行：
#
#   ./build/dev/py23 build -o /tmp/run_diff.prog tests/diff/run_diff.py
#   /tmp/run_diff.prog --py23 build/dev/py23 --cpython python3 --seeds 3
#
# 用 CPython 执行也能跑（生成器与随机序列两端一致）。
import os
import os.path as ospath
import random
import subprocess
import sys

import generators

PY23 = "build/dev/py23"
CPYTHON = "python3"
SEEDS = 3
ONLY = ""
CASES = 0


class Case:
    seed: int
    name: str
    idx: int
    src: str
    path: str

    def __init__(self, seed: int, name: str, idx: int, src: str, path: str) -> None:
        self.seed = seed
        self.name = name
        self.idx = idx
        self.src = src
        self.path = path


def parse_args(argv: list[str]) -> None:
    global PY23, CPYTHON, SEEDS, ONLY, CASES
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--py23" and i + 1 < len(argv):
            PY23 = argv[i + 1]
            i += 2
        elif a == "--cpython" and i + 1 < len(argv):
            CPYTHON = argv[i + 1]
            i += 2
        elif a == "--seeds" and i + 1 < len(argv):
            SEEDS = int(argv[i + 1])
            i += 2
        elif a == "--only" and i + 1 < len(argv):
            ONLY = argv[i + 1]
            i += 2
        elif a == "--cases" and i + 1 < len(argv):
            CASES = int(argv[i + 1])
            i += 2
        else:
            i += 1


def gen_source(name: str, rng, n: int) -> str:
    if name == "expr":
        return generators.expr(rng, n)
    if name == "struct":
        return generators.struct(rng, n)
    if name == "float":
        return generators.float_ops(rng, n)
    if name == "repr":
        return generators.repr_doubles(rng, n)
    if name == "gen":
        return generators.gen_ops(rng, n)
    if name == "async":
        return generators.async_ops(rng, n)
    if name == "range":
        return generators.range_ops(rng, n)
    return ""


def run_one(py23: str, case: Case) -> tuple[str, list[str], str]:
    # 比较也放在 try 里：变量只在块内使用，try 外的类型合并不会把它拓宽。
    try:
        a = subprocess.run([py23, "run", case.path],
                           capture_output=True, text=True, timeout=120.0)
        b = subprocess.run([CPYTHON, case.path],
                           capture_output=True, text=True, timeout=120.0)
        if a.returncode != 0:
            return "compile", [a.stderr], case.src
        if a.stdout != b.stdout:
            return "mismatch", [a.stdout, b.stdout], case.src
        return "ok", [], ""
    except Exception:
        return "timeout", ["子进程超时或启动失败"], case.src


def first_line(s: str) -> str:
    lines = s.strip().splitlines()
    if len(lines) == 0:
        return ""
    line = lines[0]
    if len(line) > 200:
        line = line[:200]
    return line


def show(kind: str, detail: list[str], src: str) -> None:
    print(f"  ✗ {kind}")
    if kind == "compile":
        msg = ""
        if len(detail) > 0:
            msg = first_line(detail[0])
        print("    " + msg)
    elif kind == "mismatch" and len(detail) >= 2:
        got = detail[0]
        want = detail[1]
        gl = got.splitlines()
        wl = want.splitlines()
        n = len(gl)
        if len(wl) < n:
            n = len(wl)
        i = 0
        shown = False
        while i < n:
            if gl[i] != wl[i]:
                print(f"    line {i}: py23={gl[i]!r} cpython={wl[i]!r}")
                shown = True
                break
            i += 1
        if not shown:
            print(f"    py23 {len(gl)} 行 / cpython {len(wl)} 行")
    lines = src.splitlines()
    n = len(lines)
    if n > 12:
        n = 12
    i = 0
    while i < n:
        print("    | " + lines[i])
        i += 1


def main() -> int:
    parse_args(sys.argv)

    names: list[str] = []
    if ONLY != "":
        names.append(ONLY)
    else:
        i = 0
        while i < len(generators.GENERATOR_NAMES):
            names.append(generators.GENERATOR_NAMES[i])
            i += 1
    for nm in names:
        if generators.default_cases(nm) == 0:
            print(f"未知生成器：{nm}")
            return 1

    if not ospath.exists(PY23):
        print(f"找不到 py23：{PY23}")
        return 1

    td = "/tmp/py23-diff-cases"
    if not ospath.exists(td):
        os.mkdir(td)

    cases: list[Case] = []
    nser = 0
    seed = 1
    while seed <= SEEDS:
        i = 0
        while i < len(names):
            nm = names[i]
            count = CASES
            if count == 0:
                count = generators.default_cases(nm)
            rng = random.Random(seed)
            k = 0
            while k < count:
                cpath = ospath.join(td, f"case_{nser}.py")
                src = gen_source(nm, rng, rng.randint(5, 15))
                f = open(cpath, "w")
                f.write(src)
                f.close()
                cases.append(Case(seed, nm, k, src, cpath))
                nser += 1
                k += 1
            i += 1
        seed += 1

    bad = 0
    done = 0
    total = len(cases)
    idx = 0
    while idx < total:
        c = cases[idx]
        kind, detail, src = run_one(PY23, c)
        done += 1
        if kind != "ok":
            print(f"seed {c.seed} / {c.name} / case {c.idx}")
            show(kind, detail, src)
            bad += 1
            if bad >= 5:
                print("差异太多，先停在这里")
                break
        idx += 1

    # 清掉本轮生成的文件；上一次中断留下的残留不影响同名覆盖，
    # 目录删不掉就留着，下次继续用。
    idx = 0
    while idx < len(cases):
        p = cases[idx].path
        if ospath.exists(p):
            os.remove(p)
        idx += 1
    try:
        os.rmdir(td)
    except OSError:
        pass

    print(f"结果：{done} 个程序，{bad} 个失败")
    if bad == 0:
        return 0
    return 1


sys.exit(main())
