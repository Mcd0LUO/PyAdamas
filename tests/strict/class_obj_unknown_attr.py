# expect: class 'K' has no attribute '__members__'
# P2：类对象接收者解析不到属性 → 编译期报错（以前静默落到运行期 AttrGet 给 0）。
# 跨模块形态是 mod.K.__members__；这里用「类名当值」在单文件里复现同一条路径
# （Alias = K 让接收者是 ClassObj，走 from_class_obj 兜底）。
class K:
    X = 1


Alias = K
print(Alias.__members__)
