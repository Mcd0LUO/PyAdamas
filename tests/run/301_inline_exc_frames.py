# 内联与异常语义钉子（PYADAMAS_INLINE_OPT）：可内联小 helper 里抛异常，
# 调用方捕获后状态必须正确（帧展开/RC）；自递归函数体内有 raise 时不得
# 被递归展开跳过（每层递归的异常都要照常抛出/捕获）。
def helper(n: int) -> int:
    if n % 2 == 0:
        raise ValueError("even")
    return n * 3


def wrapper(n: int) -> int:
    return helper(n)


raised: int = 0
returned: int = 0
checksum: int = 0
for i in range(2000):
    try:
        checksum += wrapper(i)
        returned += 1
    except ValueError as e:
        raised += 1
        checksum += len(str(e))
print(raised, returned, checksum)


def rec(n: int) -> int:
    if n == 0:
        raise ValueError("bottom")
    return rec(n - 1) + 1


caught: int = 0
for depth in range(1, 40):
    try:
        rec(depth)
    except ValueError:
        caught += 1
print(caught)


def rec_ok(n: int) -> int:
    if n <= 1:
        return n
    return rec_ok(n - 1) + rec_ok(n - 2)


print(rec_ok(18))

# --- expect ---
# 1000 1000 3004000
# 39
# 2584
