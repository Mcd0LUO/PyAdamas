# % 整数转换的 precision（CPython：最少位数、左补 0；符号与 0 标志的交互）
print("%.3d" % 5)
print("%5.3d" % 5)
print("%-5.3d" % 5)
print("%05.3d" % 5)
print("%+.3d" % 5)
print("% .3d" % 5)
print("%.5d" % -42)
print("%08.5d" % -42)
print("%8.5d" % -42)
print("%-8.5d" % -42)
print("%08.3d" % 7)
print("%8.3d" % 7)
print("%.0d" % 0)
print("%.0d" % 42)
print("%.2d" % 0)
print("%.3x" % 255)
print("%8.4x" % 255)
print("%08.4x" % 255)
print("%.4X" % 255)
print("%.4o" % 8)
print("%+.4x" % 255)
print("%-8.4x" % 255)
print("%.5d" % 1234567)
print("%06.2d" % -3)

# 没有 precision 的路径逐字不变（0 标志 / 宽度 / 符号）
print("%05d" % 42)
print("%+5d" % 42)
print("% 5d" % 42)
print("%-5d|" % 42)
print("%x %X %o" % (255, 255, 8))

# format()/f-string 的整数说明符带 precision 是 ValueError（CPython 同）
try:
    v = 5
    t = f"{v:.3d}"
    print(t)
except ValueError:
    print("fmt err")
# --- expect ---
# 005
#   005
# 005  
# 00005
# +005
#  005
# -00042
# -0000042
#   -00042
# -00042  
# 00000007
#      007
# 0
# 42
# 00
# 0ff
#     00ff
# 000000ff
# 00FF
# 0010
# +00ff
# 00ff    
# 1234567
# -00003
# 00042
#   +42
#    42
# 42   |
# ff FF 10
# fmt err
