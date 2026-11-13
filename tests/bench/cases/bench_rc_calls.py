"""RC 基准：字符串参数/返回值跨函数传递。"""
def tag(s: str) -> str:
    return s + "!"

def wrap(s: str) -> str:
    t = tag(s)
    return t + "?"

total = 0
i = 0
while i < 20000:
    total += len(wrap("abc"))
    i += 1
print(total)
