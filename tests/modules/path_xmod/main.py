import dmod

p = dmod.make('/tmp/a.txt')
print(p.name, p.suffix)

# --- expect ---
# a.txt .txt
