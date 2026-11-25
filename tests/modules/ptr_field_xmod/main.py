# 0.554：ptr[float] 作类字段跨模块（celem 必须进接口）。
import ndmod


def fill(a: ndmod.Arr) -> None:
    a.data[0] = 1.5
    a.data[3] = 4.25


a = ndmod.Arr(4)
fill(a)
print(a.get(0), a.get(3))
print(a.n)
a.close()

# --- expect ---
# 1.5 4.25
# 4
