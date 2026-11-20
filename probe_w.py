class A(ValueError):
    """x"""

try:
    raise A("bad")
except ValueError as e:
    print("value", e)

print("noend", end="")
print("noend2", end="")
print()
print("x" * 3)
print(str(2025).center(10), "|", "ab".center(4), "|")
