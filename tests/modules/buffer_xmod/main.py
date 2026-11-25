import dmod

b = dmod.make(4)
print(len(b), b[0])
b[1] = 2.5
print(b[1])

# --- expect ---
# 4 7.5
# 2.5
