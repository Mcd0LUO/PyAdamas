# 0.608：print(..., file=f) 的 repr 语义 —— 容器里的字符串要带引号（与 stdout 一致）
f = open('/tmp/pa_print_repr.txt', 'w')
print([1, "a", 2.5], file=f)
print(("s", 3), file=f)
print("plain", file=f)
print({"k": "v"}, file=f)
f.close()
g = open('/tmp/pa_print_repr.txt')
print(g.read(), end='')
g.close()

# --- expect ---
# [1, 'a', 2.5]
# ('s', 3)
# plain
# {'k': 'v'}
