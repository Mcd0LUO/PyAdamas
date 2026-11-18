from typing import Any


def make_set() -> set:
    return {1, 2}


items = make_set()
items.add(3)
print(3 in items, len(items))


def make_nested() -> dict[str, list]:
    return {"values": [1.25]}


print(make_nested()["values"][0] * 2)


def make_pair() -> tuple[int, list]:
    return (8, [2.25])


n, values = make_pair()
print(n, values[0] * 2)


def make_any() -> dict[Any, list]:
    return {"first": [3]}


a = make_any()
a["other"] = [4]
print(a["other"][0])

# --- expect ---
# True 3
# 2.5
# 8 4.5
# 4
