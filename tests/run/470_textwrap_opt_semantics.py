# textwrap 优化回归：制表展开 / 空白替换 / 连字符切块的边界对拍（CPython 实跑取期望）。
import textwrap

cases: list[str] = ["", "a", "a b", "hello\tworld", "col\tval\tend", "x\ny", "  lead  trail  ", "a\v\f\r b"]
for s in cases:
    a = textwrap.fill(s, 10, expand_tabs=True, replace_whitespace=True)
    b = textwrap.fill(s, 10, expand_tabs=False, replace_whitespace=False)
    c = textwrap.shorten(s, 10)
    d = textwrap.dedent(s)
    e = textwrap.indent(s, "> ")
    print(len(a), len(b), len(c), len(d), len(e))
    # 控制字符的 repr 在 pa/CPython 有文档化的码点差异，统一改写后比对内容。
    print(a.replace("\v", "V").replace("\f", "F").replace("\r", "R"))
    print(b.replace("\v", "V").replace("\f", "F").replace("\r", "R"))
    print(c.replace("\v", "V").replace("\f", "F").replace("\r", "R"))
for t in [4, 8, 0, -1]:
    print(repr(textwrap.fill("a\tb", 20, tabsize=t)))
# 连字符切块：goof-ball 与 em-dash 的切分点
print(textwrap.wrap("Look, goof-ball -- use the -b option!", 12))
print(textwrap.wrap("alpha-beta-gamma delta", 8))
# --- expect ---
# 0 0 0 0 0
# 
# 
# 
# 1 1 1 1 3
# a
# a
# a
# 3 3 3 3 5
# a b
# a b
# a b
# 11 11 5 11 13
# hello
# world
# hello
# world
# [...]
# 11 11 9 11 13
# col
# val
# end
# col	val
# end
# col [...]
# 3 3 3 3 7
# x y
# x
# y
# x y
# 12 12 10 13 17
#   lead
# trail
#   lead
# trail
# lead trail
# 6 6 3 6 10
# a    b
# aVFR b
# a b
# 'a   b'
# 'a       b'
# 'ab'
# 'ab'
# ['Look, goof-', 'ball -- use', 'the -b', 'option!']
# ['alpha-', 'beta-', 'gamma', 'delta']
