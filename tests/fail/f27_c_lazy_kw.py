# expect: does not take keyword arguments
# 免写直调没有声明可查关键字名：关键字实参在 check 阶段拒绝。
from c import sqrt
print(sqrt(x=2.0))
