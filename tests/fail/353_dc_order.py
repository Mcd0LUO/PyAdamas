# expect: decorator option 'order' is not supported
# pyadamas: lang-gaps
from dataclasses import dataclass, field

@dataclass(order=True)
class F:
    x: int

f = F(1)
print(f)
