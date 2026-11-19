# argparse help/usage 排版：HelpFormatter 默认口径（帮助列、options: 节、
# metavar/choices/nargs 形态、description/epilog、add_help=False、自定义 usage）。
# -h 的退出码 0 + stdout 全文用自子进程对拍。
import argparse
import subprocess
import sys

p = argparse.ArgumentParser(prog="tool", description="A demo tool.")
p.add_argument("input", help="input file")
p.add_argument("rest", nargs="*", help="extra files")
p.add_argument("-v", "--verbose", action="store_true", help="be loud")
p.add_argument("-n", "--num", type=int, default=3, help="a number")
p.add_argument("--mode", choices=["fast", "slow"], default="fast", help="pick mode")
p.add_argument("-o", metavar="OUT", help="output")
print(p.format_help(), end="")
print("----")
print(p.format_usage(), end="")

p2 = argparse.ArgumentParser(prog="t", description="line1\nline2", epilog="after text")
p2.add_argument("x")
print(p2.format_help(), end="")
print("----")

p3 = argparse.ArgumentParser(prog="t", add_help=False)
p3.add_argument("--req", required=True)
p3.add_argument("--opt", nargs="?")
p3.add_argument("--star", nargs="*")
p3.add_argument("--plus", nargs="+")
p3.add_argument("--two", nargs=2)
p3.add_argument("pos")
print(p3.format_usage(), end="")
print("----")

p4 = argparse.ArgumentParser(prog="t", usage="%(prog)s [CUSTOM] stuff")
print(p4.format_usage(), end="")
print("----")

# 长调用：帮助换行到下一行（help 列上限 24）
p5 = argparse.ArgumentParser(prog="t")
p5.add_argument("--very-long-option-name", metavar="VALUE", help="long help")
p5.add_argument("-s", help="short")
print(p5.format_help(), end="")
print("----")

p6 = argparse.ArgumentParser(prog="t")
print(p6.format_help(), end="")


def child():
    pp = argparse.ArgumentParser(prog="tool", description="child help")
    pp.add_argument("--foo", help="foo help")
    pp.parse_args(sys.argv[2:])
    print("parsed-ok")


if len(sys.argv) > 1 and sys.argv[1] == "--child":
    child()
    sys.exit(0)

cmd: list[str] = []
if sys.argv[0].endswith(".py"):
    cmd.append("python3")
cmd.append(sys.argv[0])
cmd.append("--child")
cmd.append("-h")
r = subprocess.run(cmd, capture_output=True, text=True)
print("== -h rc=" + str(r.returncode))
print(r.stdout, end="")
print(r.stderr, end="")
# --- expect ---
# usage: tool [-h] [-v] [-n NUM] [--mode {fast,slow}] [-o OUT] input [rest ...]
#
# A demo tool.
#
# positional arguments:
#   input               input file
#   rest                extra files
#
# options:
#   -h, --help          show this help message and exit
#   -v, --verbose       be loud
#   -n, --num NUM       a number
#   --mode {fast,slow}  pick mode
#   -o OUT              output
# ----
# usage: tool [-h] [-v] [-n NUM] [--mode {fast,slow}] [-o OUT] input [rest ...]
# usage: t [-h] x
#
# line1 line2
#
# positional arguments:
#   x
#
# options:
#   -h, --help  show this help message and exit
#
# after text
# ----
# usage: t --req REQ [--opt [OPT]] [--star [STAR ...]] [--plus PLUS [PLUS ...]]
#          [--two TWO TWO]
#          pos
# ----
# usage: t [CUSTOM] stuff
# ----
# usage: t [-h] [--very-long-option-name VALUE] [-s S]
#
# options:
#   -h, --help            show this help message and exit
#   --very-long-option-name VALUE
#                         long help
#   -s S                  short
# ----
# usage: t [-h]
#
# options:
#   -h, --help  show this help message and exit
# == -h rc=0
# usage: tool [-h] [-v] [-n NUM] [--mode {fast,slow}] [-o OUT] input [rest ...]
#
# A demo tool.
#
# positional arguments:
#   input               input file
#   rest                extra files
#
# options:
#   -h, --help          show this help message and exit
#   -v, --verbose       be loud
#   -n, --num NUM       a number
#   --mode {fast,slow}  pick mode
#   -o OUT              output
# ----
# usage: tool [-h] [-v] [-n NUM] [--mode {fast,slow}] [-o OUT] input [rest ...]
# usage: t [-h] x
#
# line1 line2
#
# positional arguments:
#   x
#
# options:
#   -h, --help  show this help message and exit
#
# after text
# ----
# usage: t --req REQ [--opt [OPT]] [--star [STAR ...]] [--plus PLUS [PLUS ...]]
#          [--two TWO TWO]
#          pos
# ----
# usage: t [CUSTOM] stuff
# ----
# usage: t [-h] [--very-long-option-name VALUE] [-s S]
#
# options:
#   -h, --help            show this help message and exit
#   --very-long-option-name VALUE
#                         long help
#   -s S                  short
# ----
# usage: t [-h]
#
# options:
#   -h, --help  show this help message and exit
# usage: tool [-h] [--foo FOO]
#
# child help
#
# options:
#   -h, --help  show this help message and exit
#   --foo FOO   foo help
