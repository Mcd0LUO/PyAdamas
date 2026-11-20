# expect: dataclass inheritance is not supported
# pyadamas: lang-gaps
from dataclasses import dataclass, field

@dataclass
class B:
    x: int


@dataclass
class D(B):
    y: int

f = F(1)
print(f)
