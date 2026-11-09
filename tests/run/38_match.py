def classify(x: int) -> str:
    match x:
        case 0:
            return "zero"
        case 1 | 2:
            return "small"
        case -1:
            return "neg"
        case _:
            return "other"


print(classify(0), classify(1), classify(2), classify(-1), classify(5))


def seq(xs: list[int]) -> str:
    match xs:
        case []:
            return "empty"
        case [a]:
            return "one"
        case [a, b]:
            return "two"
        case [a, *rest]:
            return "many"
        case _:
            return "?"


print(seq([]), seq([1]), seq([1, 2]), seq([1, 2, 3]), seq([1, 2, 3, 4]))


def guarded(xs: list[int]) -> int:
    match xs:
        case [a, b] if a == b:
            return 1
        case [a, b]:
            return 2
        case _:
            return 3


print(guarded([2, 2]), guarded([1, 2]), guarded([1]))


def mapping(d: dict[str, int]) -> str:
    match d:
        case {"a": v}:
            return "a"
        case {"b": v}:
            return "b"
        case _:
            return "?"


print(mapping({"a": 1}), mapping({"b": 2}), mapping({"c": 3}))


def nested(xs: list[list[int]]) -> str:
    match xs:
        case [[a], [b]]:
            return "nested"
        case _:
            return "no"


print(nested([[1], [2]]), nested([[1, 2], [3]]))

# --- expect ---
# zero small small neg other
# empty one two many many
# 1 2 3
# a b ?
# nested no
