# re 优化回归：覆盖本次加入的快路径（编译缓存 / 首字节剪枝 / 纯字面量 /
# 简单循环紧扫 / 末尾空匹配单点）在边界上的语义，逐行对拍 CPython re。
import re

text = "alpha 12 x7, beta 340 x0, gamma 5 x99, delta 0 x1"
lines = ["alpha 1 x2", "beta 33 x44", "gamma 555 x6", "delta 7 x8"]

# 1) 纯字面量：findall / search / split / sub / match / fullmatch / 编译对象
print(re.findall("alpha", text))
print(re.findall("x", text))
print(re.findall("zzz", text))
m = re.search("gamma", text)
if m is None:
    print("NONE")
else:
    print(m.start(), m.end(), m.group(0))
m = re.match("alpha", text)
if m is None:
    print("NONE")
else:
    print(m.span())
print(re.fullmatch("alpha", "alpha") is not None, re.fullmatch("alpha", "alphax") is None)
print(re.split(" ", "a b c"))
print(re.sub("alpha", "A", text))
p = re.compile("beta")
print(p.findall(text))
mm = p.search(text)
if mm is None:
    print("NONE")
else:
    print(mm.start())
print(re.findall("", "abc"))
mm = re.search("", "abc")
if mm is None:
    print("NONE")
else:
    print(mm.span())

# 2) IGNORECASE 字面量（不能走 memmem 快路径）+ 类/量词首字节剪枝
print(re.findall("ALPHA", text, re.I))
print(re.findall("Alpha", text, re.I))
print(re.findall(r"[0-9]+", text))
print(re.findall(r"[A-Za-z]+", text))
print(re.findall(r"\d+", text))
print(re.findall(r"\w+", text))
print(re.findall(r"x[0-9]", text))
print(re.findall(r"\s+", text))
print(re.findall(r".", "ab\nc", re.S))
print(re.findall(r".", "ab\nc"))

# 3) 锚定：\A / ^ / MULTILINE / 行首剪枝 / 搜索中途失败
print(re.findall(r"^a", "abc"))
print(re.findall(r"^.", "a\nb\nc", re.M))
print(re.findall(r"\Aalpha", text))
m = re.search(r"^beta", text)
print(m is None)
m = re.search(r"(?m)^beta", text)
if m is None:
    print("NONE")
else:
    print(m.start())
print(re.sub(r"(?m)^", ">", "a\nb"))
print(re.findall(r"b$", "ab\ncb"))
print(re.findall(r"(?m)b$", "ab\ncb"))

# 4) 简单循环紧扫的特化分支：类 / 字面字符 / 点号；贪婪与惰性
print(re.findall(r"a*", "aaabaa"))
print(re.findall(r"a+", "aaabaa"))
print(re.findall(r"a+?", "aaabaa"))
print(re.findall(r"[0-9]*", "a12b"))
print(re.findall(r"[0-9]+?", "a12b"))
print(re.findall(r".*", "ab\ncd"))
print(re.findall(r".*", "ab\ncd", re.S))
print(re.findall(r"b*", "abbbc"))
print(re.findall(r"x*", "abc"))
print(re.sub(r"a+", "-", "baaacaa"))
print(re.split(r"a+", "baaacaa"))
print(re.findall(r"[ab]+", "aabbccab"))

# 5) 循环后接单指令收尾（一阶失败剪枝）：X*Y / X+Y / X*$ / X*\b
print(re.findall(r"[a-z]+ [0-9]+", text))
print(re.findall(r"[a-z]*[0-9]", "ab12cd34"))
print(re.findall(r"[0-9]*x", "12x 3x 4x"))
print(re.findall(r"a*b", "aaab aab b ab"))
print(re.findall(r"a+b", "aaab aab b ab"))
print(re.findall(r"a+ c", "aaa c aa c"))
print(re.findall(r"[0-9]+$", "a1 b22 c333"))
print(re.findall(r"[0-9]*$", "a1 b22 c333"))
print(re.findall(r"[a-z]+\b", "ab cd ef"))
print(re.findall(r"[a-z]*\b", "ab cd ef"))
print(re.findall(r"\w+@\w+", "a@b c@d e"))

# 6) 分组 / 交替 / 捕获组（findall 取组语义）
print(re.findall(r"alpha|gamma", text))
p2 = re.compile(r"(?P<a>\d+)-(?P<b>\d+)")
m = p2.search("x 12-34 y")
if m is None:
    print("NONE")
else:
    print(m.group(0), m.group(1), m.group(2), m.group("a"), m.groupdict()["b"])
    gs = m.groups()
    print(gs[0], gs[1], m.span(1), m.start(2), m.end(2))

# 7) 边界：剩余起点里的空匹配、重复调用、多模式轮换（缓存 LRU 槽位）
print(re.findall(r"b*", "abbbc"))
print(re.findall(r"b*", "abbbc"))
print(re.findall(r"x*", ""))
print(re.split(r"b*", "abbbc"))
print(re.sub(r"b*", "-", "abbbc"))
print(re.sub(r"", "-", ""))
pats = [r"[0-9]+", r"[a-z]+", r"x[0-9]", r"alpha", r"\s+"]
tot = 0
for i in range(3):
    for pp in pats:
        tot += len(re.findall(pp, text))
print(tot)

# 8) 后向引用 / 断言（分析必须保守放弃，退回逐位）
print(re.findall(r"(\w)\1", "aabbc dd"))
print(re.sub(r"(a)(?=b)", "X", "ab ac ab"))
print(re.sub(r"(?<=a)b", "X", "ab cb ab"))

# 9) 多行 + DOTALL 首字节剪枝
ml = "one\ntwo\nthree"
print(re.findall(r"(?m)^\w+", ml))
print(re.findall(r"(?s)^.*$", ml))
print(re.findall(r"(?m)^t", ml))
print(re.split(r"(?m)^", ml))

# 10) Pattern 首次使用才分析 / 同一 Pattern 多次调用
q = re.compile(r"[0-9]+")
print(q.findall("a1b22"), q.sub("#", "a1b22"), q.split("a1b22"))
print(q.findall("a1b22"))

# 11) 子串命中位置靠后（首字节剪枝 + memmem 的起点推进）
long = "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzneedle"
mm = re.search("needle", long)
if mm is None:
    print("NONE")
else:
    print(mm.start())
print(re.findall(r"[0-9]+", long))
print(re.findall(r"[0-9]+", long + "42"))

# 12) 贪婪失败回退：X* 后接不可能匹配的收尾（含末尾/整串失败）
print(re.findall(r"a*b", "aaac"))
print(re.findall(r"[0-9]+x", "123 y"))
print(re.search(r"a*b", "aaac"))
print(re.search(r"[0-9]+z", "123 y4"))
acc = 0
for line in lines:
    acc += len(re.findall(r"[0-9]+", line))
print(acc)
print(re.findall(r"[0-9]+", lines[1]))

# --- expect ---
# ['alpha']
# ['x', 'x', 'x', 'x']
# []
# 26 31 gamma
# (0, 5)
# True True
# ['a', 'b', 'c']
# A 12 x7, beta 340 x0, gamma 5 x99, delta 0 x1
# ['beta']
# 13
# ['', '', '', '']
# (0, 0)
# ['alpha']
# ['alpha']
# ['12', '7', '340', '0', '5', '99', '0', '1']
# ['alpha', 'x', 'beta', 'x', 'gamma', 'x', 'delta', 'x']
# ['12', '7', '340', '0', '5', '99', '0', '1']
# ['alpha', '12', 'x7', 'beta', '340', 'x0', 'gamma', '5', 'x99', 'delta', '0', 'x1']
# ['x7', 'x0', 'x9', 'x1']
# [' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ']
# ['a', 'b', '\n', 'c']
# ['a', 'b', 'c']
# ['a']
# ['a', 'b', 'c']
# ['alpha']
# True
# NONE
# >a
# >b
# ['b']
# ['b', 'b']
# ['aaa', '', 'aa', '']
# ['aaa', 'aa']
# ['a', 'a', 'a', 'a', 'a']
# ['', '12', '', '']
# ['1', '2']
# ['ab', '', 'cd', '']
# ['ab\ncd', '']
# ['', 'bbb', '', '']
# ['', '', '', '']
# b-c-
# ['b', 'c', '']
# ['aabb', 'ab']
# ['alpha 12', 'beta 340', 'gamma 5', 'delta 0']
# ['ab1', '2', 'cd3', '4']
# ['12x', '3x', '4x']
# ['aaab', 'aab', 'b', 'ab']
# ['aaab', 'aab', 'ab']
# ['aaa c', 'aa c']
# ['333']
# ['333', '']
# ['ab', 'cd', 'ef']
# ['ab', '', 'cd', '', 'ef', '']
# ['a@b', 'c@d']
# ['alpha', 'gamma']
# 12-34 12 34 12 34
# 12 34 (2, 4) 5 7
# ['', 'bbb', '', '']
# ['', 'bbb', '', '']
# ['']
# ['', 'a', '', 'c', '']
# -a--c-
# -
# 96
# ['a', 'b', 'd']
# Xb ac Xb
# aX cb aX
# ['one', 'two', 'three']
# ['one\ntwo\nthree']
# ['t', 't']
# ['', 'one\n', 'two\n', 'three']
# ['1', '22'] a#b# ['a', 'b', '']
# ['1', '22']
# 40
# []
# ['42']
# []
# []
# None
# None
# 8
# ['33', '44']
