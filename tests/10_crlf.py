x = 1
y = 2
if x < y:
    z = """multi
line"""
else:
    z = "flat"

def f(a: int) -> int:
    return a + 1

print(f(x), z)
