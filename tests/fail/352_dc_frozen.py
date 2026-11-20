# expect: decorator option 'frozen' is not supported
# pyadamas: lang-gaps
from dataclasses import dataclass, field

@dataclass(frozen=True)
class F:
    x: int

f = F(1)
print(f)
