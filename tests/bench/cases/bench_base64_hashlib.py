"""base64 / hashlib 基准：编码往返 + sha256 摘要。"""
import base64
import hashlib

data = b"the quick brown fox jumps over the lazy dog"

total = 0
for _ in range(20000):
    enc = base64.b64encode(data)
    dec = base64.b64decode(enc)
    total += len(dec)
    total += len(hashlib.sha256(data).hexdigest())
print(total)
