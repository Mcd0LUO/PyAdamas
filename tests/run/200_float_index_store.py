# 浮点列表写入：正常、负下标和两个方向的越界。
# 堆元素列表也应保留覆盖前的所有权处理。
def put(a: list[float], i: int, value: float) -> None:
    a[i] = value


def must_fail(a: list[float], i: int) -> None:
    try:
        put(a, i, 99.5)
    except IndexError:
        print("out of range", i)


floats = [1.25, 2.5, 3.75]
put(floats, 1, 8.25)
put(floats, -3, -4.5)
put(floats, -1, 0.125)
must_fail(floats, 3)
must_fail(floats, -4)
print(floats[0], floats[1], floats[2])


def put_heap(a: list[str], i: int, value: str) -> None:
    a[i] = value


def make_text(i: int) -> str:
    return "dynamic-" + str(i)


texts = [make_text(1), make_text(2)]
kept = texts[0]
put_heap(texts, 0, make_text(3))
put_heap(texts, -1, make_text(4))
print(kept, texts[0], texts[1])

# --- expect ---
# out of range 3
# out of range -4
# -4.5 8.25 0.125
# dynamic-1 dynamic-3 dynamic-4
