# 0.302 unknown dict 标量-only 销毁跳过回归。
# saw_heap sticky：纯标量 unknown dict 销毁跳过链游；存过堆值必须真释放
# （ASan detect_leaks=1 验证）；覆盖 / update 升级 / copy / clear / bump /
# defaultdict / GC 环各写入路径不得漏探针。
import collections

def lcg(seed: int) -> int:
    return (seed * 1103515245 + 12345) % 2147483648

def scalar_churn(rounds: int) -> int:
    # {} 推 dict[unknown,unknown]：全标量 entry，销毁跳过链游。
    total = 0
    for r in range(rounds):
        memo = {}
        for i in range(25):
            memo[i] = i * i + r
        s = 0
        for k in memo:
            s += memo[k]
        total += s
    return total

def fib_memo(n: int, memo) -> int:
    if n < 2:
        return n
    if n in memo:
        return memo[n]
    v = fib_memo(n - 1, memo) + fib_memo(n - 2, memo)
    memo[n] = v
    return v

def memo_churn(rounds: int) -> int:
    # rec_memo 形态：fib(25)=75025 >= 65536，覆盖大标量的探针路径。
    total = 0
    for r in range(rounds):
        memo = {}
        total += fib_memo(25, memo)
    return total

def heap_churn(rounds: int) -> int:
    # 存堆值：saw_heap=1，销毁必须真释放（泄漏检查抓）。
    s = 0
    for r in range(rounds):
        d = {}
        for i in range(30):
            d[i] = "v" + str((r + i) % 100)
        for k in d:
            s += len(d[k])
        # 堆->标量覆盖：旧 str 在覆盖时 dec，sticky 保持 1。
        d[0] = 7
        d[1] = 8
        s += d[0] + d[1]
        # 标量->堆覆盖。
        d[2] = "w" + str(r % 50)
        s += len(d[2])
    return s

def nested_heap_churn(rounds: int) -> int:
    # 堆值是容器（list/dict），销毁级联释放。
    s = 0
    for r in range(rounds):
        d = {}
        for i in range(20):
            xs = []
            xs.append(i)
            xs.append("e" + str(i))
            d[i] = xs
        d[100] = {1: "a", 2: "b"}
        for k in d:
            s += len(d[k])
    return s

def update_copy_checks() -> int:
    s = 0
    # unknown <- unknown（带堆值），copy 传播。
    for r in range(200):
        a = {}
        a[1] = "x" + str(r % 30)
        b = {}
        b[2] = [r, r + 1]
        a.update(b)
        s += len(a) + len(a[1]) + len(a[2])
        c = a.copy()
        s += len(c) + len(c[2])
    # dict[str,str]（own 1,1）<- unknown：1->2 升级，既有堆 entry 不得漏。
    for r in range(200):
        t: dict[str, str] = {"k": "v" + str(r % 30)}
        u = {}
        u["k2"] = "w" + str(r % 30)
        t.update(u)
        s += len(t) + len(t["k"]) + len(t["k2"])
    # dict[int,int]（own 0,0）<- unknown 标量：0->2 升级。
    for r in range(200):
        t2: dict[int, int] = {1: r}
        u2 = {}
        u2[2] = r + 1
        t2.update(u2)
        s += t2[1] + t2[2]
    return s

def clear_pop_checks() -> int:
    s = 0
    for r in range(200):
        d = {}
        for i in range(10):
            d[i] = "s" + str(i)
        d.clear()
        s += len(d)
        d[1] = 5
        d[2] = "t" + str(r % 20)
        s += d[1] + len(d[2])
        p = d.pop(1)
        s += p
        del d[2]
        s += len(d)
        # bump 标量路径（get_or+set 融合）。
        d[7] = d.get(7, 0) + 1
        d[7] = d.get(7, 0) + 1
        s += d[7]
        # setdefault：缺失创建堆值必须探针。
        q = d.setdefault(9, "u" + str(r % 20))
        s += len(q)
    return s

def from_pairs_checks() -> int:
    s = 0
    for r in range(200):
        pairs = []
        for i in range(10):
            pairs.append((i, "p" + str((i + r) % 40)))
        d = dict(pairs)
        s += len(d) + len(d[3])
        st = set([1, 2, "a" + str(r % 20), 3])
        s += len(st)
    return s

def defaultdict_checks() -> int:
    s = 0
    for r in range(200):
        dd = collections.defaultdict(list)
        for i in range(10):
            dd[i % 3].append(i)
        s += len(dd) + len(dd[0])
        di = collections.defaultdict(int)
        for i in range(10):
            di[i % 4] += 1
        s += di[0]
        ds = collections.defaultdict(str)
        ds["k"] += "v"
        s += len(ds["k"])
    return s

def gc_cycle_dicts() -> int:
    n = 0
    # 自环 unknown dict：堆值 -> saw_heap=1，GC release 走链游。
    for r in range(20000):
        dd = {}
        dd["self"] = dd
        n += 1
    # 标量 unknown dict 经列表进环：dict_gc_release 跳过链游，环靠 GC 收。
    for r in range(20000):
        xs: list[object] = []
        d2 = {}
        d2[1] = r % 100
        xs.append(d2)
        xs.append(xs)
        n += 1
    return n

print(scalar_churn(2000))
print(memo_churn(2000))
print(heap_churn(2000))
print(nested_heap_churn(1000))
print(update_copy_checks())
print(clear_pop_checks())
print(from_pairs_checks())
print(defaultdict_checks())
print(gc_cycle_dicts())

# --- expect ---
# 59775000
# 150050000
# 209600
# 42000
# 43590
# 3400
# 3350
# 2200
# 40000
