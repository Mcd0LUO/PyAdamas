# pyadamas: lang-gaps
# contextlib 常用 API 对拍（CPython 3.14 同形脚本，逐字节 0 diff）
# suppress 需要 PYADAMAS_LANG_GAPS=1（异常类当值），由文件头指令开启。
import contextlib

# suppress
with contextlib.suppress(ValueError):
    raise ValueError("expected")
print("suppress", 1)

# nullcontext
with contextlib.nullcontext(42) as v:
    print("null", v)

# closing (file)
p = "/tmp/sb6_closing.txt"
f = open(p, "w")
with contextlib.closing(f):
    f.write("x")
print("closed", f.closed)

# contextmanager
@contextlib.contextmanager
def cm() -> object:
    print("enter")
    yield 7
    print("exit")

with cm() as v:
    print("cm", v)

# --- expect ---
# suppress 1
# null 42
# closed True
# enter
# cm 7
# exit
