# 0.639：自文档化 f-string 的格式说明符 / f-string 表达式里的注释
x = 3.14
print(f"{x=:.1f}")
print(f"{x=}")

y = 1
print(f"""{
    y  # 注释里有右花括号和冒号
}""")

print(f"{y:#b}")

# --- expect ---
# x=3.1
# x=3.14
# 1
# 0b1
