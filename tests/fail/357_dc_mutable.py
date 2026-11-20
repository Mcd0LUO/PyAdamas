# expect: mutable default for field 'x'
# pyadamas: lang-gaps
from dataclasses import dataclass, field

@dataclass
class F:
    x: list[int] = []

f = F(1)
print(f)
