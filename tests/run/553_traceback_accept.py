# traceback.format_exc 对拍。
import traceback
try:
    raise ValueError("x")
except ValueError:
    s = traceback.format_exc()
    print("has", "ValueError" in s)
print("fmt", isinstance(traceback.format_exc(), str))

# --- expect ---
# has True
# fmt True
