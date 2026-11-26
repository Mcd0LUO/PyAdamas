# 0.613：go 的实参绑定与普通调用一致（默认参数 / 关键字 / *args）
# 逐个 join 保证输出顺序确定（并发下顺序不定，测试要可比）。

def worker(x: int, retry: int = 3) -> None:
    print("worker", x, retry)

def many(*args) -> None:
    print("many", len(args))

join(go worker(1))
join(go worker(x=5, retry=7))
join(go many(1, 2, 3))

# --- expect ---
# worker 1 3
# worker 5 7
# many 3
