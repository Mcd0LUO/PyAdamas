# 335_fixed_int_control —— P1 定宽整数在控制流/循环/下标里的符号语义
#
# 比较分支（codegen 走 jcc 而不是 setcc）与循环条件必须按提升后的类型
# 选 signed/unsigned：u64 用 jb/ja，其余宽度符号指令等价（值非负不变量）。
# 这里覆盖 if/while/for、负值分支、无符号边界比较、定宽循环计数（回绕退出）。

i: i8 = -5
if i < 0:
    print("neg")
else:
    print("nonneg")
u: u8 = 200
if u > 100:
    print("big")
else:
    print("small")
n: u64 = cast_u64(-1)
if n > cast_u64(1):
    print("u64-big")
if n < cast_u64(1):
    print("u64-small")
if cast_u32(4294967295) > cast_u32(1):
    print("u32-big")
if cast_i32(-1) < cast_i32(1):
    print("i32-neg")

total: i32 = 0
k: i8 = 0
while k < 5:
    total += k
    k += 1
print(total, k)

count: u8 = 0
while count < cast_u8(200):
    count += cast_u8(50)
print(count)

dec: i16 = 0
while True:
    dec -= 1
    if dec < cast_i16(-3):
        break
print(dec)

neg_u8: u8 = 1
print(-neg_u8)
print(~neg_u8)

sq: i8 = 0
for v in range(1, 100):
    sq = cast_i8(sq + cast_i8(v * v % 100))
    if sq > cast_i8(50):
        break
print(sq)

lst: list[int] = [1, 2, 3, 4]
idx: u8 = 3
print(lst[idx])
print(lst[idx] + lst[cast_i8(0)])

print(min(cast_u64(-1), cast_u64(0)))
print(max(cast_i64(-9223372036854775807 - 1), cast_i64(9223372036854775807)))
# --- expect ---
# neg
# big
# u64-big
# u32-big
# i32-neg
# 10 5
# 200
# -4
# 255
# 254
# 55
# 4
# 5
# 0
# 9223372036854775807
