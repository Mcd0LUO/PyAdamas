# secrets token_hex/choice/randbelow 对拍（随机值用范围判定）。
import secrets
print("tok", len(secrets.token_hex(8)))
print("choice", secrets.choice([1,2,3]) in [1,2,3])
print("below", secrets.randbelow(10) < 10)

# --- expect ---
# tok 16
# choice True
# below True
