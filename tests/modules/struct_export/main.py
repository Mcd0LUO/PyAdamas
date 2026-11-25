import smod

p = smod.make(3.0, 4.0)
print(p.x, p.y)
print(smod.norm2(p))
p.x = 6.0
print(p.x, p.y)
print(smod.norm2(p))

# --- expect ---
# 3.0 4.0
# 25.0
# 6.0 4.0
# 52.0
