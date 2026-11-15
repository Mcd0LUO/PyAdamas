"""stdlib/csv.py：DictWriter 写 / DictReader 读（函数式，返回 list[dict]）。"""
import csv

path = "/tmp/pa_csv_dict.csv"
f = open(path, "w")
w = csv.DictWriter(f, ["a", "b"])
w.writeheader()
w.writerow({"a": "1", "b": "2"})
w.writerows([{"a": "3", "b": "4"}])
f.close()

g = open(path)
rows = list(csv.DictReader(g))
print(len(rows), rows[0]["a"], rows[0]["b"], rows[1]["a"], rows[1]["b"])

h = open(path)
for row in csv.DictReader(h):
    print(row["b"])

# --- expect ---
# 2 1 2 3 4
# 2
# 4
