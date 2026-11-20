# expect: dataclasses.field() / call defaults are not supported
# pyadamas: lang-gaps
from dataclasses import dataclass, field

@dataclass
class F:
    x: list[int] = field(default_factory=list)

f = F(1)
print(f)
