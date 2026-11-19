# argparse 错误路径：退出码 2 + usage + 错误消息到 stderr（自子进程对拍，
# 同一份源码在 CPython/pyadamas 下跑，argv[0] 区分执行形态）。
# 进程内可捕获的 ArgumentError/ValueError/TypeError 也覆盖。
import argparse
import subprocess
import sys


def child():
    sc = sys.argv[2]
    rest = sys.argv[3:]
    p = argparse.ArgumentParser(prog="tool")
    if sc == "req":
        p.add_argument("--req", required=True)
        p.add_argument("pos")
    elif sc == "reqpos":
        p.add_argument("p1")
        p.add_argument("p2")
    elif sc == "int":
        p.add_argument("--n", type=int)
    elif sc == "float":
        p.add_argument("--x", type=float)
    elif sc == "choice":
        p.add_argument("--c", choices=["a", "b"])
    elif sc == "one":
        p.add_argument("--foo")
    elif sc == "two":
        p.add_argument("--n", type=int, nargs=2)
    elif sc == "least":
        p.add_argument("--plus", nargs="+")
    elif sc == "explicit":
        p.add_argument("--flag", action="store_true")
    elif sc == "ambig":
        p.add_argument("--long-name")
        p.add_argument("--long-other")
    elif sc == "unknown":
        p.add_argument("--known")
    elif sc == "help":
        p.add_argument("--foo")
    p.parse_args(rest)
    print("parsed-ok")


def run(sc, args):
    cmd: list[str] = []
    if sys.argv[0].endswith(".py"):
        cmd.append("python3")
    cmd.append(sys.argv[0])
    cmd.append("--child")
    cmd.append(sc)
    cmd.extend(args)
    r = subprocess.run(cmd, capture_output=True, text=True)
    print("== " + sc + " rc=" + str(r.returncode))
    print(r.stdout, end="")
    print(r.stderr, end="")


if len(sys.argv) > 2 and sys.argv[1] == "--child":
    child()
    sys.exit(0)

run("req", [])
run("reqpos", ["a"])
run("int", ["--n", "xx"])
run("float", ["--x", "yy"])
run("choice", ["--c", "z"])
run("one", ["--foo"])
run("two", ["--n", "1"])
run("least", ["--plus"])
run("explicit", ["--flag=1"])
run("ambig", ["--long"])
run("unknown", ["--zzz"])
run("help", ["-h"])

# 进程内异常
p = argparse.ArgumentParser(prog="t")
p.add_argument("--foo-bar")
try:
    p.add_argument("--foo-bar")
except argparse.ArgumentError as e:
    print("AE", str(e))
try:
    p.add_argument("--q", action="bogus")
except ValueError as e:
    print("VE", str(e))
try:
    p.add_argument("--q2", action="store_true", nargs=2)
except TypeError as e:
    print("TE", str(e))
# --- expect ---
# == req rc=2
# usage: tool [-h] --req REQ pos
# tool: error: the following arguments are required: --req, pos
# == reqpos rc=2
# usage: tool [-h] p1 p2
# tool: error: the following arguments are required: p2
# == int rc=2
# usage: tool [-h] [--n N]
# tool: error: argument --n: invalid int value: 'xx'
# == float rc=2
# usage: tool [-h] [--x X]
# tool: error: argument --x: invalid float value: 'yy'
# == choice rc=2
# usage: tool [-h] [--c {a,b}]
# tool: error: argument --c: invalid choice: 'z' (choose from a, b)
# == one rc=2
# usage: tool [-h] [--foo FOO]
# tool: error: argument --foo: expected one argument
# == two rc=2
# usage: tool [-h] [--n N N]
# tool: error: argument --n: expected 2 arguments
# == least rc=2
# usage: tool [-h] [--plus PLUS [PLUS ...]]
# tool: error: argument --plus: expected at least one argument
# == explicit rc=2
# usage: tool [-h] [--flag]
# tool: error: argument --flag: ignored explicit argument '1'
# == ambig rc=2
# usage: tool [-h] [--long-name LONG_NAME] [--long-other LONG_OTHER]
# tool: error: ambiguous option: --long could match --long-name, --long-other
# == unknown rc=2
# usage: tool [-h] [--known KNOWN]
# tool: error: unrecognized arguments: --zzz
# == help rc=0
# usage: tool [-h] [--foo FOO]
#
# options:
#   -h, --help  show this help message and exit
#   --foo FOO
# AE argument --foo-bar: conflicting option string: --foo-bar
# VE unknown action 'bogus'
# TE _StoreTrueAction.__init__() got an unexpected keyword argument 'nargs'
