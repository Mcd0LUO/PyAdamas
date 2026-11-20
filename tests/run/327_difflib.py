# difflib 子集：SequenceMatcher / get_close_matches / unified_diff
import difflib

sm = difflib.SequenceMatcher(None, "abcd", "bcde")
print(repr(sm.find_longest_match(0, 4, 0, 4)))
print(sm.ratio(), sm.quick_ratio(), sm.real_quick_ratio())
print(sm.get_matching_blocks())
print(sm.get_opcodes())
sm.set_seqs("abc", "abd")
print(repr(sm.find_longest_match(0, 3, 0, 3)))
print(sm.get_opcodes())
print(sm.ratio())

sm2 = difflib.SequenceMatcher(None, [1, 2, 3], [2, 3, 4])
print(sm2.get_opcodes())
print(sm2.ratio())
print(difflib.SequenceMatcher(None, "", "").ratio())
print(difflib.SequenceMatcher(None, "abc", "abc").ratio())

print(difflib.get_close_matches("apple", ["ape", "appel", "banana", "pineapple"]))
print(difflib.get_close_matches("apple", ["ape", "appel", "banana"], 2, 0.6))
print(difflib.get_close_matches("zzz", ["a", "b"], 3, 0.6))

a = ["one", "two", "three", "four"]
b = ["one", "TWO", "three", "four", "five"]
for line in difflib.unified_diff(a, b, "a.txt", "b.txt"):
    print(line, end="")
print("---")
for line in difflib.unified_diff(a, b, lineterm="|"):
    print(line, end="")

print("---")
a2 = ["x", "a", "b", "c", "y"]
b2 = ["x", "a", "c", "y"]
for line in difflib.unified_diff(a2, b2, n=1):
    print(line, end="")


print(difflib.SequenceMatcher(None, "aaaa", "aa").quick_ratio())
print(difflib.SequenceMatcher(None, "abcd", "abcd").get_opcodes())
sm3 = difflib.SequenceMatcher(None, ["a", "b", "c", "d", "e", "f"], ["a", "c", "e", "f"])
for g in sm3.get_grouped_opcodes(1):
    print(g)
print(sm3.find_longest_match(1, 6, 1, 4))
sm4 = difflib.SequenceMatcher(lambda x: x == " ", "a b c", "a c")
print(sm4.ratio(), sm4.get_matching_blocks())
print(difflib.get_close_matches("cat", ["dog", "cat", "cart", "coat", "cut"], 2))


big: list[str] = []
i = 0
while i < 200:
    big.append("x")
    i += 1
big.append("q")
probe = ["q", "x", "q"]
sm5 = difflib.SequenceMatcher(None, probe, big)
print(sm5.find_longest_match(0, 3, 0, 201))
print(round(sm5.ratio(), 4))
sm6 = difflib.SequenceMatcher(None, probe, big, False)
print(sm6.find_longest_match(0, 3, 0, 201))
print(round(sm6.ratio(), 4))


# --- expect ---
# Match(a=1, b=0, size=3)
# 0.75 0.75 1.0
# [Match(a=1, b=0, size=3), Match(a=4, b=4, size=0)]
# [('delete', 0, 1, 0, 0), ('equal', 1, 4, 0, 3), ('insert', 4, 4, 3, 4)]
# Match(a=0, b=0, size=2)
# [('equal', 0, 2, 0, 2), ('replace', 2, 3, 2, 3)]
# 0.6666666666666666
# [('delete', 0, 1, 0, 0), ('equal', 1, 3, 0, 2), ('insert', 3, 3, 2, 3)]
# 0.6666666666666666
# 1.0
# 1.0
# ['appel', 'ape', 'pineapple']
# ['appel', 'ape']
# []
# --- a.txt
# +++ b.txt
# @@ -1,4 +1,5 @@
#  one-two+TWO three four+five---
# --- |+++ |@@ -1,4 +1,5 @@| one-two+TWO three four+five---
# --- 
# +++ 
# @@ -2,3 +2,2 @@
#  a-b c0.6666666666666666
# [('equal', 0, 4, 0, 4)]
# [('equal', 0, 1, 0, 1), ('delete', 1, 2, 1, 1), ('equal', 2, 3, 1, 2), ('delete', 3, 4, 2, 2), ('equal', 4, 5, 2, 3)]
# Match(a=4, b=2, size=2)
# 0.75 [Match(a=0, b=0, size=2), Match(a=4, b=2, size=1), Match(a=5, b=3, size=0)]
# ['cat', 'coat']
# Match(a=0, b=200, size=1)
# 0.0098
# Match(a=1, b=199, size=2)
# 0.0196
