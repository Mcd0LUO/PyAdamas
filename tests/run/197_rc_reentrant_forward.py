# 旧全局生成器归零会重入 finally，覆写刚发布的新值。
# 同块下一次读取必须重新装载全局，不能把被替换且已释放的值转发给 next。
def stream(n: int):
    try:
        yield n
        yield n * 10
    finally:
        if n == 1:
            install(3)

current = stream(1)

def install(n: int) -> None:
    global current
    current = stream(n)

def replace_and_read() -> int:
    global current
    current = stream(2)
    return next(current)

print(next(current))
print(replace_and_read())
print(next(current))

# --- expect ---
# 1
# 3
# 30
