"""string / textwrap 基准：字符串方法 + 换行与缩写。"""
import string
import textwrap

one = "the quick brown fox jumps over the lazy dog "
text = ""
for _ in range(20):
    text += one

total = 0
for _ in range(2000):
    total += len(string.capwords("hello world from pyadamas"))
    total += len(text.upper())
    total += text.count("the")
    total += len(text.split())
    total += len(textwrap.wrap(text, 40))
    total += len(textwrap.shorten(text, 60))
print(total)
