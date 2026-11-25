import dmod

p = dmod.spawn()
p.wait()
print(p.returncode)

# --- expect ---
# 0
