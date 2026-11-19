# argparse 基本解析面：位置/可选、actions、nargs、type、choices、缩写、
# bundling、-- 分隔、dest 推断、parse_known_args、Namespace 动态属性。
# 对拍 CPython：bool/None 值在 unknown 域按裸 1/0（0.324 边界），打印用
# 比较/条件取值，不直接 print 含 bool/None 的 Namespace。
import argparse

p = argparse.ArgumentParser(prog="tool")
p.add_argument("input")
p.add_argument("rest", nargs="*")
p.add_argument("-v", "--verbose", action="store_true")
p.add_argument("-n", "--num", type=int, default=3)
p.add_argument("--mode", choices=["fast", "slow"], default="fast")
p.add_argument("-o", metavar="OUT")
ns = p.parse_args(["in.txt", "-v", "-n", "7", "--mode", "slow", "a", "b"])
print(ns.input, ns.num, ns.mode, ns.rest)
print(ns.verbose == True, ns.o is None)
ns2 = p.parse_args(["x"])
print(ns2.num, ns2.mode, ns2.rest, ns2.verbose == False)

# dest 推断与显式 dest
p2 = argparse.ArgumentParser(prog="t")
a1 = p2.add_argument("-f", "--foo-bar")
a2 = p2.add_argument("--baz", "-b")
a3 = p2.add_argument("-x")
a4 = p2.add_argument("-s", "--long-name", dest="custom")
print(a1.dest, a2.dest, a3.dest, a4.dest)
print(p2.parse_args(["-s", "v"]).custom)

# bundling / 附着值 / = 形式
p3 = argparse.ArgumentParser(prog="t")
p3.add_argument("-a", action="store_true")
p3.add_argument("-b", action="store_true")
p3.add_argument("-f")
n = p3.parse_args(["-ab", "-fval"])
print(n.a == True, n.b == True, n.f)
n = p3.parse_args(["-af", "val"])
print(n.a == True, n.b == False, n.f)
n = p3.parse_args(["-f=-x"])
print(n.f)

# -- 分隔与交错
p4 = argparse.ArgumentParser(prog="t")
p4.add_argument("x")
p4.add_argument("--opt")
n = p4.parse_args(["--opt", "o", "pos"])
print(n.x, n.opt)
n = p4.parse_args(["pos", "--opt", "o"])
print(n.x, n.opt)
n = p4.parse_args(["--", "--opt"])
print(n.x, n.opt is None)

# 缩写
p5 = argparse.ArgumentParser(prog="t")
p5.add_argument("--foo-bar")
print(p5.parse_args(["--foo", "v"]).foo_bar)
print(p5.parse_args(["--foo-b", "v"]).foo_bar)
p5b = argparse.ArgumentParser(prog="t", allow_abbrev=False)
p5b.add_argument("--foo-bar")
ns5, ex5 = p5b.parse_known_args(["--foo", "v"])
print(ex5)

# nargs 各形态
p6 = argparse.ArgumentParser(prog="t")
p6.add_argument("--n", type=int, nargs=2)
p6.add_argument("--one", nargs=1)
p6.add_argument("--q", nargs="?")
n = p6.parse_args(["--n", "1", "2", "--one", "5"])
print(n.n, n.one)
print(p6.parse_args([]).q is None, p6.parse_args(["--q"]).q is None,
      p6.parse_args(["--q", "v"]).q)
p7 = argparse.ArgumentParser(prog="t")
p7.add_argument("--star", nargs="*")
p7.add_argument("--plus", nargs="+")
n = p7.parse_args(["--star", "--plus", "a", "b"])
print(n.star, n.plus)
p8 = argparse.ArgumentParser(prog="t")
p8.add_argument("pos", nargs="?", default="dflt")
print(p8.parse_args([]).pos, p8.parse_args(["v"]).pos)
p9 = argparse.ArgumentParser(prog="t")
p9.add_argument("files", nargs="+")
print(p9.parse_args(["a", "b", "c"]).files)

# count / append / append_const / store_const / store_false
p10 = argparse.ArgumentParser(prog="t")
p10.add_argument("-v", action="count", default=0)
print(p10.parse_args([]).v, p10.parse_args(["-vvv"]).v)
p11 = argparse.ArgumentParser(prog="t")
p11.add_argument("--a", action="append")
print(p11.parse_args([]).a is None, p11.parse_args(["--a", "1", "--a", "2"]).a)
p12 = argparse.ArgumentParser(prog="t")
p12.add_argument("--a", action="append", default=["d"])
print(p12.parse_args([]).a, p12.parse_args(["--a", "1"]).a)
p13 = argparse.ArgumentParser(prog="t")
p13.add_argument("--ac", action="append_const", const=7)
print(p13.parse_args(["--ac", "--ac"]).ac)
p14 = argparse.ArgumentParser(prog="t")
p14.add_argument("--flag", action="store_const", const=42)
print(p14.parse_args([]).flag is None, p14.parse_args(["--flag"]).flag)
p15 = argparse.ArgumentParser(prog="t")
p15.add_argument("--no-arg", action="store_false")
print(p15.parse_args([]).no_arg == True, p15.parse_args(["--no-arg"]).no_arg == False)

# type=float 与运算
p16 = argparse.ArgumentParser(prog="t")
p16.add_argument("--x", type=float)
n = p16.parse_args(["--x", "2.5"])
print(n.x * 2)

# 负数当值
p17 = argparse.ArgumentParser(prog="t")
p17.add_argument("--n", type=int)
print(p17.parse_args(["--n", "-5"]).n)

# parse_known_args 剩余
p18 = argparse.ArgumentParser(prog="t")
p18.add_argument("--known")
ns18, ex18 = p18.parse_known_args(["--known", "k", "--unknown", "u", "extra"])
print(ns18.known, ex18)

# Namespace 动态属性操作
p19 = argparse.ArgumentParser(prog="t")
p19.add_argument("--val", type=int, default=1)
ns19 = p19.parse_args([])
print(ns19.val)
ns19.val = 9
print(ns19.val)
setattr(ns19, "extra", "e")
print(ns19.extra, getattr(ns19, "extra"), hasattr(ns19, "extra"),
      hasattr(ns19, "nope"))
print(getattr(ns19, "nope", "dflt"))
print("extra" in ns19, "nope" in ns19)
d = vars(ns19)
print(d["val"], d["extra"])
ns20 = argparse.Namespace(a=1, b="x")
print(ns20.a, ns20.b)
print(repr(ns20))
# --- expect ---
# in.txt 7 slow ['a', 'b']
# True True
# 3 fast [] True
# foo_bar baz x custom
# v
# True True val
# True True val
# -x
# pos o
# pos o
# --opt True
# v
# v
# ['--foo', 'v']
# [1, 2] ['5']
# True True v
# [] ['a', 'b']
# dflt v
# ['a', 'b', 'c']
# 0 3
# True ['1', '2']
# ['d'] ['d', '1']
# [7, 7]
# True 42
# True True
# 5.0
# -5
# k ['--unknown', 'u', 'extra']
# 1
# 9
# e e True False
# dflt
# True False
# 9 e
# 1 x
# Namespace(a=1, b='x')
