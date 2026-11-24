# 全 None 列表字面量按 list[unknown] 推断（0.514）：占位容器惯用法不再误报。
# 注意：这不是放宽对真实 Optional 的检查 —— 见 tests/strict/narrow_optional_still_errors.py。
table = [None] * 4
table[1] = 7
print(table[1] + 1)

pair = [None, None]
pair[0] = 3
print(pair[0] * 2)

# --- expect ---
# 8
# 6
