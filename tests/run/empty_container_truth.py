# 空容器的真值判断（3.12 的 -O0 崩溃候选）：list/dict/set/str/tuple 全族。
# 这条同时进 tests/opt_levels.sh（-O0 / -O1 / -O2 都跑），钉住「空容器的 data
# 指针即使为 NULL 也不能被当数组解引用」。


def truthy_list(xs: list[int]) -> str:
    return "nonempty" if xs else "empty"


def truthy_dict(d: dict[str, int]) -> str:
    return "nonempty" if d else "empty"


def truthy_set(s: set[int]) -> str:
    return "nonempty" if s else "empty"


def truthy_str(s: str) -> str:
    return "nonempty" if s else "empty"


def truthy_tuple(t: tuple[int, ...]) -> str:
    return "nonempty" if t else "empty"


xs: list[int] = []
if xs:
    print("nonempty")
else:
    print("empty")
print(truthy_list([]), truthy_list([1]))
print(truthy_dict({}), truthy_dict({"a": 1}))
print(truthy_set(set()), truthy_set({1}))
print(truthy_str(""), truthy_str("x"))
print(truthy_tuple(()), truthy_tuple((1,)))
while xs:
    print("never")
print(not xs, bool(xs), len(xs))

# --- expect ---
# empty
# empty nonempty
# empty nonempty
# empty nonempty
# empty nonempty
# empty nonempty
# True False 0
