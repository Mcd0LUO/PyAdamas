#!/usr/bin/env python3
"""用 CPython 3.14 给 tests/run/*_decimal*.py 生成 "# --- expect ---" 块。

pyadamas 的 float 构造口径是「最短 repr 再解析」，所以黄金侧把
Decimal(float) 换成 _D(repr(float))，其余构造/format 都是真 CPython。
用法：python3 tools/decexpect.py tests/run/406_decimal_ctor.py [...]
"""
import subprocess
import sys

SHIM = """from decimal import Decimal as _D, getcontext, Context  # noqa: F401
from decimal import (ROUND_HALF_EVEN, ROUND_HALF_UP, ROUND_HALF_DOWN,
                     ROUND_DOWN, ROUND_UP, ROUND_CEILING, ROUND_FLOOR,
                     ROUND_05UP)
Decimal = _D  # 只给变量注解用（构造调用一律走 Dec）
def Dec(v="0", ctx=None):
    if isinstance(v, float):
        return _D(repr(v))
    if isinstance(v, bool):
        return _D(int(v))
    return _D(v)
"""

MARK = "# --- expect ---"


def render(path, use_pa=False):
    text = open(path).read()
    body = text.split(MARK)[0].rstrip("\n")
    if use_pa:
        # 有意拒绝的 delta 用例（'n'、NaN/Infinity）：期望取自本实现，
        # 逐条在文件头注明哪些与 CPython 不同、为什么。
        run = subprocess.run(["./build/pyadamas", "run", "--rebuild", "-o",
                              "/tmp/_decexpect", path],
                             capture_output=True, text=True)
    else:
        lines = [l for l in body.splitlines()
                 if not l.startswith("from decimal import")]
        code = SHIM + "\n".join(lines) + "\n"
        code = code.replace("Decimal(", "Dec(")
        run = subprocess.run([sys.executable, "-c", code],
                             capture_output=True, text=True)
    if run.returncode != 0:
        print("golden side failed for", path)
        print(run.stderr[-3000:])
        return False
    if run.stderr:
        print("stderr:", run.stderr[:500])
    out = run.stdout.splitlines()
    # 每秒都带 "# " 前缀；空行写成 "#"（runner 的 sed -E 's/^# ?//' 还原）
    tail = ["# " + l if l else "#" for l in out]
    with open(path, "w") as f:
        f.write(body + "\n\n" + MARK + "\n" + "\n".join(tail) + "\n")
    print("wrote %s expect lines=%d" % (path, len(out)))
    return True


def main():
    ok = True
    args = sys.argv[1:]
    use_pa = "--pa" in args
    args = [a for a in args if a != "--pa"]
    for p in args:
        ok = render(p, use_pa) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
