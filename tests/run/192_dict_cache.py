# 字典末次查找缓存的失效回归（0.283）。
#
# 缓存命中省 hash+probe，但任何改变 entry 下标/含义的变异都必须失效：
# 插入（空闲 entry 复用）、擦除、reindex（扩容/清墓碑）。这里用确定性
# 伪随机操作序列对拍 CPython dict 语义，覆盖 contains+get / get+set /
# del+复用 / 扩容 / 墓碑清理 / 相等异指针 str 键 / ±0.0 / pop / clear。

def lcg(seed: int) -> int:
    # 32 位 LCG：乘积 < 2^61，不触发 int64 回绕（pyadamas 与 CPython
    # 的整数语义差异只在溢出时出现，测试序列必须两边一致）。
    return (seed * 1103515245 + 12345) % 2147483648

def stress() -> int:
    d = {}
    ref = {}
    seed = 12345
    ops = 0
    for i in range(20000):
        seed = lcg(seed)
        k = seed % 40
        op = (seed // 40) % 6
        if op == 0:
            v = (seed // 240) % 1000
            d[k] = v
            ref[k] = v
        elif op == 1:
            a = k in d
            b = k in ref
            if a != b:
                return -1
            if a:
                if d[k] != ref[k]:
                    return -2
        elif op == 2:
            if k in d:
                del d[k]
                del ref[k]
        elif op == 3:
            g = d.get(k, -7)
            r = ref.get(k, -7)
            if g != r:
                return -3
        elif op == 4:
            if k in d:
                p = d.pop(k)
                if p != ref.pop(k):
                    return -4
        else:
            d[k] = d.get(k, 0) + 1
            ref[k] = ref.get(k, 0) + 1
        ops += 1
    s = 0
    for k in ref:
        if d[k] != ref[k]:
            return -5
        s += d[k]
    if len(d) != len(ref):
        return -6
    return s + ops

def edge() -> int:
    s = 0
    # contains+get 连续命中
    d = {1: 10, 2: 20}
    if 1 in d:
        s += d[1]
    # get+set 更新路径
    d[1] = d.get(1, 0) + 5
    s += d[1]
    # 擦除缓存键后必须缺失
    del d[1]
    s += d.get(1, -1)
    if 1 in d:
        s += 1000
    # 空闲 entry 复用：删 2 插 3，旧键 1 不得复活
    del d[2]
    d[3] = 30
    s += d.get(1, -2) + d[3]
    # 扩容 reindex 后旧键仍可查
    for i in range(100):
        d[i + 100] = i
    s += d[100] + d[199]
    # 墓碑清理：反复插删同一批键
    for r in range(50):
        for i in range(20):
            d[i + 1000] = r
        for i in range(20):
            del d[i + 1000]
        s += d.get(1000, -3)
    # 相等异指针 str 键
    sd = {}
    sd["ab"] = 7
    k = "a" + "b"
    s += sd[k]
    if k in sd:
        s += 1
    # ±0.0 同槽
    fd = {0.0: 9}
    s += fd[-0.0]
    # popitem / clear / update
    d2 = {5: 50, 6: 60}
    kk, vv = d2.popitem()
    s += kk + vv
    d2.clear()
    s += len(d2)
    d2.update({7: 70, 8: 80})
    s += d2[7] + d2[8]
    return s

print(stress())
print(edge())

# --- expect ---
# 31817
# 234
