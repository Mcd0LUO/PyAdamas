# re 锚点矩阵（0.493）：Pattern.match 必须从 0 起（隐式 \A），search 保持扫描；
# \A / \Z / \z 是锚点，^ / $ 保持原有语义。expect 段来自 CPython 3.14.4 实跑。
import re

PATS: list[str] = [
    "a.c",
    "[a-z]",
    "abc",
    "a*",
    "a+",
    "ab{2,3}",
    "\\Aa",
    "a\\Z",
    "a\\z",
    "^a",
    "a$",
    "a|b",
    "(ab)+",
    "[0-9]+$",
    "\\A[0-9]+\\Z",
    "a.c$",
]

SUBS: list[str] = [
    "",
    "a",
    "abc",
    "xabc",
    "1abc",
    "aa",
    "aaa",
    "abb",
    "abbb",
    "cab",
    "a1",
    "123",
    "x",
    "abcx",
]

i: int = 0
while i < len(PATS):
    p: str = PATS[i]
    r = re.compile(p)
    j: int = 0
    while j < len(SUBS):
        s: str = SUBS[j]
        print(p, "|", s, "|", r.match(s) is not None,
              r.search(s) is not None, r.fullmatch(s) is not None)
        j = j + 1
    i = i + 1
# --- expect ---
# a.c |  | False False False
# a.c | a | False False False
# a.c | abc | True True True
# a.c | xabc | False True False
# a.c | 1abc | False True False
# a.c | aa | False False False
# a.c | aaa | False False False
# a.c | abb | False False False
# a.c | abbb | False False False
# a.c | cab | False False False
# a.c | a1 | False False False
# a.c | 123 | False False False
# a.c | x | False False False
# a.c | abcx | True True False
# [a-z] |  | False False False
# [a-z] | a | True True True
# [a-z] | abc | True True False
# [a-z] | xabc | True True False
# [a-z] | 1abc | False True False
# [a-z] | aa | True True False
# [a-z] | aaa | True True False
# [a-z] | abb | True True False
# [a-z] | abbb | True True False
# [a-z] | cab | True True False
# [a-z] | a1 | True True False
# [a-z] | 123 | False False False
# [a-z] | x | True True True
# [a-z] | abcx | True True False
# abc |  | False False False
# abc | a | False False False
# abc | abc | True True True
# abc | xabc | False True False
# abc | 1abc | False True False
# abc | aa | False False False
# abc | aaa | False False False
# abc | abb | False False False
# abc | abbb | False False False
# abc | cab | False False False
# abc | a1 | False False False
# abc | 123 | False False False
# abc | x | False False False
# abc | abcx | True True False
# a* |  | True True True
# a* | a | True True True
# a* | abc | True True False
# a* | xabc | True True False
# a* | 1abc | True True False
# a* | aa | True True True
# a* | aaa | True True True
# a* | abb | True True False
# a* | abbb | True True False
# a* | cab | True True False
# a* | a1 | True True False
# a* | 123 | True True False
# a* | x | True True False
# a* | abcx | True True False
# a+ |  | False False False
# a+ | a | True True True
# a+ | abc | True True False
# a+ | xabc | False True False
# a+ | 1abc | False True False
# a+ | aa | True True True
# a+ | aaa | True True True
# a+ | abb | True True False
# a+ | abbb | True True False
# a+ | cab | False True False
# a+ | a1 | True True False
# a+ | 123 | False False False
# a+ | x | False False False
# a+ | abcx | True True False
# ab{2,3} |  | False False False
# ab{2,3} | a | False False False
# ab{2,3} | abc | False False False
# ab{2,3} | xabc | False False False
# ab{2,3} | 1abc | False False False
# ab{2,3} | aa | False False False
# ab{2,3} | aaa | False False False
# ab{2,3} | abb | True True True
# ab{2,3} | abbb | True True True
# ab{2,3} | cab | False False False
# ab{2,3} | a1 | False False False
# ab{2,3} | 123 | False False False
# ab{2,3} | x | False False False
# ab{2,3} | abcx | False False False
# \Aa |  | False False False
# \Aa | a | True True True
# \Aa | abc | True True False
# \Aa | xabc | False False False
# \Aa | 1abc | False False False
# \Aa | aa | True True False
# \Aa | aaa | True True False
# \Aa | abb | True True False
# \Aa | abbb | True True False
# \Aa | cab | False False False
# \Aa | a1 | True True False
# \Aa | 123 | False False False
# \Aa | x | False False False
# \Aa | abcx | True True False
# a\Z |  | False False False
# a\Z | a | True True True
# a\Z | abc | False False False
# a\Z | xabc | False False False
# a\Z | 1abc | False False False
# a\Z | aa | False True False
# a\Z | aaa | False True False
# a\Z | abb | False False False
# a\Z | abbb | False False False
# a\Z | cab | False False False
# a\Z | a1 | False False False
# a\Z | 123 | False False False
# a\Z | x | False False False
# a\Z | abcx | False False False
# a\z |  | False False False
# a\z | a | True True True
# a\z | abc | False False False
# a\z | xabc | False False False
# a\z | 1abc | False False False
# a\z | aa | False True False
# a\z | aaa | False True False
# a\z | abb | False False False
# a\z | abbb | False False False
# a\z | cab | False False False
# a\z | a1 | False False False
# a\z | 123 | False False False
# a\z | x | False False False
# a\z | abcx | False False False
# ^a |  | False False False
# ^a | a | True True True
# ^a | abc | True True False
# ^a | xabc | False False False
# ^a | 1abc | False False False
# ^a | aa | True True False
# ^a | aaa | True True False
# ^a | abb | True True False
# ^a | abbb | True True False
# ^a | cab | False False False
# ^a | a1 | True True False
# ^a | 123 | False False False
# ^a | x | False False False
# ^a | abcx | True True False
# a$ |  | False False False
# a$ | a | True True True
# a$ | abc | False False False
# a$ | xabc | False False False
# a$ | 1abc | False False False
# a$ | aa | False True False
# a$ | aaa | False True False
# a$ | abb | False False False
# a$ | abbb | False False False
# a$ | cab | False False False
# a$ | a1 | False False False
# a$ | 123 | False False False
# a$ | x | False False False
# a$ | abcx | False False False
# a|b |  | False False False
# a|b | a | True True True
# a|b | abc | True True False
# a|b | xabc | False True False
# a|b | 1abc | False True False
# a|b | aa | True True False
# a|b | aaa | True True False
# a|b | abb | True True False
# a|b | abbb | True True False
# a|b | cab | False True False
# a|b | a1 | True True False
# a|b | 123 | False False False
# a|b | x | False False False
# a|b | abcx | True True False
# (ab)+ |  | False False False
# (ab)+ | a | False False False
# (ab)+ | abc | True True False
# (ab)+ | xabc | False True False
# (ab)+ | 1abc | False True False
# (ab)+ | aa | False False False
# (ab)+ | aaa | False False False
# (ab)+ | abb | True True False
# (ab)+ | abbb | True True False
# (ab)+ | cab | False True False
# (ab)+ | a1 | False False False
# (ab)+ | 123 | False False False
# (ab)+ | x | False False False
# (ab)+ | abcx | True True False
# [0-9]+$ |  | False False False
# [0-9]+$ | a | False False False
# [0-9]+$ | abc | False False False
# [0-9]+$ | xabc | False False False
# [0-9]+$ | 1abc | False False False
# [0-9]+$ | aa | False False False
# [0-9]+$ | aaa | False False False
# [0-9]+$ | abb | False False False
# [0-9]+$ | abbb | False False False
# [0-9]+$ | cab | False False False
# [0-9]+$ | a1 | False True False
# [0-9]+$ | 123 | True True True
# [0-9]+$ | x | False False False
# [0-9]+$ | abcx | False False False
# \A[0-9]+\Z |  | False False False
# \A[0-9]+\Z | a | False False False
# \A[0-9]+\Z | abc | False False False
# \A[0-9]+\Z | xabc | False False False
# \A[0-9]+\Z | 1abc | False False False
# \A[0-9]+\Z | aa | False False False
# \A[0-9]+\Z | aaa | False False False
# \A[0-9]+\Z | abb | False False False
# \A[0-9]+\Z | abbb | False False False
# \A[0-9]+\Z | cab | False False False
# \A[0-9]+\Z | a1 | False False False
# \A[0-9]+\Z | 123 | True True True
# \A[0-9]+\Z | x | False False False
# \A[0-9]+\Z | abcx | False False False
# a.c$ |  | False False False
# a.c$ | a | False False False
# a.c$ | abc | True True True
# a.c$ | xabc | False True False
# a.c$ | 1abc | False True False
# a.c$ | aa | False False False
# a.c$ | aaa | False False False
# a.c$ | abb | False False False
# a.c$ | abbb | False False False
# a.c$ | cab | False False False
# a.c$ | a1 | False False False
# a.c$ | 123 | False False False
# a.c$ | x | False False False
# a.c$ | abcx | False False False
