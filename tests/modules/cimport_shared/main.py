import helper
cimport clib as c

print(helper.root(9.0))
print(helper.pi())
print(c.sqrt(25.0))

# --- expect ---
# 3.0
# 3.141592653589793
# 5.0
