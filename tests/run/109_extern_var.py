# extern 变量：读取 C 全局 int 与 FILE*，并直接交给 C 函数使用。
cimport clib as c
cimport cstdio as s

print(c.optind)

f = s.stdout
if f is None:
    print("no stdout")
else:
    s.fputs("hello from C stdout\n", f)

# 写入 C 全局变量：写回后立即读到新值。
c.optind = 7
print(c.optind)

# --- expect ---
# 1
# hello from C stdout
# 7
