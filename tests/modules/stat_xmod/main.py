import dmod

s = dmod.st(__file__)
print(s.st_size >= 0)

# --- expect ---
# True
