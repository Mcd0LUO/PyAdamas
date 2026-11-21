import re
# fullmatch 顶层交替/量词分支：首条成功但没吃完整串时必须继续回溯
print(re.fullmatch(r"a|ab", "ab") is not None)
print(re.fullmatch(r"ab|a", "ab") is not None)
print(re.fullmatch(r"abc|ab", "ab") is not None)
print(re.fullmatch(r"(a|ab)c", "abc") is not None)
print(re.fullmatch(r"a+b|ab+", "abb") is not None)
print(re.fullmatch(r"ab?", "ab") is not None)
print(re.fullmatch(r"a?ab", "ab") is not None)
print(re.fullmatch(r"\d+|\d+x", "12x") is not None)
print(re.fullmatch(r"x|xy|xyz", "xyz") is not None)
print(re.fullmatch(r"a*a", "aaa") is not None)
print(re.fullmatch(r"a|ab", "abc") is not None)
print(re.fullmatch(r"[0-9]+|[0-9]+[a-z]", "12z") is not None)
print(re.fullmatch(r"(?:ab|abc)d", "abcd") is not None)
print(re.fullmatch(r"", "") is not None)
print(re.fullmatch(r"a*", "aaa") is not None)
# 编译后的 Pattern 走同一路径
p = re.compile(r"a|ab")
print(p.fullmatch("ab") is not None)
# match / search 不受影响（不要求整串）
m = re.match(r"a|ab", "ab")
if m:
    print("match", m.group(0))
s = re.search(r"a|ab", "xxab")
if s:
    print("search", s.group(0))
mm = re.match(r"a|ab", "ab")
if mm:
    print(mm.end())
# 断言/原子组内部仍然能局部成功（不能要求吃完整串）
print(re.findall(r"(?=(a|ab))", "ab"))
print(re.fullmatch(r"(?:(?=ab)ab)", "ab") is not None)
ss = re.search(r"a(?=b)", "ab")
if ss:
    print(ss.group(0))

# --- expect ---
# True
# True
# True
# True
# True
# True
# True
# True
# True
# True
# False
# True
# True
# True
# True
# True
# match a
# search a
# 1
# ['a']
# True
# a
