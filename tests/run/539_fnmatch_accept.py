# fnmatch 常用 API 对拍。
import fnmatch
print("f1", fnmatch.fnmatch("hello.txt", "*.txt"))
print("f2", fnmatch.fnmatch("hello.py", "*.txt"))
print("f3", fnmatch.fnmatchcase("ABC.txt", "*.TXT"))
print("f4", fnmatch.fnmatchcase("abc.txt", "*.txt"))
print("fil", fnmatch.filter(["a.txt","b.py","c.txt"], "*.txt"))
print("esc", fnmatch.translate("*")[:6])

# --- expect ---
# f1 True
# f2 False
# f3 False
# f4 True
# fil ['a.txt', 'c.txt']
# esc (?s:.*
