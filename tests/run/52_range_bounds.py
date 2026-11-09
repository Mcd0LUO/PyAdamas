def sum_self(a: list[int]) -> int:
    s = 0
    for k in range(len(a)):
        s = s + a[k]
    return s


def sum_double(a: list[int]) -> int:
    s = 0
    for k in range(len(a)):
        s = s + a[k] * a[k]
    return s


def write_inside(a: list[int]) -> int:
    s = 0
    for k in range(len(a)):
        a[k] = a[k] + 1
        s = s + a[k]
    return s


def append_inside(a: list[int]) -> int:
    s = 0
    for k in range(len(a)):
        s = s + a[k]
        a.append(1)
    return s


def reassign(a: list[int]) -> int:
    s = 0
    b = [9, 9]
    for k in range(len(a)):
        s = s + a[k]
        a = b
    return s


def reassign_k(a: list[int]) -> int:
    s = 0
    for k in range(len(a)):
        s = s + a[k]
        k = 99
    return s


def nested(a: list[int]) -> int:
    s = 0
    for k in range(len(a)):
        for j in range(len(a)):
            s = s + a[k] * a[j]
    return s


def start_len(a: list[int]) -> int:
    s = 0
    for k in range(0, len(a)):
        s = s + a[k]
    return s


print(sum_self([1, 2, 3, 4]))
print(sum_double([1, 2, 3]))
print(write_inside([0, 0, 0]))
print(append_inside([1, 2]))
print(reassign([5, 6]))
print(reassign_k([7, 8, 9]))
print(nested([1, 2]))
print(start_len([3, 4, 5]))

# --- expect ---
# 10
# 14
# 3
# 3
# 14
# 24
# 9
# 12
