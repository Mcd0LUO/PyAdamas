# Intentional pa semantics; this test is not a CPython oracle.
x: big = 18446744073709551619
small: big = cast_big(-3.9)
wide: u64 = 18446744073709551615
as_big: big = wide
text: big = cast_big("  -12_3456789012345678901234  ")
print(cast_i64(x), cast_i64(-x), cast_i8(x), as_big)
print(small, x + 2.7, -2.9 + x, text)
f: object = 5.9
converted: big = f
print(converted)
try:
    print(x ** -1)
except ValueError:
    print("caught power")
try:
    bad: big = cast_big("1234567890_ ")
    print(bad)
except ValueError:
    print("caught parse")
try:
    bad: big = cast_big("1\x00x")
    print(bad)
except ValueError:
    print("caught nul")
# --- expect ---
# 3 -3 3 18446744073709551615
# -3 18446744073709551621 18446744073709551617 -123456789012345678901234
# 5
# caught power
# caught parse
# caught nul
