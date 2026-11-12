# ptr[struct]：不透明结构体指针，可接 C 返回的句柄。
cimport cstdio

f = cstdio.tmpfile()
if f is None:
    print("tmpfile failed")
else:
    print(cstdio.fputs("hello", f) >= 0)
    print(cstdio.fclose(f))

missing = cstdio.fopen("/definitely/not/here", "r")
print(missing is None)

# --- expect ---
# True
# 0
# True
