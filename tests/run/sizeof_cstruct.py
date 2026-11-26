# 0.561：.cpya 里声明的 C struct 也能 sizeof（布局来自头文件/声明）。
cimport szstruct as c

print(sizeof(c.pair))
print(sizeof(c.triple))

# --- expect ---
# 16
# 24
