import lib
from lib import greet as g

f = lib.greet
print(f("a"))
print(g("b", "?"))
from lib import value
print(value)
lib.value = "new"
print(value, lib.value)
print(lib.__name__)

# --- expect ---
# hi a!
# hi b?
# old
# old new
# lib
