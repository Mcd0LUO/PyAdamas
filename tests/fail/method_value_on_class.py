# expect: is an instance method; take the value on an instance
# A8-5：在类对象上取实例方法的值 → 一条诊断（本地类名 / 类对象别名 / 跨模块 mod.K.m
# 都走同一处）。以前本地要到降级层才报这句，别名给的是 method value ... requires a
# class instance，跨模块则是走到调用点才报 function '<imported>' takes 0 positional
# arguments but 1 were given —— 同一件事三种文案。
class C:
    def get(self) -> int:
        return 1


c = C()
g = C.get
print(g(c))
