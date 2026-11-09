def le(x: int) -> int:
    if x <= 3:
        return 1
    return 2


def lt(x: int) -> int:
    if x < 3:
        return 1
    return 2


def gt(x: int) -> int:
    if x > 3:
        return 1
    return 2


def ge(x: int) -> int:
    if x >= 3:
        return 1
    return 2


print(le(2), le(4), lt(2), lt(4), gt(2), gt(4), ge(2), ge(4))

total = 0
for i in range(6):
    if i <= 2:
        total = total + i
    elif i >= 5:
        total = total + 10
print(total)

# --- expect ---
# 1 2 1 2 2 1 2 1
# 13
