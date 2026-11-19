# NEW BUG N19a (silent wrong float): a loop-carried float accumulator combined
# with nested calls (list-returning minor() + float-returning det3()) INSIDE a
# function miscomputes under whole-program register pressure. det4(identity)=0.0
# (should 1.0), det4(diag(1,2,3,4))=0.0 (should 24.0). Module-level loop is correct;
# int cofactor is correct; isolated small cases are correct (Heisenbug).
# Suspect: float slot/register promotion not spilled across calls (xmm caller-saved
# clobber) when promotion pressure is high. Workaround: inline the arithmetic
# (call-free loop) -- verified correct in this same context.
import math
def det3(m: list[float]) -> float:
    a: float = m[0]; b: float = m[1]; c: float = m[2]
    d: float = m[3]; e: float = m[4]; f: float = m[5]
    g: float = m[6]; h: float = m[7]; i: float = m[8]
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
def minor(m: list[float], n: int, row: int, col: int) -> list[float]:
    out: list[float] = []
    for i in range(n):
        if i == row:
            continue
        for j in range(n):
            if j == col:
                continue
            out.append(m[i * n + j])
    return out
def det4(m: list[float]) -> float:
    s: float = 0.0
    for j in range(4):
        sign: float = 1.0 if j % 2 == 0 else -1.0
        sub: list[float] = minor(m, 4, 0, j)
        s += sign * m[j] * det3(sub)
    return s
ident: list[float] = [1.0,0.0,0.0,0.0, 0.0,1.0,0.0,0.0, 0.0,0.0,1.0,0.0, 0.0,0.0,0.0,1.0]
print("ident det4:", det4(ident))
diag: list[float] = [1.0,0.0,0.0,0.0, 0.0,2.0,0.0,0.0, 0.0,0.0,3.0,0.0, 0.0,0.0,0.0,4.0]
print("diag det4:", det4(diag))
m4: list[float] = [1.0,2.0,3.0,4.0, 5.0,6.0,7.0,8.0, 9.0,10.0,11.0,12.0, 13.0,14.0,15.0,17.0]
print("m4 det4:", det4(m4))
print("minor00:", minor(m4, 4, 0, 0))
print("det3 minor00:", det3(minor(m4, 4, 0, 0)))
print("minor01:", minor(m4, 4, 0, 1))
print("det3 minor01:", det3(minor(m4, 4, 0, 1)))
print("minor02:", minor(m4, 4, 0, 2))
print("det3 minor02:", det3(minor(m4, 4, 0, 2)))
print("minor03:", minor(m4, 4, 0, 3))
print("det3 minor03:", det3(minor(m4, 4, 0, 3)))
# integer exact det4 via same cofactor
def det3i(m: list[int]) -> int:
    return m[0]*(m[4]*m[8]-m[5]*m[7]) - m[1]*(m[3]*m[8]-m[5]*m[6]) + m[2]*(m[3]*m[7]-m[4]*m[6])
def minor_i(m: list[int], row: int, col: int) -> list[int]:
    out: list[int] = []
    for i in range(4):
        if i == row:
            continue
        for j in range(4):
            if j == col:
                continue
            out.append(m[i*4+j])
    return out
m4i: list[int] = [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,17]
s: int = 0
for j in range(4):
    sign: int = 1 if j % 2 == 0 else -1
    s += sign * m4i[j] * det3i(minor_i(m4i, 0, j))
print("int det4:", s)