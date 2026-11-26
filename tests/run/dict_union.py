# 0.565：dict 的 | 运算符（PEP 584）。
a: dict[str, int] = {"x": 1, "y": 2}
b: dict[str, int] = {"y": 9, "z": 3}
c: dict[str, int] = a | b
print(a)
print(c)

# --- expect ---
# {'x': 1, 'y': 2}
# {'x': 1, 'y': 9, 'z': 3}
