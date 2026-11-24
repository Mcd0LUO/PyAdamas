# expect: argument 'o' of 'g' expects 'int | str', got 'float'
# 0.489（Bug A）：同一规则要覆盖调用点 —— 形参是联合时不能走「未知形参 →
# 回填并放行」的捷径；显式 unknown/object 标注仍然保持 unknown 域。
def g(o: int | str) -> int:
    return 0

g(1.5)