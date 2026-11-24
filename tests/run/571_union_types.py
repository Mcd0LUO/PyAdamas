# 联合类型（0.489，Bug A 修复后的正向口径）：
#   * int 成员接受 int（也接受 bool，bool 是 int 的子类型）；
#   * int | float 仍然是 float（数值塔 join，int→float 隐式提升不变）；
#   * 本地类的联合成员按类判（子类也能进父类成员）。
def take_int_str(x: int | str) -> int:
    return 1

def take_int_float(x: int | float) -> float:
    return x

class Box:
    v: int
    def __init__(self, v: int) -> None:
        self.v = v

def take_box_int(x: Box | int) -> int:
    return 0

print(take_int_str(3), take_int_str("s"))
print(take_int_float(1) == 1.0, take_int_float(1.5))
print(take_box_int(Box(1)), take_box_int(2))

a: int | str = 7
a = "ok"
print(a)

b: int | float = 2
print(b + 0.5)
# --- expect ---
# 1 1
# True 1.5
# 0 0
# ok
# 2.5
