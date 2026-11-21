from precision import add_at_limit, offset, hold, Cell

def relay(v: object) -> big:
    k: big = v
    return k

wide: big = add_at_limit()
print(wide, offset())
print(relay(hold(wide)), relay(431))
print(Cell(wide).read() + 3)
print(18446744073709551616.0 < wide + 1 < wide + 2)
print(format((wide + 3) / (wide - 1), ".17g"))
print(f"{wide:#028x}", str(wide), repr(wide))
# --- expect ---
# 18446744073709551616 -1234567890123456789012341
# 18446744073709551616 431
# 18446744073709551619
# True
# 1
# 0x00000000010000000000000000 18446744073709551616 18446744073709551616
