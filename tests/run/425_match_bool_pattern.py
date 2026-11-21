# True/False 是单例模式：CPython 用 is 比较，只有主体静态类型本身是 bool
# 时才可能命中。原来 Bool 被并进数值比较，match 1: case True 会命中。

def on_bool(b: bool) -> str:
    match b:
        case True:
            return "T"
        case False:
            return "F"

def true_pattern_on_int(n: int) -> str:
    match n:
        case True:
            return "true-pattern"
        case _:
            return "other"

def false_pattern_on_int(n: int) -> str:
    match n:
        case False:
            return "false-pattern"
        case _:
            return "other"

print(on_bool(True), on_bool(False))
print(true_pattern_on_int(1), true_pattern_on_int(0), true_pattern_on_int(2))
print(false_pattern_on_int(0))
# --- expect ---
# T F
# other other other
# other
