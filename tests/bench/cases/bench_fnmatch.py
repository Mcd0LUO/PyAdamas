"""fnmatch 基准：filter 多模式（每个模式一次 translate + compile）。"""
import fnmatch

pats = ["*.txt", "data_?.csv", "[abc]*.log", "src/*/[a-z]*.py", "a*[0-9]"]
names = ["a.txt", "b.csv", "data_1.csv", "abc.log", "src/x/foo.py", "a1"]

total = 0
r = 0
while r < 6000:
    for p in pats:
        total += len(fnmatch.filter(names, p))
    r += 1
print(total)
