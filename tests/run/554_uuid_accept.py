# uuid.uuid4/NAMESPACE_DNS 对拍；.version 属性缺失（见 docs）。
import uuid
u = uuid.uuid4()
print("len", len(str(u)))
print("dash", str(u).count("-") == 4)
print("ns", len(str(uuid.NAMESPACE_DNS)) > 0)

# --- expect ---
# len 36
# dash True
# ns True
