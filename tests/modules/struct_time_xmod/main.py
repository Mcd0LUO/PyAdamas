import dmod

t = dmod.epoch()
print(t.tm_year, t.tm_mon, t.tm_mday)

# --- expect ---
# 1970 1 1
