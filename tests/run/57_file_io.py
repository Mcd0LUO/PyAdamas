path = "/tmp/py23_io_test.txt"
f = open(path, "w")
print(f.write("hello\n"), f.write("world\n"))
f.close()

g = open(path, "r")
print(g.readline().strip())
print(len(g.readlines()))
g.close()

with open(path, "a") as w:
    w.write("tail\n")

h = open(path)
data = h.read()
print(len(data))
h.close()

h2 = open(path)
print(h2.read(5))
h2.close()

fe = open(path, encoding="utf-8")
print(len(fe.read()))
fe.close()

ok = 0
try:
    open("/tmp/py23_missing_input.txt")
except OSError:
    ok = 1
print("missing", ok)

# --- expect ---
# 6 6
# hello
# 1
# 17
# hello
# 17
# missing 1
