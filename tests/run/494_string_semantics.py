# 字符串语义回归（0.429）：replace 的 count=0（以前会替换一次，静默错值）+
# 支持面上的拆分/连接/查找/修剪/大小写/格式化/运算/不可变性。

# 1) replace count 语义（修复路径）：0 = 不替换，负数 = 全部
s = "abcabcabc"
print("rep_c0", s.replace("abc", "X", 0))
print("rep_c1", s.replace("abc", "X", 1), s.replace("abc", "X", 2))
print("rep_cneg", s.replace("abc", "X", -1), s.replace("abc", "X", -5))
print("rep_old_empty", "abc".replace("", "-", 0), "abc".replace("", "-", 1), "abc".replace("", "-", -1))
print("rep_nomatch", "abc".replace("z", "X", 0), "".replace("a", "b"))
b = b"abcabc"
print("bytes_c0", b.replace(b"abc", b"X", 0), b.replace(b"abc", b"X", 1))

# 2) 拆分/连接
print("split_ws", "a b  c".split(), "  a b  ".split(), "".split(), "   ".split())
print("split_sep", "a,b,c".split(","), "a,b,c".split(",", 1), "a,b,c".split(",", 0))
print("split_absent", "abc".split(","), "a,,b".split(","))
print("splitlines", "a\nb\r\nc\r\n".splitlines(), "a\nb".splitlines(), "".splitlines())
print("join", ",".join(["a", "b"]), "".join([]), "-".join(["x"]), " ".join(["a", "b", "c"]))

# 3) 查找/替换（count 1 参）
f = "abcabcabc"
print("find", f.find("abc"), f.find("abc", 4), f.find("z"), f.find("", 3))
print("find_span", f.find("abc", 1, 5), f.rfind("abc", 0, 5), f.rfind("z"))
print("index", f.index("abc"), f.index("abc", 1))
print("count", f.count("abc"), "aaa".count("aa"), f.count(""), "".count("a"))
try:
    f.index("z")
except ValueError as e:
    print("index_missing", str(e))

# 4) 修剪/大小写（ASCII）
print("strip", "  ab  ".strip(), "xxabxx".strip("x"), "xyabyx".strip("xy"), "abc".strip(""))
print("lrstrip", " a b ".lstrip(), " a b ".rstrip(), "aabbcc".lstrip("a"), "aabbcc".rstrip("c"))
print("case", "aBc DeF".upper(), "aBc DeF".lower(), "hello world".title(), "hELLO wORLD".capitalize())

# 5) 格式化（f-string 与 % 标量）
x = 42
name = "bob"
print("fstring", f"{x} {name}", f"{x:5d}", f"{3.14159:.2f}", f"{name!r}")
print("percent", "%d" % 5, "%s" % "a", "%5.1f" % 3.14159, "%x" % 255, "%d%%" % 50)
print("percent_multi", "%d %s %.1f" % (1, "a", 2.25), "%-8s|" % "ab")

# 6) 运算 / 不可变性 / 迭代 / ord-chr
t = "ab"
print("ops", t * 3, t * 0, t * -1, t + "cd", "bc" in "abcd", "" in "abc")
print("cmp", "abc" < "abd", "ab" < "abc", "b" > "abc", "ABC" < "abc")
u = t
u += "cd"
print("iadd_immutable", t, u, t.upper())
print("iter_len", [c for c in "abc"], len("abc"), len("h\u00e9llo"))
print("ord_chr", ord("A"), chr(65), ord("\u00e9"), chr(233))

# 7) 码点语义（非 ASCII 的大小写是文档化差异，这里只测长度/迭代/切片/查找）
w = "h\u00e9llo"
print("cp_sem", len(w), [c for c in w], w[1:3], w.find("\u00e9"), w.replace("\u00e9", "e"))
print("cp_cmp", w < "h\u00e9llp", "\u4e2d" < "\u6587", "\u4e2d" in "a\u4e2db")

# --- expect ---
# rep_c0 abcabcabc
# rep_c1 Xabcabc XXabc
# rep_cneg XXX XXX
# rep_old_empty abc -abc -a-b-c-
# rep_nomatch abc 
# bytes_c0 b'abcabc' b'Xabc'
# split_ws ['a', 'b', 'c'] ['a', 'b'] [] []
# split_sep ['a', 'b', 'c'] ['a', 'b,c'] ['a,b,c']
# split_absent ['abc'] ['a', '', 'b']
# splitlines ['a', 'b', 'c'] ['a', 'b'] []
# join a,b  x a b c
# find 0 6 -1 3
# find_span -1 0 -1
# index 0 3
# count 3 1 10 0
# index_missing substring not found
# strip ab ab ab abc
# lrstrip a b   a b bbcc aabb
# case ABC DEF abc def Hello World Hello world
# fstring 42 bob    42 3.14 'bob'
# percent 5 a   3.1 ff 50%
# percent_multi 1 a 2.2 ab      |
# ops ababab   abcd True True
# cmp True True True True
# iadd_immutable ab abcd AB
# iter_len ['a', 'b', 'c'] 3 5
# ord_chr 65 A 233 é
# cp_sem 5 ['h', 'é', 'l', 'l', 'o'] él 1 hello
# cp_cmp True True True
