# 0.301 列表缓冲池扩容回归：跨档增长 / cap 抬高 / 位图复用 / 排序临时区 /
# 超大表池外 fallback / 清空再增长 / GC 环列表。池块跨轮复用，旧内容不得
# 泄漏进 data / 位图语义；GC 清扫直接 free 不经池。

def lcg(seed: int) -> int:
    # 32 位 LCG：乘积 < 2^61，两边整数序列一致（同 192 的教训）。
    return (seed * 1103515245 + 12345) % 2147483648

def churn_tuples(rounds: int) -> int:
    # kind-1 元组表：500 元素缓冲 4000B 超旧 512B 池档。第二轮起首次增长
    # 应弹出上一轮销毁归还的大块，cap 一次到位；内容逐轮验证。
    s = 0
    for r in range(rounds):
        xs = []
        x = r + 1
        for i in range(500):
            x = lcg(x)
            a = x % 1000
            x = lcg(x)
            b = x % 1000
            xs.append((a, b, a + b))
        xs.sort()
        if len(xs) != 500:
            return -1
        # 0.312：元组排序已按元素字典序（修复前落机器字/指针序≈no-op，
        # sort_struct 同款）——校验和与顺序无关，修复前后输出不变。
        for t in xs:
            s += t[0] + t[1] + t[2]
    return s

def churn_mixed(rounds: int) -> int:
    # kind-2 混合表（list[object] -> 位图）：位图块销毁入池、下一轮复用，
    # 旧位必须按语义清零/重写。与 churn_tuples 交替，池块交叉充当 data/位图。
    s = 0
    for r in range(rounds):
        xs: list[object] = []
        for i in range(200):
            if i % 3 == 0:
                xs.append(str(i % 50))
            else:
                xs.append(i)
        if len(xs) != 200:
            return -1
        for v in xs:
            s += len(str(v))
        xs.clear()
        # 清空再增长：位图复用旧块，append 逐位重写 [0,len)。
        for i in range(150):
            if i % 2 == 0:
                xs.append(i * 2)
            else:
                xs.append("v" + str(i % 20))
        if len(xs) != 150:
            return -2
        for v in xs:
            s += len(str(v))
    return s

def sort_checks() -> int:
    # 排序临时区入池：n>64 触发 timsort tmp；边界 0/1/2/64/65 都过一遍。
    s = 0
    for n in [0, 1, 2, 64, 65, 500]:
        xs = []
        x = 7
        for i in range(n):
            x = lcg(x)
            xs.append(x % 100000)
        xs.sort()
        ok = 1
        for i in range(1, n):
            if xs[i - 1] > xs[i]:
                ok = 0
        s += ok * 1000 + n
        if n > 0:
            s += xs[0] + xs[n - 1]
    fs = []
    x = 3
    for i in range(300):
        x = lcg(x)
        fs.append((x % 10000) / 7.0)
    fs.sort()
    for i in range(1, 300):
        if fs[i - 1] > fs[i]:
            s -= 1
    s += int(fs[0] * 100) + int(fs[299] * 100)
    ss = []
    for i in range(300):
        ss.append("k" + str((i * 37) % 300))
    ss.sort()
    for i in range(1, 300):
        if ss[i - 1] > ss[i]:
            s -= 1
    s += len(ss[0]) + len(ss[299])
    return s

def mod7(v: int) -> int:
    return v % 7

def sorted_key_checks() -> int:
    # sorted(key=)：key 在生成循环预算好，排序本体不调用户代码。
    # 稳定性：相等 key 保持原相对顺序。
    xs = []
    for i in range(200):
        xs.append(i)
    ys = sorted(xs, key=mod7)
    s = 0
    for i in range(1, 200):
        if mod7(ys[i - 1]) > mod7(ys[i]):
            return -1
        if mod7(ys[i - 1]) == mod7(ys[i]) and ys[i - 1] >= ys[i]:
            return -2
    s += ys[0] + ys[199]
    ws = []
    for i in range(50):
        ws.append(str(i * i))
    zs = sorted(ws, key=len)
    for i in range(1, 50):
        if len(zs[i - 1]) > len(zs[i]):
            return -3
    s += len(zs) + len(zs[49])
    return s

def big_list() -> int:
    # 3000 元素：cap 链到 4096 元素（32KB）超 8192B 池上限，走池外
    # malloc/realloc fallback；排序 tmp 1501 元素同样池外。
    xs = []
    for i in range(3000):
        xs.append(i * 3 - 1)
    if len(xs) != 3000:
        return -1
    for i in range(3000):
        if xs[i] != i * 3 - 1:
            return -2
    xs.sort()
    for i in range(1, 3000):
        if xs[i - 1] > xs[i]:
            return -3
    s = xs[0] + xs[2999]
    xs.clear()
    for i in range(1200):
        xs.append(i)
    s += len(xs) + xs[1199]
    return s

def extend_checks() -> int:
    # extend / 切片 / 切片赋值的增长路径（copy_range 与 slice_assign）。
    s = 0
    for r in range(200):
        a = []
        for i in range(100):
            a.append(i)
        b = []
        for i in range(400):
            b.append(i * 2)
        a.extend(b)
        if len(a) != 500:
            return -1
        s += a[499]
        c = a[10:400]
        if len(c) != 390:
            return -2
        s += c[0] + c[389]
        # delta=+25 -> 525 > cap 512，增长到 1024 元素（8192B 边界档）。
        a[5:10] = b[0:30]
        if len(a) != 525:
            return -3
        s += a[5] + a[524]
    return s

def gc_cycles() -> int:
    # 自环混合表：RC 解不开，靠 GC 清扫。清扫路径直接 free data/位图，
    # 不经池 —— 池块不得被双重释放（ASan 抓）。
    n = 0
    for r in range(30000):
        a: list[object] = []
        a.append(r % 100)
        a.append("s" + str(r % 50))
        a.append(a)
        n += 1
    return n

print(churn_tuples(40))
print(churn_mixed(40))
print(sort_checks())
print(sorted_key_checks())
print(big_list())
print(extend_checks())
print(gc_cycles())

# --- expect ---
# 39874176
# 33160
# 716342
# 249
# 11394
# 440800
# 30000
