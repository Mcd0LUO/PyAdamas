p = "/tmp/pyadamas_fio_506.txt"
w = open(p, "w")
print("write", w.write("hello\nworld\n"))
w.close()
r = open(p, "r")
print("read_all", repr(r.read()))
r.close()
r2 = open(p, "r")
print("readline", repr(r2.readline()), repr(r2.readline()), repr(r2.readline()))
r2.close()
lines = []
for ln in open(p):
    lines.append(ln)
print("iter", lines)
a = open(p, "a")
a.write("tail\n")
a.close()
print("append", open(p).readlines())
with open(p, "w") as wf:
    wf.write("ctx")
s2 = 0
try:
    wf.write("after")
except ValueError:
    s2 = 1
print("with_closed", s2)
prop = 0
try:
    with open(p, "w") as ef:
        ef.write("bad")
        raise ValueError("x")
except ValueError:
    prop = 1
print("with_exc", prop)
enc = "/tmp/pyadamas_fio_506_lat.txt"
e = open(enc, "w", encoding="latin-1")
e.write("caf\u00e9")
e.close()
print("latin", open(enc, "r", encoding="latin-1").read())
ud = 0
try:
    open(enc, "r", encoding="utf-8").read()
except UnicodeDecodeError:
    ud = 1
print("strict_decode", ud)
class Ctx:
    def __enter__(self) -> int:
        return 7

    def __exit__(self, e1, e2, e3) -> bool:
        return True


with Ctx() as v:
    print("cm", v)
print("cm_exit_ok", 1)
import os
os.remove(p)
os.remove(enc)
print("cleanup", 1)

# --- expect ---
# write 12
# read_all 'hello\nworld\n'
# readline 'hello\n' 'world\n' ''
# iter ['hello\n', 'world\n']
# append ['hello\n', 'world\n', 'tail\n']
# with_closed 1
# with_exc 1
# latin café
# strict_decode 1
# cm 7
# cm_exit_ok 1
# cleanup 1
