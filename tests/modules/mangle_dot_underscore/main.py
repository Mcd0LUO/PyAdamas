import a_b
import a.b as ab

print(a_b.f())
print(ab.f())

# --- expect ---
# 11
# 22
