# 562_for_rebind_types.py —— 同名循环目标被不同类型元素的 for 反复绑定
#
# 历史 bug：符号表里一个名字只有一个槽类型，第二次 for 会 join 出
# tuple[float, ...]；存储点按槽类型把 int 元组逐元素转成 float，读取点
# 却按当初记录的元素类型解释，于是打印 double 位模式
# （4607182418800017408）。修复后：循环体内按本轮元素类型检查，槽在
# 绑定类型冲突时退化成 unknown，不再做逐元素转换。

a = [(1, 2, 3), (4, 5, 6)]
for t in a:
    print(t[0], t[1], t[2])

d = [(1.5, 4), (2.5, 5)]
for t in d:
    print(t[0], t[1])

for t in [(7, 8, 9)]:
    print(t[0])
for t in [("x", 4)]:
    print(t[0], t[1])

for p, q in [(1, 2), (3, 4)]:
    print(p, q)
for p, q in [(1.5, 2), (2.5, 4)]:
    print(p, q)

for v in [[1, 2], [3, 4]]:
    print(v[0])
for v in [[1.5, 2], [2.5, 4]]:
    print(v[0])

for x in [1, 2]:
    print(x)
for x in [1.5, 2.5]:
    print(x)

for u in [1, 2]:
    pass
print(u)
for u in ["p", "q"]:
    pass
print(u)

# --- expect ---
# 1 2 3
# 4 5 6
# 1.5 4
# 2.5 5
# 7
# x 4
# 1 2
# 3 4
# 1.5 2
# 2.5 4
# 1
# 3
# 1.5
# 2.5
# 1
# 2
# 1.5
# 2.5
# 2
# q
