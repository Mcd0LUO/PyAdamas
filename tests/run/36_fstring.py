x = 3.14159
print(f"{x:.2f}")
print(f"{x:>10.3f}")
print(f"{x:<10.2f}|")
print(f"{x:^10.2f}|")
print(f"{42:05d}")
print(f"{42:+d}")
print(f"{255:x} {255:X} {5:b} {8:o}")
print(f"{1234567:,}")
print(f"{0.256:.1%}")
print(f"{'hi':>5}|")
print(f"{'hi':.1}")
print(f"{x!r}")
print(f"{'abc'!r}")
print(f"{x:{5}.2f}")
name = "py"
print(f"{name!s:>4}")
print(f"{{literal}} {x:.1f}")
print(f"{x=}")

# --- expect ---
# 3.14
#      3.142
# 3.14      |
#    3.14   |
# 00042
# +42
# ff FF 101 10
# 1,234,567
# 25.6%
#    hi|
# h
# 3.14159
# 'abc'
#  3.14
#   py
# {literal} 3.1
# x=3.14159
