import dmod

s = dmod.make("zz")
print(s.getvalue())
print(s.read(1))

# --- expect ---
# zz
# z
