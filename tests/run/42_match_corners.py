def cap(xs: list[int]) -> int:
    match xs:
        case [a, b]:
            pass
        case [a]:
            pass
        case _:
            a = 0
    return a


print(cap([1, 2]), cap([3]), cap([]))

match [7]:
    case [m]:
        pass
print(m)


def unknown(v) -> str:
    match v:
        case "x":
            return "sx"
        case 3j:
            return "cj"
        case _:
            return "?"


print(unknown("x"), unknown(3j), unknown(1))


def kinds(v) -> str:
    match v:
        case str():
            return "str"
        case complex():
            return "complex"
        case object():
            return "obj"


print(kinds("a"), kinds(2j), kinds(5))

# --- expect ---
# 1 3 0
# 7
# sx cj ?
# str complex obj
