# expect: is not subscriptable
# 类没有 __getitem__：以前直接落到 IndexLoad，把对象当 list 解引用 -> 段错误。
class NoGet:
    pass

n = NoGet()
print(n[0])
