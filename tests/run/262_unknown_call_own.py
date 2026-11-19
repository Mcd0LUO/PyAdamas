# 262_unknown_call_own —— 0.328 P3-1b：unknown 调用结果所有权（基线 leak 根治）
#
# 0.325 基线已存在的所有权洞：用户调用返回 object（unknown）时，被调按
# emit_return 契约给调用方保留了一份引用，但降级器 def_heap 只给静态堆类型
# 的结果登记临时栈 —— unknown 结果直接消费（print/算术/比较）时那份引用
# 无人释放（j12 str 104B、m5 float 盒 72B、print(object 调用) 144B/2obj）。
# 修复：def_heap 对 Call/CallDirect/VCall* 的 unknown 结果补 push_temp_unknown
# （运行时探针确认堆性才入栈，整数安全）；inc/temp_unknown 探针与 dec_unknown
# 统一为 pa_unknown_is_heap（exact-RC 模式 leaf 对称，stdlib-batch1 §四半）。
# 覆盖：直打（inline/递归非 inline）、算术/比较/一元负消费、局部槽中转、
# return f() 上抛、容器 append、同句双调用、identity 透传 int/str/float、
# 循环逐轮 cleanup。ASan detect_leaks=1 钉住无泄漏。
# 全部先对拍 CPython 再钉 expect；O0/O1/O2 输出一致。
def fbox(x: float) -> object:
    if x > 100.0:
        return fbox(x - 1.0)
    return x * 2.0
def fstr(s: str) -> object:
    return s.lower()
def fid(x: object) -> object:
    return x
print(fbox(1.5))
print(fstr("ABC"))
print(fbox(2.5) + 1)
print(fbox(1.0) * 2)
print(-fbox(0.5))
print(fbox(3.0) == 6.0)
def g():
    v: object = fbox(1.5)
    print(v)
    w: object = fstr("XY")
    print(w + "!")
g()
def outer() -> object:
    return fbox(4.0)
print(outer())
xs: list[object] = []
xs.append(fbox(0.25))
xs.append(fstr("Q"))
print(xs)
print(fbox(1.0) + fbox(2.0))
print(fid(7))
print(fid("s"))
print(fid(2.5))
i: int = 0
total: object = 0
while i < 5:
    total = fbox(i + 0.5)
    i = i + 1
print(total)
# --- expect ---
# 3.0
# abc
# 6.0
# 4.0
# -1.0
# True
# 3.0
# xy!
# 8.0
# [0.5, 'q']
# 6.0
# 7
# s
# 2.5
# 9.0
