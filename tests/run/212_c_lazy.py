# 免写直调：from c import 直接绑定默认库符号（dlsym RTLD_DEFAULT），
# 不写任何 cdecl/.cpya 声明。契约（ctypes 式，调用方对 ABI 负责）：
#   float 实参 → C double；int/bool → i64；str → char*；
#   返回有 float 实参时按 double，否则按 int；标量标注可覆盖返回 ABI。
from c import sqrt, strlen, labs
from c import pow as cpow
from c import getpid

print(sqrt(2.0))
print(sqrt(9.0))
print(cpow(2.0, 10.0))
print(strlen("hello"))
print(labs(-1234567890123))

# 标注定返回 ABI
r: float = sqrt(2.0)
print(r)

# 函数返回标注同样生效
def half(x: float) -> float:
    return sqrt(x)
print(half(9.0))

# 无参 int 返回
print(getpid() > 0)

# 别名导入：符号名仍是 C 侧的 pow
print(cpow(3.0, 4.0))
# --- expect ---
# 1.4142135623730951
# 3.0
# 1024.0
# 5
# 1234567890123
# 1.4142135623730951
# 3.0
# True
# 81.0
