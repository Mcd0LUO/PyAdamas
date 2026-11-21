# configparser 修改面 + 映射协议 + SectionProxy + write 往返/键校验。
import configparser
from configparser import SectionProxy

c = configparser.ConfigParser()
c.read_string("[s]\na = 1\nnum = 42\n[DEFAULT]\nd = base\n[t]\nx = 2\n")
print(c.sections(), len(c), "s" in c, "DEFAULT" in c, "nope" in c)
p0: SectionProxy = c["s"]
print(len(p0), "a" in p0, "d" in p0)
print(repr(p0))
for k in c:
    print(k)

c.set("s", "b", "2")
c.set("", "e", "from-empty")
c.set("DEFAULT", "f", "from-default")
c.add_section("new")
c.set("new", "k", "v")
print(c.sections(), c.defaults())
print(c.get("s", "b"), c.get("s", "e"), c.get("s", "f"), c.get("new", "k"))
print(c.remove_option("s", "a"), c.remove_option("s", "a"))
print(c.remove_option("DEFAULT", "d"), c.remove_option("", "e"))
print(c.remove_section("new"), c.remove_section("nope"))
print(c.sections(), c.defaults())

p: SectionProxy = c["s"]
print(p["b"], p.get("b"), p.getint("num"), p.getint("nope", 5))
print(p.get("nope") == None)
p["c"] = "3"
print("c" in p, len(p), len(p.keys()), len(p.items()), len(p.values()))
print(p.pop("c"))
print("c" in p)
print(p.setdefault("e2", "v2"))
print(p.pop("e2"), p.pop("nope", "dflt"))
p.update({"u": "4"})
print(p["u"], len(p))

q = configparser.ConfigParser()
q.read_string("[m]\nx = 1\ny = 2\n")
qm: SectionProxy = q["m"]
qm.clear()
print(len(qm.keys()), len(qm))

c["s"] = {"z": "9"}
pz: SectionProxy = c["s"]
print(len(pz.items()))
print(c.remove_section("s"))
print(c.sections(), "s" in c)
print(c.remove_section("DEFAULT"))
try:
    c["nope"]
except KeyError:
    print("K1")

dp: SectionProxy = c["DEFAULT"]
dp["dd"] = "1"
print(len(dp.items()), c.get("t", "dd"))
print(dp.pop("dd"))
print("dd" in dp)

w = configparser.ConfigParser()
w.read_string("[one]\na = 1\nmulti = x\n  y\n")
w.set("one", "b", "2")
w.set("DEFAULT", "d", "3")
f = open("/tmp/pa393.ini", "w")
w.write(f)
f.close()
f = open("/tmp/pa393.ini", "r")
print(f.read(), end="")
f.close()
r = configparser.ConfigParser()
print(r.read("/tmp/pa393.ini"))
print(r.get("one", "a"), r.get("one", "b"), r.get("one", "d"), repr(r.get("one", "multi")))
f = open("/tmp/pa393b.ini", "w")
w.write(f, False)
f.close()
f = open("/tmp/pa393b.ini", "r")
print(f.read(), end="")
f.close()

bad = configparser.ConfigParser()
bad.add_section("s")
bad.set("s", "a=b", "1")
f = open("/tmp/pa393c.ini", "w")
try:
    bad.write(f)
except configparser.InvalidWriteError as e:
    print("W1", str(e))
f.close()
bad2 = configparser.ConfigParser()
bad2.add_section("s2")
bad2.set("s2", "[k]", "v")
f = open("/tmp/pa393d.ini", "w")
try:
    bad2.write(f)
except configparser.InvalidWriteError as e:
    print("W2", str(e))
f.close()
# --- expect ---
# ['s', 't'] 3 True True False
# 3 True True
# <Section: s>
# DEFAULT
# s
# t
# ['s', 't', 'new'] {'d': 'base', 'e': 'from-empty', 'f': 'from-default'}
# 2 from-empty from-default v
# True False
# True True
# True False
# ['s', 't'] {'f': 'from-default'}
# 2 2 42 5
# True
# True 4 4 4 4
# 3
# False
# v2
# v2 dflt
# 4 4
# 0 0
# 2
# True
# ['t'] False
# False
# K1
# 2 1
# 1
# False
# [DEFAULT]
# d = 3
#
# [one]
# a = 1
# multi = x
# 	y
# b = 2
#
# ['/tmp/pa393.ini']
# 1 2 3 'x\ny'
# [DEFAULT]
# d=3
#
# [one]
# a=1
# multi=x
# 	y
# b=2
#
# W1 Cannot write key a=b; contains delimiter =
# W2 Cannot write key [k]; begins with section pattern
