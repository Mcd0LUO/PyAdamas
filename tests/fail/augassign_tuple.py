# expect: does not support in-place operator
# tuple 的原地运算没有运行时支持：必须报错（以前退化成 bin op 会打出野指针）
t = (1, 2)
t += (3,)