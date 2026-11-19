# fnmatch 模块（stdlib/fnmatch.py）：shell 通配符匹配，翻译成正则后交给 re。
import fnmatch

print(fnmatch.fnmatch("foo.py", "*.py"))
print(fnmatch.fnmatch("foo.txt", "*.py"))
print(fnmatch.fnmatch("abc", "a?c"))
print(fnmatch.fnmatch("abc", "a[bd]c"))
print(fnmatch.fnmatch("axc", "a[!bd]c"))
print(fnmatch.filter(["a.py", "b.txt", "c.py"], "*.py"))
print(fnmatch.fnmatchcase("ABC", "abc"))
print(fnmatch.fnmatchcase("abc", "abc"))

# --- expect ---
# True
# False
# True
# True
# True
# ['a.py', 'c.py']
# False
# True
