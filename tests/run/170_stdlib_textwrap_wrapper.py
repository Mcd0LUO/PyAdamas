"""stdlib/textwrap.py：TextWrapper 类的 wrap/fill 与缩进。"""
import textwrap

w = textwrap.TextWrapper(width=9)
for line in w.wrap("one two three four"):
    print(line)
print(w.fill("aaa bbb ccc"))

w2 = textwrap.TextWrapper(width=12, initial_indent="  ")
for line in w2.wrap("one two three four five"):
    print("[" + line + "]")

print(textwrap.fill("one two three", 7))

# --- expect ---
# one two
# three
# four
# aaa bbb
# ccc
# [  one two]
# [three four]
# [five]
# one two
# three
