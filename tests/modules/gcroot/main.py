import store

junk: list[str] = []
for i in range(200000):
    junk.append("a" + "b")
print(store.size())
store.data.append("c")
print(store.size(), store.data[2])

# --- expect ---
# 2
# 3 c
