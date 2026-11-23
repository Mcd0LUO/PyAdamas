# hmac.new(..., "sha256") 对拍；函数对象 digestmod 不支持（见 stdlib/hmac.py）。
import hmac
h = hmac.new(b"key", b"msg", "sha256")
print("hexlen", len(h.hexdigest()))
h.update(b"more")
print("hexlen2", len(h.hexdigest()))

# --- expect ---
# hexlen 64
# hexlen2 64
