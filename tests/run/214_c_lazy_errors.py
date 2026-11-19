# 免写直调/dylib 的错误路径：符号不存在、库不存在、句柄符号不存在，
# 都抛 OSError 且消息点名符号/路径，绝不静默跳空指针。
import subprocess
import sys
import dylib
from c import no_such_symbol_214_xyz

try:
    no_such_symbol_214_xyz(1)
except OSError as e:
    print(e)

try:
    lib = dylib.load("./__no_such_library_214.so")
except OSError as e:
    # dlerror 文本随平台变化，只校验我们自己的前缀与路径都在消息里
    m = str(e)
    print("dylib.load: cannot open" in m, "__no_such_library_214.so" in m)

so = sys.argv[0] + ".so"
subprocess.run(["cc", "-shared", "-fPIC", "-O2", "tests/dylib/probe.c", "-o", so],
               capture_output=True, text=True)
lib = dylib.load(so)
try:
    lib.no_such_fn_214(1)
except OSError as e:
    print(e)
print(dylib.close(lib))
# --- expect ---
# undefined C symbol 'no_such_symbol_214_xyz' (dlsym RTLD_DEFAULT)
# True True
# undefined C symbol 'no_such_fn_214' in dylib handle
# 0
