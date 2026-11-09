from . import util as um
from .util import add

def triple(x: int) -> int:
    return add(x, um.add(x, x))
