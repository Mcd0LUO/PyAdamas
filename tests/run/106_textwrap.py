# 标准库 textwrap 子集
import textwrap

print(textwrap.fill("the quick brown fox jumps over the lazy dog", 15))
print(textwrap.wrap("a b c", 5))
print(textwrap.dedent("    a\n      b\n"))
print(textwrap.indent("a\n\nb", "> "))
print(textwrap.fill("single", 40))

# --- expect ---
# the quick brown
# fox jumps over
# the lazy dog
# ['a b c']
# a
#   b
#
# > a
#
# > b
# single
