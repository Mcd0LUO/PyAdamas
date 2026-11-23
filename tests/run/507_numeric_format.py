# 数字格式化/解析/转换回归（0.440）：进制、int/float 解析、f-string/format、round、% 格式化。
print("radix", hex(0), hex(255), hex(-255), oct(8), oct(-9), bin(5), bin(-5))
print("radix_big", hex(2**62), oct(2**62), bin(2**62))
print("int_parse", int("42"), int("-17"), int(" 42 "), int("+42"))
print("int_base", int("101", 2), int("377", 8), int("ff", 16), int("FF", 16))
v1 = 0
try:
    int("zz")
except ValueError:
    v1 = 1
print("bad_int", v1)
print("float_parse", float("1.5"), float("-2.5e3"), float(" 3.25 "), float("inf"))
print("float_str", str(1.0), str(0.1 + 0.2), str(1e300), str(1e16), str(1e-7))
print("float_str2", str(1.0 / 3.0), str(-0.0), str(float("nan")))
print("bool_int", str(True), int(True), int(False), True + 1)
print("conv", int(3.9), int(-3.9), float(3), int(2.0))
print("fmt_d", f"{42:d}", f"{-7:d}", f"{0:d}")
print("fmt_f", f"{1.5:f}", f"{1.5:.3f}", f"{-0.0:f}")
print("fmt_e", f"{1234.5:e}", f"{1234.5:.2e}", f"{0.0:e}")
print("fmt_g", f"{1.5:g}", f"{1234567.0:g}", f"{0.0001:g}")
print("fmt_align", f"{42:>10}", f"{42:<10}", f"{42:^10}")
print("fmt_sign", f"{42:+d}", f"{3.5:+.1f}", f"{42:010d}", f"{-42:010d}")
print("fmt_comma", f"{1234567:,d}", f"{1234567.5:,.1f}", f"{0.25:%}")
print("fmt_fill", f"{42:*>10}", f"{7:^7}")
print("format_builtin", format(42, "d"), format(1.5, ".2f"), format(255, "x"), format(-3, "05d"))
print("round_half", round(0.5), round(1.5), round(2.5), round(3.5), round(-0.5), round(-1.5))
print("round_nd", round(1.25, 1), round(1.35, 1), round(2.675, 2), round(-1.25, 1))
print("round_int", round(5), round(5, -1), round(15, -1), round(25, -1))
print("abs", abs(-5), abs(5), abs(-2.5), abs(2.5))
print("minmax", min(3, 1, 2), max(3, 1, 2), min([4, 2]), max([4, 2]))
print("divmod", divmod(7, 3), divmod(-7, 3), divmod(7, -3), divmod(-7, -3))
print("pct", "%d" % 42, "%5d" % 42, "%-5d" % 42, "%05d" % 42, "%+d" % 7)
print("pct_f", "%f" % 1.5, "%.2f" % 1.5, "%08.2f" % 3.14159, "%e" % 1234.5)
print("pct_alt", "%#x" % 255, "%#X" % 255, "%#o" % 8, "%#x" % -255)
print("pct_alt_w", "[%#10x]" % 255, "[%#-10x]" % 255, "[%#010x]" % 255)
print("pct_s", "[%10s]" % "hi", "[%-10s]" % "hi", "[%5.1s]" % "hello")
print("pct_hex", "%x" % 255, "%X" % 255, "%o" % 8, "%r" % "hi")
print("pct_multi", "%d+%d=%d" % (1, 2, 3), "%s=%d" % ("a", 5), "100%%" % ())
print("neg_fmt", f"{-1.5:8.2f}", f"{-1.5:08.2f}", f"{-255:08x}")

# --- expect ---
# radix 0x0 0xff -0xff 0o10 -0o11 0b101 -0b101
# radix_big 0x4000000000000000 0o400000000000000000000 0b100000000000000000000000000000000000000000000000000000000000000
# int_parse 42 -17 42 42
# int_base 5 255 255 255
# bad_int 1
# float_parse 1.5 -2500.0 3.25 inf
# float_str 1.0 0.30000000000000004 1e+300 1e+16 1e-07
# float_str2 0.3333333333333333 -0.0 nan
# bool_int True 1 0 2
# conv 3 -3 3.0 2
# fmt_d 42 -7 0
# fmt_f 1.500000 1.500 -0.000000
# fmt_e 1.234500e+03 1.23e+03 0.000000e+00
# fmt_g 1.5 1.23457e+06 0.0001
# fmt_align         42 42             42    
# fmt_sign +42 +3.5 0000000042 -000000042
# fmt_comma 1,234,567 1,234,567.5 25.000000%
# fmt_fill ********42    7   
# format_builtin 42 1.50 ff -0003
# round_half 0 2 2 4 0 -2
# round_nd 1.2 1.4 2.67 -1.2
# round_int 5 0 20 20
# abs 5 5 2.5 2.5
# minmax 1 3 2 4
# divmod (2, 1) (-3, 2) (-3, -2) (2, -1)
# pct 42    42 42    00042 +7
# pct_f 1.500000 1.50 00003.14 1.234500e+03
# pct_alt 0xff 0XFF 0o10 -0xff
# pct_alt_w [      0xff] [0xff      ] [0x000000ff]
# pct_s [        hi] [hi        ] [    h]
# pct_hex ff FF 10 'hi'
# pct_multi 1+2=3 a=5 100%
# neg_fmt    -1.50 -0001.50 -00000ff
