# 525_cimport_pysyntax.py —— .cpya 改成 Python 语法后的 cimport 全链路
#
# 覆盖迁移后每一条声明形态：
#   * cmodule 省略（从 cimport 名字推导）：cstdio / clib / cstructabi 都没写 cmodule
#   * link(...)：clib 链 m、cstructabi 链 structabi.c
#   * 常量：NAME: T = 字面量（编译期内联）
#   * class NAME: pass = 不透明句柄（cstdio 的 FILE），只能 ptr[FILE]
#   * class NAME: + 注解无值赋值 = 带字段的 C struct（pair/triple），按值传参/返回
#   * extern def：约定 C 符号 = Python 名（sqrt/strlen），@extern 改名（abs→abs32、
#     isalpha→is_alpha、malloc→malloc_i）
#   * extern var：注解无值（cvar_i 等）
#   * Callable[[...], R] = C 回调形参
#   * C 变参：*args（printf）

cimport clib as c
cimport cstdio
cimport cstructabi as abi

# ── 常量 + extern def（约定符号 / 改名）──────────────────────────────
print(c.PI)
print(c.ANSWER, c.SMALL, c.CODE)
print(c.NAME, c.FLAG, c.OFF)
print(c.sqrt(2.0) > 1.41)
print(c.pow(2.0, 10.0))
print(c.strlen("hello"))
print(c.abs32(-5))
print(c.is_alpha(65), c.is_alpha(95))
print(c.getenv("PYADAMAS_NO_SUCH_VAR") is None)

# ── C 变参 ──────────────────────────────────────────────────────────
print(c.printf("x=%d\n", 3))

# ── 不透明句柄（class FILE: pass）+ ptr[FILE] ────────────────────────
f = cstdio.tmpfile()
cstdio.fputs("hello", f)
cstdio.fclose(f)
print("tmpfile_ok")

# ── 带字段 struct：按值传参/返回 + 回调 ──────────────────────────────
p = pair(1.5, 2.5)
q = pair(3.0, 4.0)
r = abi.pair_add(p, q)
print(r.a, r.b)
t = triple(1, 2, 3)
u = abi.triple_sum(t)
print(u.a, u.b, u.c)
print(abi.triple_mix(10, t, 20))


def add(a: int, b: int) -> int:
    return a + b


print(abi.cb_apply(20, 22, add))

# ── extern var（无值注解 = C 名 = py 名）─────────────────────────────
print(abi.cvar_i)
abi.cvar_i = 42
print(abi.cvar_i)
print(abi.cvar_u, abi.cvar_f, abi.cvar_b)

# ── ptr[T] 返回值 + 解引用 + 写回 ────────────────────────────────────
buf = c.malloc_i(3)
c.memset(buf, 0, 24)
buf[0] = 7
print(buf[0])
c.free(buf)

# --- expect ---
# 3.141592653589793
# 42 7 8
# pyadamas True False
# True
# 1024.0
# 5
# 5
# True False
# True
# x=3
# 4
# tmpfile_ok
# 4.5 6.5
# 2 4 6
# 36
# 42
# 1234567890123
# 42
# 4294967295 2.5 True
# 7
