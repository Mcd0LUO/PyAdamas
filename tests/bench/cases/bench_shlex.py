"""shlex 基准：split 命令行 + quote/join 词表。"""
import shlex

line = "cmd -a 1 --flag='x y' \"q q\" plain text here"
words = ["alpha", "beta gamma", "it's", "plain", "-x=1", "", "a/b"]

total = 0
r = 0
while r < 4000:
    total += len(shlex.split(line))
    total += len(shlex.join(words))
    r += 1
print(total)
