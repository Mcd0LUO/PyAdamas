#!/usr/bin/env python3
"""rt_big 随机化 CPython 对拍（黄金标准）。"""
import random, subprocess, sys

random.seed(20261120)
DRV = sys.argv[1] if len(sys.argv) > 1 else "build/big_fuzz"
N = int(sys.argv[2]) if len(sys.argv) > 2 else 4000

def rint(bits):
    v = random.getrandbits(bits)
    if random.random() < 0.4:
        v = -v
    # 边界值掺入
    r = random.random()
    if r < 0.05: v = 0
    elif r < 0.08: v = 1
    elif r < 0.11: v = -1
    elif r < 0.14: v = 2**63
    elif r < 0.17: v = -(2**63) - 1
    elif r < 0.20: v = 2**64 - 1
    return v

cases = []
def add(op, a, b, want):
    cases.append((op, str(a), str(b), str(want)))

for i in range(N):
    bits = random.choice([1, 8, 31, 32, 33, 63, 64, 65, 96, 128, 200, 256, 512, 1024])
    a, b = rint(bits), rint(bits)
    add("add", a, b, a + b)
    add("sub", a, b, a - b)
    add("mul", a, b, a * b)
    if b != 0:
        add("floordiv", a, b, a // b)
        add("mod", a, b, a % b)
    add("and", a, b, a & b)
    add("or", a, b, a | b)
    add("xor", a, b, a ^ b)
    add("cmp", a, b, (a > b) - (a < b))
    add("eq", a, b, int(a == b))
    add("neg", a, 0, -a)
    add("not", a, 0, ~a)
    if b != 0 and -1022 < a.bit_length() - b.bit_length() < 1024:
        q = a / b
        if q == q and abs(q) != float("inf"):
            add("truediv", a, b, "%.17g" % q)
    if i % 17 == 0:
        # Exercise the independent subnormal rounding grid, including halfway and signed zero.
        n = random.randrange(1, 16) * (-1 if i % 2 else 1)
        den = (1 << random.randrange(1020, 1090)) + random.randrange(0, 4)
        add("truediv", n, den, "%.17g" % (n / den))
    if i % 3 == 0:
        d = random.choice([0.0, -0.5, 1.5, -3.25, 2.0**63, -(2.0**63),
                           1e30, -1e30, 5e-324, 1.7976931348623157e308,
                           float("inf"), float("-inf"),
                           float(random.getrandbits(60)), -float(random.getrandbits(200))])
        add("cmpf", a, repr(d), (a > d) - (a < d))
    add("abs", a, 0, abs(a))
    add("truth", a, 0, int(bool(a)))
    k = random.randint(0, 300)
    add("shl", a, k, a << k)
    add("shr", a, k, a >> k)
    e = random.randint(0, 40)
    if abs(a) < 2**40 or e < 8:
        add("pow", a, e, a ** e)
    add("to_i64", a, 0, ((a + 2**63) % 2**64) - 2**63)
    add("hex", a, 0, hex(a))
    add("bin", a, 0, bin(a))
    add("oct", a, 0, oct(a))
    if abs(a) < 2**1023:
        add("to_f64", a, 0, "%.17g" % float(a))
    # 解析往返（十进制 / 十六进制）
    add("parse", str(a), 10, a)
    add("parse", hex(a), 0, a)

inp = "\n".join(f"{op} {a} {b}" for op, a, b, _ in cases) + "\n"
import os
env = dict(os.environ)
p = subprocess.run([DRV], input=inp, capture_output=True, text=True, timeout=300, env=env)
if p.returncode != 0:
    print("DRIVER CRASH", p.returncode, p.stderr[-2000:]); sys.exit(1)
lines = p.stdout.splitlines()
if len(lines) != len(cases):
    print("LINE COUNT", len(lines), len(cases)); sys.exit(1)
fails = 0
for (op, a, b, want), got in zip(cases, lines):
    if got != want:
        fails += 1
        if fails <= 15:
            print(f"DIFF {op} a={a[:60]} b={b[:60]}\n  got  {got[:80]}\n  want {want[:80]}")
print(f"FUZZ {'FAIL ' + str(fails) if fails else 'OK'} ({len(cases)} cases)")
sys.exit(1 if fails else 0)
