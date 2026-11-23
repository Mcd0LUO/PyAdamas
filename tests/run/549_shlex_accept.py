# shlex split/quote/join 对拍。
import shlex
print("s1", shlex.split("a b c"))
print("s2", shlex.split("  a   b"))
print("q", shlex.quote("a b"))
print("j", shlex.join(["a","b c"]))

# --- expect ---
# s1 ['a', 'b', 'c']
# s2 ['a', 'b']
# q 'a b'
# j a 'b c'
