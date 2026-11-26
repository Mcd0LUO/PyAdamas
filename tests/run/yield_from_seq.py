# 0.615：yield from 支持 range / bytes / list（原来 range、bytes 直接报错）

def g():
    yield from range(3)
    yield from b"ab"
    yield from [7, 8]
    yield from (9,)

print(list(g()))

# --- expect ---
# [0, 1, 2, 97, 98, 7, 8, 9]
