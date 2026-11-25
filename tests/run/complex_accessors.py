# C3（0.534）：complex 的访问器与 conjugate()。
z: complex = complex(1.0, 2.0)
print(z.real, z.imag, z.conjugate())
print(abs(z))
w: complex = complex(3.0, -1.0)
print(z + w)
print(z - w)
print(z * w)
print((z * 2.0) / 2.0)
print(z.real + z.imag)

# --- expect ---
# 1.0 2.0 (1-2j)
# 2.23606797749979
# (4+1j)
# (-2+3j)
# (5+5j)
# (1+2j)
# 3.0
