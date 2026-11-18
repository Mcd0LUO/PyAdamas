# RC 内联快路径：堆对象、永生字面量和没初始化的全局旧值。
# 清空列表后循环变量仍要拥有最后一个元素；不同子类仍走虚调用。
class Item:
    def __init__(self, n: int) -> None:
        self.n = n

    def cost(self) -> int:
        return self.n


class DoubleItem(Item):
    def cost(self) -> int:
        return self.n * 2


items: list[Item] = [Item(3), DoubleItem(5), Item(7)]
total = 0
for item in items:
    total += item.cost()
items.clear()
print(total, item.cost())


def text_case(n: int) -> str:
    text = "literal"
    for i in range(n):
        text = "value-" + str(i)
        if i % 2 == 0:
            text = "A"
    return text


print(text_case(4), text_case(5))

# --- expect ---
# 20 7
# value-3 A
