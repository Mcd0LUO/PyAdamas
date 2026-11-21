def erase(v: big) -> object:
    return v

def bits(v: object, n: object) -> object:
    return v & n

def shl(v: object) -> object:
    return v << 2

def shr(v: object) -> object:
    return v >> 1

def invert(v: object) -> object:
    return ~v

def pw(v: object) -> object:
    return v ** 2

a: big = 18446744073709551623
e: object = erase(a)
print(bits(e, 15), shl(e), shr(e), invert(e), pw(e))
# --- expect ---
# 7 73786976294838206492 9223372036854775811 -18446744073709551624 340282366920938463721629024463701934129
