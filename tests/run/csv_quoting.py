# csv 引号/分隔符口径（CPython 实跑取期望；真实文件，两边同一份 API）。
# 未钉桩：QUOTE_NONE 的错误文本与非法 delimiter 的异常类型是既有差异
#（CPython: TypeError/"need to escape, but no escapechar set"；pa: csv.Error），
# 见 docs/stdlib-round2.md。
import csv
import os
import tempfile

path = os.path.join(tempfile.mkdtemp(), "t.csv")
rows = [["a", "b,c", "d\"e", ""], ["x y", "z", "1", "2"]]

f = open(path, "w")
w = csv.writer(f)
w.writerows(rows)
f.close()
f = open(path, "r")
s = f.read()
f.close()
print(s.replace("\r\n", "<CRLF>"))

f = open(path, "w")
w2 = csv.writer(f, delimiter=";", quotechar="'", lineterminator="\n")
w2.writerows(rows)
f.close()
f = open(path, "r")
s = f.read()
f.close()
print(s.replace("\n", "<LF>"))

f = open(path, "w")
w3 = csv.writer(f, quoting=csv.QUOTE_ALL)
w3.writerow(["a", "b"])
f.close()
f = open(path, "r")
s = f.read()
f.close()
print(s.replace("\r\n", "<CRLF>"))

f = open(path, "w")
f.write("a,b\r\n1,2\n\n3,4\n")
f.close()
f = open(path, "r")
n = 0
for row in csv.reader(f):
    print(len(row), "-".join(row))
    n += 1
print("rows", n)
f.close()

f = open(path, "w")
f.write("a|b\n1|2\n")
f.close()
f = open(path, "r")
for row in csv.reader(f, delimiter="|"):
    print("-".join(row))
f.close()

# --- expect ---
# a,"b,c","d""e",
# x y,z,1,2
# 
# a;b,c;d"e;<LF>x y;z;1;2<LF>
# "a","b"
# 
# 2 a-b
# 2 1-2
# 0 
# 2 3-4
# rows 4
# a-b
# 1-2
