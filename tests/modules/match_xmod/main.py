import dmod

m = dmod.find("baa")
if m is not None:
    print(m.group(1))

# --- expect ---
# aa
