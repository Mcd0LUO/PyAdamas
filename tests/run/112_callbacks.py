# C 回调：py23 函数/闭包作为 C 函数指针，跳板保持闭包环境。
cimport cstructabi as c


def add(a: int, b: int) -> int:
    return a + b


def twice(x: float) -> float:
    return x * 2.0


def negate(x: bool) -> bool:
    return not x


print(c.cb_apply(20, 22, add))
print(c.cb_apply_f(2.5, twice))
print(c.cb_apply_i32(-5, 7, add))
print(c.cb_fold(5, add, 0))
print(c.cb_apply_b(False, negate))


# 捕获环境的闭包：C 只拿到跳板地址，闭包本身由槽传递。
def make(k: int):
    def bump(a: int, b: int) -> int:
        return a + b + k

    return bump


f = make(10)
print(c.cb_apply(1, 2, f))

# --- expect ---
# 42
# 5.0
# 2
# 10
# True
# 13
