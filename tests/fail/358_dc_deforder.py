# expect: non-default argument 'y' follows default argument
# pyadamas: lang-gaps
from dataclasses import dataclass, field

@dataclass
class F:
    x: int = 1
    y: int

f = F(1)
print(f)
