import math
from math import sqrt, pi, floor
import sys

print(math.sqrt(9.0))
print(sqrt(16.0))
print(math.floor(3.7), math.ceil(3.1))
print(floor(4.9))
print(math.pow(2.0, 3.0))
print(math.fmod(7.0, 4.0))
print(pi > 3.14, math.pi > 3.14)
print(math.log(math.e) > 0.99)
print(sys.maxsize)
print(len(sys.argv))
from sys import argv
print(len(argv))

# --- expect ---
# 3.0
# 4.0
# 3 4
# 4
# 8.0
# 3.0
# True True
# True
# 9223372036854775807
# 1
# 1
