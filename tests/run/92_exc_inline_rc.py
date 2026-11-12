# 异常展开 + 内联：被跳过帧里的堆槽引用必须回收，正常返回后槽要清零，
# 否则循环里复用同一槽位会漏引用或二次释放。输出必须与 CPython 一致。
def repeat(unit: str, n: int) -> str:
    out: str = ""
    i: int = 0
    while i < n:
        out = out + unit
        i += 1
    return out


def raise_on_even(n: int) -> int:
    text: str = repeat("ab", 3)
    nums: list[int] = list(range(n % 3 + 2))
    if n % 2 == 0:
        raise ValueError(text)
    return len(text) + len(nums)


def wrapper(n: int) -> int:
    return raise_on_even(n)


i: int = 0
raised: int = 0
returned: int = 0
checksum: int = 0
while i < 2000:
    try:
        checksum += wrapper(i)
        returned += 1
    except ValueError as e:
        raised += 1
        checksum += len(repeat("z", 3))
    i += 1

print(raised, returned, checksum)


def checked(n: int) -> int:
    words: list[str] = ["a", "b", "c"]
    text: str = repeat("q", 2)
    return len(text) + len(words) + n


total: int = 0
j: int = 0
while j < 1000:
    total += checked(j)
    j += 1
print(total)


# 递归函数不会被内联：异常跳过的是真实调用帧。局部堆值必须被帧记录
# 释放，只读形参 tag 是借用的，不能被释放。
def frame_leak(n: int, tag: str) -> int:
    payload: list[int] = list(range(n % 3 + 2))
    text: str = tag + "!"
    if n % 2 == 0:
        raise ValueError(text)
    if n > 1:
        return frame_leak(n - 1, tag)
    return len(text) + len(payload)


caught: int = 0
returns: int = 0
seq: int = 0
k: int = 0
while k < 500:
    try:
        seq += frame_leak(k % 6, "n" + str(k % 4 + 1))
        returns += 1
    except ValueError:
        caught += 1
    k += 1
print(caught, returns, seq)


# 被覆盖过的形参槽：内联体在同一个函数的 handler 里被捕获，换轮时形参
# 槽里的旧引用要在被实参覆盖前释放（不经过帧展开）。
def overwritten(s: list[int], n: int) -> int:
    if n < 0:
        s = []
    if n % 2 == 0:
        raise ValueError("b")
    return len(s)


ok: int = 0
m: int = 0
while m < 500:
    try:
        ok += overwritten([1, 2, 3], m)
    except ValueError:
        pass
    m += 1
print(ok)

# --- expect ---
# 1000 1000 12000
# 504500
# 416 84 504
# 750
