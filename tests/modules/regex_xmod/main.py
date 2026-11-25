import dmod

r = dmod.make()
print(r.pattern)
m = r.search("baa")
if m is not None:
    print(m.group(0))

# --- expect ---
# a+
# aa
