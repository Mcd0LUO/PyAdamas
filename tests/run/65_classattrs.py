class Config:
    name = "base"
    timeout = 30
    tags = []

    def __init__(self, name):
        self.name = name

    def show(self):
        print(self.name, self.timeout)

    @classmethod
    def make(cls, name):
        return cls(name)

    @classmethod
    def describe(cls):
        return cls.name + ":" + str(cls.timeout)


class FastConfig(Config):
    name = "fast"
    timeout = 5


c = Config("c")
f = FastConfig("f")
c.show()
f.show()
print(Config.name, FastConfig.name)
print(Config.describe())
print(FastConfig.describe())
print(Config("z").describe())
print(FastConfig("z").describe())


# 类属性是共享存储：通过类方法修改会影响所有实例
class Counter:
    total = 0

    @classmethod
    def bump(cls):
        cls.total += 1


Counter.bump()
Counter.bump()
print(Counter.total)
print(Counter().total)


# 子类赋值不影响基类
class A:
    kind = "a"


class B(A):
    pass


print(B.kind)
B.kind = "b"
print(A.kind, B.kind)
print(B().kind, A().kind)


# 类体里的类属性对后续语句可见
class Chain:
    x = 1
    y = x + 10
    z = y * 2


print(Chain.x, Chain.y, Chain.z)

# 可变类属性在父子之间是同一个对象
print(Config.tags is FastConfig.tags)
Config.tags.append(1)
print(FastConfig.tags)

# 实例字段遮蔽类属性时，构造时就带上类属性的初值
class Shadow:
    level = 7

    def raise_level(self):
        self.level = self.level + 1


s = Shadow()
print(s.level, Shadow.level)
s.raise_level()
print(s.level, Shadow.level)
print(Shadow().level)

# --- expect ---
# c 30
# f 5
# base fast
# base:30
# fast:5
# base:30
# fast:5
# 2
# 2
# a
# a b
# b a
# 1 11 22
# True
# [1]
# 7 7
# 8 7
# 7
