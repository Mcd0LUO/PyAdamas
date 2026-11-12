# csv：纯 py23 stdlib 模块（reader -> list[list[str]]，writer 类）
import csv
from csv import reader, writer

path = "/tmp/py23_csv_run.csv"

f = open(path, "w", newline="")
w = csv.writer(f)
w.writerow(["name", "note"])
w.writerow(["alice", "a,b"])
w.writerow(["bob", 'say "hi"'])
w.writerows([["carol", "line1\nline2"], ["dave", ""]])
f.close()

f = open(path, "r", newline="")
print(csv.reader(f))
f.close()

f = open(path, "r", newline="")
print(reader(f))
f.close()

f = open(path, "w", newline="")
w2 = csv.writer(f, delimiter=";", quoting=csv.QUOTE_ALL, lineterminator="\n")
w2.writerows([["a", "b;c"], ["", "q"]])
f.close()

f = open(path, "r", newline="")
print(csv.reader(f, delimiter=";"))
f.close()

f = open(path, "w", newline="")
f.write("a,b\n\nc,\n")
f.close()

f = open(path, "r", newline="")
print(csv.reader(f))
f.close()

f = open(path, "w", newline="")
f.write('"unterminated\n')
f.close()

f = open(path, "r", newline="")
try:
    bad = csv.reader(f)
    print(bad)
except ValueError as e:
    print("ValueError", e)
f.close()

f = open(path, "w", newline="")
w3 = writer(f, lineterminator="\n")
w3.writerow([])
w3.writerow(["a", "b"])
f.close()

f = open(path, "r", newline="")
print(csv.reader(f))
f.close()
# --- expect ---
# [['name', 'note'], ['alice', 'a,b'], ['bob', 'say "hi"'], ['carol', 'line1\nline2'], ['dave', '']]
# [['name', 'note'], ['alice', 'a,b'], ['bob', 'say "hi"'], ['carol', 'line1\nline2'], ['dave', '']]
# [['a', 'b;c'], ['', 'q']]
# [['a', 'b'], [], ['c', '']]
# ValueError unexpected end of data
# [[], ['a', 'b']]
