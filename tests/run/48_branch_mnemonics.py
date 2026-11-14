import math


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

nan = math.nan
print(nan == nan, nan != nan, nan < 1.0, nan <= 1.0, nan > 1.0, nan >= 1.0)
if nan != nan:
    print("ne-true")
else:
    print("ne-false")
if nan == nan:
    print("eq-true")
else:
    print("eq-false")
if nan < 1.0:
    print("lt-true")
else:
    print("lt-false")
if nan >= 1.0:
    print("ge-true")
else:
    print("ge-false")

# --- expect ---
# 1 2 1 2 2 1 2 1
# 13
# False True False False False False
# ne-true
# eq-false
# lt-false
# ge-false
