# 0.627：限定类模式的静态守卫 + Big 字面量模式的数值比较

class Local:
    def __init__(self, v: int) -> None:
        self.v = v


def scalar_subject(v: int) -> str:
    match v:
        case Local():
            return "matched"
        case _:
            return "other"


print("A", scalar_subject(5))

big = 1180591620717411303424
match big:
    case 1180591620717411303424:
        print("B big")
    case _:
        print("B other")

small = 7
match small:
    case 1180591620717411303424:
        print("C big")
    case _:
        print("C other")

# --- expect ---
# A other
# B big
# C other
