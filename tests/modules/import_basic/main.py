import state
import helper
from state import value
from pkg.mod import double
from pkg import value as pkg_value

print("init", state.counter, value)
state.bump()
print("after_bump", state.counter)
print("helper", helper.touch())
state.value = 100
print("snapshot", value, state.value)
print("pkg", double(4), pkg_value)
print("name", __name__)

# --- expect ---
# init 0 1
# after_bump 1
# helper 2
# snapshot 1 100
# pkg 8 21
# name __main__
