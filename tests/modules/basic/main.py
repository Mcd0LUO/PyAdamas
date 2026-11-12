import util
from util import add, scale as sc

print(util.add(1, 2))
print(add(3, 4))
print(util.base)
util.base = 20
print(util.base)
print(sc(1.5))
print(util.wrap(util.tag))
print(__name__)

# --- expect ---
# 3
# 7
# 10
# 20
# 3.0
# b'pre!'
# __main__
