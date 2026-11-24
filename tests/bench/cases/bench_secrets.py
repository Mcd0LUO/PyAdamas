"""secrets 基准：token_hex / randbelow / compare_digest。"""
import secrets

total = 0
r = 0
while r < 400:
    total += len(secrets.token_hex(16))
    total += len(secrets.token_urlsafe(12))
    total += secrets.randbelow(1000)
    if secrets.compare_digest("abc", "abc"):
        total += 1
    r += 1
print(total)
