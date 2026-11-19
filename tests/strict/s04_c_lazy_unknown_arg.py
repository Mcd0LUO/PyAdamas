# expect: cannot infer C ABI
# 免写直调的实参类型必须能推断 C 编组；多态回填成 unknown 的实参
# 在 build 阶段拒绝（而不是静默按 int 传）。
from c import some_c_fn
def take(x):
    some_c_fn(x)
take(1)
take("s")
