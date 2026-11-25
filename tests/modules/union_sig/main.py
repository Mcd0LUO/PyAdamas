# 跨模块 union 形参与类对象（0.527 P0-B）：接口编码必须保留联合成员表与 ClassObj。
# 修前：`a.add(2.0)` 会退化成 unknown 回填，第二次 `a.add(Value(...))` 直接报
# 「used with both float and non-float values」；类对象别名 `P = Value` 也拿不到构造器。
from value import Value

a: Value = Value(3.0)
print(a.add(2.0).get(), a.add(Value(1.0)).get())

P = Value
b: Value = P(7.0)
print(b.get())

c: Value = Value(9.0)
print(c.get())

# --- expect ---
5.0 4.0
7.0
9.0
