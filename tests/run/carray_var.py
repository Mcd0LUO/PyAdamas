# 0.562：array[ptr, N] 声明的 extern 数组变量取符号地址，与 C 侧一致。
cimport carrayvar as c

print(c.arr_addr() == c.arr_values)

# --- expect ---
# True
