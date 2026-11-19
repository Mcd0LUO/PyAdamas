# 266_p3_gen_tag —— 0.328 P3-1b 步骤3：生成器 resume vreg tag + send 装箱
#
# 生成器/协程 resume 函数的 unknown vreg 启用平行 tag（命名槽在堆帧里，
# 无平行 tag 来源，装载仍 UNK 回退）。一期整函数禁 tag 的两个根因已修：
#   1) tag 区清零挪到 g.entry 块头——每次 resume 都经过，垃圾 tag 不跨
#      resume 残留（一期 118/124/128/133 SIGSEGV 根因）；
#   2) GenSent 写 UNK（send 边界无 tag 来源）。
# 配套：send/throw 值进 unknown 域装箱（float 裸位模式原先被 gen.sent 当
# 整数，0.327 基线已存在）；同步 resume 内消费，调用方临时份足够。
# 覆盖：门控生成器（unknown+unknown 算术不被降级器改写）for-gen/list(gen)/
# next/send 消费、str 累加生成器、send int/float 装箱、close。
# 全部先对拍 CPython 再钉 expect；O0/O1/O2 输出一致；ASan detect_leaks=1 干净。
def gen(xs: list[object]):
    s: object = 0
    for x in xs:
        s = s + x
        yield s
xs: list[object] = [1.5, 2, 3.5]
for v in gen(xs):
    print(v)
def gen2(n: int):
    s: object = 0
    i: int = 0
    while i < n:
        s = s + i
        i = i + 1
        yield s
print(list(gen2(4)))
g = gen2(3)
print(next(g))
print(g.send(10))
print(next(g))

def gen3(ys: list[object]):
    s: object = ""
    for y in ys:
        s = s + y
        yield s
for w in gen3(["a", "b", "c"]):
    print(w)
def acc():
    s: object = 0
    while True:
        x: object = yield s
        s = s + x
g = acc()
print(next(g))
print(g.send(5))
print(g.send(2.5))
print(g.send(2))
g.close()
# --- expect ---
# 1.5
# 3.5
# 7.0
# [0, 1, 3, 6]
# 0
# 1
# 3
# a
# ab
# abc
# 0
# 5
# 7.5
# 9.5
