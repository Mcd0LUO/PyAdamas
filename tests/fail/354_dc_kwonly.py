# expect: decorator option 'kw_only' is not supported
# pyadamas: lang-gaps
from dataclasses import dataclass, field

@dataclass(kw_only=True)
class F:
    x: int

f = F(1)
print(f)
