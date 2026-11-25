import dmod

u = dmod.info()
print(len(u.sysname) > 0)
print(u.machine == u.machine)

# --- expect ---
# True
# True
