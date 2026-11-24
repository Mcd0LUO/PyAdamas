# expect: annotated as 'int | str' but assigned 'float'
# 0.489（Bug A）：联合类型以前在 parse_annotation 里塌成 unknown，于是任何值
# 都能赋进去（float 的 double 位模式一路漏到运行期）。现在保留成员表，
# 目标侧按成员逐个判：int 成员不吃 float（隐式转换只有 int→float 一个方向）。
x: int | str = 1.5