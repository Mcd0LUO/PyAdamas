# pyadamas: lang-gaps
# re.Pattern / re.Match 声明式方法表对拍（CPython 3.14 同形脚本）
# 已知差异：Match.groups() PA 返回 list、CPython 返回 tuple（本轮未改），用 list() 对齐。
import re

p = re.compile(r"a(b+)")
m = p.search("xabby")
print("search", m is not None)
if m is not None:
    print("g0", m.group())
    print("g1", m.group(1))
    print("span", m.span())
    print("span1", m.span(1))
    print("start", m.start(1), m.end(1))
    print("expand", m.expand("X<\1>"))

m2 = p.match("abbb")
print("match", m2 is not None)
m3 = p.fullmatch("abbb")
print("full", m3 is not None)
print("findall", p.findall("ab abbb"))
print("sub", p.sub("Z", "ab abbb"))
print("split", p.split("xabbyabb"))
mg = p.search("ab abb")
if mg is not None:
    print("groups", list(mg.groups()) == ["b"])

# --- expect ---
# search True
# g0 abb
# g1 bb
# span (1, 4)
# span1 (2, 4)
# start 2 4
# expand X<>
# match True
# full True
# findall ['b', 'bbb']
# sub Z Z
# split ['x', 'bb', 'y', 'bb', '']
# groups True
