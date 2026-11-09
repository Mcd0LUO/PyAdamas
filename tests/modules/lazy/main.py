def call() -> int:
    import helper
    return helper.add(1, 2)

print(call())
print(call())

def call2() -> int:
    from helper import value
    return value

print(call2())

import helper as h
print(h.add(3, 4))

from helper import value
import helper
print(value, helper.value)
helper.value = 5
print(value, helper.value)

# --- expect ---
# load helper
# 3
# 3
# 1
# 7
# 1 1
# 1 5
