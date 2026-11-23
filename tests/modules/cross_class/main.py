import base
import derived
import chainmod

p: base.Point = base.make(3, 4)
print("read", p.x, p.y)
p.x = 9
p.y = 16
print("write", p.x, p.y, p.norm())
q: base.Point = base.Point(5, 6)
print("ctor", q.x, q.y, q.norm())
q.move(-1, -2)
print("move", q.x, q.y)
d: derived.Point3 = derived.Point3(1, 2, 3)
print("derived", d.x, d.y, d.z, d.norm(), d.norm3())
d.x = 10
print("derived_write", d.x, d.norm3())
u = base.make(7, 8)
print("untyped", u.x, u.y, u.norm())
v = base.Point(2, 5)
print("untyped_ctor", v.x, v.norm())
h: chainmod.Holder = chainmod.Holder(chainmod.Box(42))
print("chain_read", h.b.value)
h.b.value = 43
print("chain_write", h.b.value)

h2 = chainmod.Holder(chainmod.Box(7))
print("chain_untyped", h2.b.value)
h2.b.value = 8
print("chain_untyped_write", h2.b.value)
from base import Point as P
pp: P = P(1, 1)
print("from_import", pp.x, pp.norm())
pp.y = 2
print("from_import_write", pp.y, pp.norm())


# --- expect ---
# read 3 4
# write 9 16 337
# ctor 5 6 61
# move 4 4
# derived 1 2 3 5 14
# derived_write 10 113
# untyped 7 8 113
# untyped_ctor 2 29
# chain_read 42
# chain_write 43
# chain_untyped 7
# chain_untyped_write 8
# from_import 1 2
# from_import_write 2 5
