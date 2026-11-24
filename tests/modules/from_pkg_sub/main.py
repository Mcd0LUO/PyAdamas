from lib import sub
from lib import sub as s

print("call", sub.hi())
print("alias", s.hi() + 1)
print("attr", sub.VALUE)

# --- expect ---
# call 7
# alias 8
# attr 42
