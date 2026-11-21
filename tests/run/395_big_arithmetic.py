# CPython oracle: runpy.run_path(..., init_globals={"big": int}).
def factorial(n: int) -> big:
    value: big = 1
    for i in range(2, n + 1):
        value *= i
    return value

a: big = 123456789012345678901234567890
b: big = -987654321098765432109876543210
two: big = 2
zero: big = 0
print(a, b, a + b, a - b, a * b)
print(a // b, a % b, b // a, b % a, -a, ~a)
print(a & b, a | b, a ^ b, a << 77, b >> 77)
print(two ** 100, factorial(100), a / b)
print(1 / (two << 1073), 3 / (two << 1074), 1 / (two << 1022))
print(-1 / (two << 1074), 1 / (two << 1076), (two << 1022) / 1)
try:
    print((two << 1023) / 1)
except OverflowError:
    print("caught overflow")
print(a < b, 0 < a < 5.5e30, 5.5 < a, a > 1e30)
print(a < float("inf"), float("-inf") < b, a > float("nan"), a != float("nan"))
print(9007199254740992.0 < (two ** 53 + 1) <= two ** 54)
print(a + 5, 5 + a, zero or a, a and zero)
if a and not zero and 1:
    print("truth")
for d in (0,):
    try:
        print(a // d)
    except ZeroDivisionError:
        print("caught //")
    try:
        print(a % d)
    except ZeroDivisionError:
        print("caught %")
    try:
        print(a / d)
    except ZeroDivisionError:
        print("caught /")
try:
    print(a << -1)
except ValueError:
    print("caught shift")
# --- expect ---
# 123456789012345678901234567890 -987654321098765432109876543210 -864197532086419753208641975320 1111111110111111111011111111100 -121932631137021795226185032733622923332237463801111263526900
# -1 -864197532086419753208641975320 -9 123456780012345678001234567800 -123456789012345678901234567890 -123456789012345678901234567891
# 121512828827855409466171785234 -985710360914275162674813760554 -1107223189742130572140985545788 18656262480467543164914817745080825512029480634286080 -6535749
# 1267650600228229401496703205376 93326215443944152681699238856266700490715968264381621468592963895217599993229915608941463976156518286253697920827223758251185210916864000000000000000000000000 -0.1249999988609375
# 5e-324 1e-323 1.1125369292536007e-308
# -0.0 0.0 8.98846567431158e+307
# caught overflow
# False True True False
# True True False True
# True
# 123456789012345678901234567895 123456789012345678901234567895 123456789012345678901234567890 0
# truth
# caught //
# caught %
# caught /
# caught shift
