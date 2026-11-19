# dylib：dlopen 动态库 + 句柄取符号直调（与 from c import 共享 dlsym 后端）。
# 测试库 tests/dylib/probe.c 由本用例现场编译（构建说明见该文件头部）。
import subprocess
import sys
import dylib

so = sys.argv[0] + ".so"
r = subprocess.run(["cc", "-shared", "-fPIC", "-O2", "tests/dylib/probe.c", "-o", so],
                   capture_output=True, text=True)
print(r.returncode)

lib = dylib.load(so)
print(lib.probe_add(2, 3))
print(lib.probe_scale(4.0))
# int/str 混合签名：n + strlen(s)
print(lib.probe_mix(10, "hello"))
# char* 返回：推断默认 int，用 str 标注定 ABI
s: str = lib.probe_greet("hi")
print(s)
# 同一句柄多次取不同符号
print(lib.probe_add(40, 2))
print(dylib.close(lib))
# --- expect ---
# 0
# 5
# 10.0
# 15
# hi
# 42
# 0
