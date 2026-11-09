fs: list[float] = []
for i in range(1000):
    fs.append(1.0 / (i + 1))
print(sum(fs))

# --- expect ---
# 7.485470860550343
