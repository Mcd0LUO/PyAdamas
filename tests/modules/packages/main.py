import app
import app.util as u
from app import util
from app.util import add
from app.helper import triple

print(app.base)
print(u.add(1, 2))
print(util.add(3, 4))
print(add(5, 6))
print(triple(7))

from app.util import *
print(add(4, 5))

# --- expect ---
# 5
# 3
# 7
# 11
# 21
# 9
