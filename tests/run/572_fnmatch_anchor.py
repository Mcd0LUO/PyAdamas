# fnmatch 匹配口径（CPython 实跑取期望）。
# 注意：translate() 的正则文本不逐字对齐 CPython 3.14（CPython 用 \z 与原子组
# (?>.*?b)，pa 的 re 用 \Z 且无原子组），这里只钉**匹配结果**这一可观测语义。
import fnmatch

names = ["a.txt", "abc", "ax", "bx", "cx", "a[", "x\\y", "", ".hidden"]
npat = ["*.txt", "a?c", "[abc]x", "[!abc]x", "[a-z]*", "[]]", "abc[", "a*b*c",
        "*", "?", "[[]x", "**", "x\\y", "a[!]", "a[^b]c"]
for p in npat:
    print(p, len(fnmatch.filter(names, p)))
    hits: list[int] = []
    k = 0
    while k < len(names):
        if fnmatch.fnmatchcase(names[k], p):
            hits.append(k)
        k += 1
    print(hits)

# --- expect ---
# *.txt 1
# [0]
# a?c 1
# [1]
# [abc]x 3
# [2, 3, 4]
# [!abc]x 0
# []
# [a-z]* 7
# [0, 1, 2, 3, 4, 5, 6]
# []] 0
# []
# abc[ 0
# []
# a*b*c 1
# [1]
# * 9
# [0, 1, 2, 3, 4, 5, 6, 7, 8]
# ? 0
# []
# [[]x 0
# []
# ** 9
# [0, 1, 2, 3, 4, 5, 6, 7, 8]
# x\y 1
# [6]
# a[!] 0
# []
# a[^b]c 1
# [1]
