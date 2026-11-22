# 453: 有捕获的嵌套函数（堆闭包）被间接调用时，浮点返回值必须按统一约定
# 从 %rax 取到。以前堆闭包的 +0 填的是函数本体（真实 SysV ABI，浮点在
# %xmm0），间接调用却按统一约定读 %rax —— 于是循环里的累加器只剩最后一项，
# 直接把返回值 print 出来是 5e-323（把某个整数当浮点位模式）。
# 注：无捕获的 f 走静态闭包（+0 早已是 shim），g 捕获 f 走堆闭包 ——
# 两条路径都要覆盖，缺一条这个 bug 就不出现。


def outer() -> None:
    def f(i: int, j: int) -> float:
        return 1.0 / (i + j + 1)

    def g(n: int) -> float:
        total = 0.0
        for j in range(n):
            total += f(0, j)
        return total

    # ① 循环累加逐项相加（只留最后一项 → 0.25）
    print(format(g(4), ".12f"))
    # ② 返回值不能是位模式垃圾
    print(g(1) == 1.0)
    # ③ 堆闭包返回浮点、再被当值用
    def scaled(x: float) -> float:
        return x * 2.0

    print(scaled(g(2)))


outer()
# --- expect ---
# 2.083333333333
# True
# 3.0
