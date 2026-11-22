# 迭代中改容器：dict、zip、推导式三条守卫
# 0.382 合并自 3 个文件：433_dict_iter_mutation.py, 437_zip_iter_mutation.py, 445_comp_iter_guard.py
# 每段包在独立函数里，避免同名变量/类跨段冲突。

# ── 原 433_dict_iter_mutation.py ──
def _case_1() -> None:
    # 433: 迭代中修改 dict/set 必须抛可捕获的 RuntimeError（CPython 语义）。
    # 旧行为有两种错法：删空之后被下标上界悄悄结束（静默），
    # 或者走到越界读时 panic（进程直接退出、except 接不住）。


    def dict_del() -> str:
        d: dict[int, int] = {1: 1}
        try:
            for k in d:
                del d[k]
        except RuntimeError as e:
            return "RuntimeError: " + str(e)
        return "no error"


    def set_add() -> str:
        s: set[int] = set()
        s.add(1)
        try:
            for x in s:
                s.add(2)
        except RuntimeError as e:
            return str(e)
        return "no error"


    def dict_add() -> str:
        d: dict[int, int] = {1: 1, 2: 2}
        try:
            for k in d:
                d[99] = 9
        except RuntimeError as e:
            return "RuntimeError: " + str(e)
        return "no error"


    def enum_mut() -> str:
        d: dict[str, int] = {"a": 1}
        try:
            for i, k in enumerate(d):
                d["b"] = 2
        except RuntimeError as e:
            return "RuntimeError: " + str(e)
        return "no error"


    def plain() -> str:
        d: dict[int, int] = {1: 10, 2: 20}
        out: str = ""
        for k in d:
            out = out + str(k) + "=" + str(d[k]) + " "
        s: set[int] = set()
        s.add(7)
        s.add(8)
        n: int = 0
        for v in s:
            n = n + v
        return out + "/" + str(n)


    print(dict_del())
    print(set_add())
    print(dict_add())
    print(enum_mut())
    print(plain())

# ── 原 437_zip_iter_mutation.py ──
def _case_2() -> None:
    # 437: zip 迭代中改 dict/set 也要抛可捕获的 RuntimeError（与 for k in d 同口径）。
    # zip 的长度是入口算的 min(len a, len b)，删短其中一个会让两者相对关系变化，
    # 所以两个操作数各存一份入口长度逐轮比对。


    def del_first() -> str:
        d: dict[str, int] = {"a": 1, "b": 2}
        e: dict[str, int] = {"x": 1, "y": 2}
        try:
            for k, v in zip(d, e):
                del d[k]
        except RuntimeError as err:
            return "RuntimeError: " + str(err)
        return "no error"


    def del_second() -> str:
        a: dict[str, int] = {"x": 1, "y": 2}
        b: dict[str, int] = {"x": 3, "y": 4}
        try:
            for k, v in zip(a, b):
                del b[k]
                print("never")
        except RuntimeError as err:
            return "RuntimeError: " + str(err)
        return "no error"


    def set_mutate() -> str:
        s: set[int] = set()
        s.add(1)
        t: set[int] = set()
        t.add(2)
        try:
            for x, y in zip(s, t):
                s.add(3)
        except RuntimeError as err:
            return str(err)
        return "no error"


    def plain() -> str:
        a: dict[str, int] = {"a": 1, "b": 2}
        b: dict[str, int] = {"x": 1, "y": 2}
        out: str = ""
        for k, v in zip(a, b):
            out = out + k + str(v)
        l1: list[int] = [1, 2, 3]
        l2: list[int] = [4, 5]
        n: int = 0
        for x, y in zip(l1, l2):
            n = n + x * y
        return out + "/" + str(n)


    print(del_first())
    print(del_second())
    print(set_mutate())
    print(plain())

# ── 原 445_comp_iter_guard.py ──
def _case_3() -> None:
    # 445: 推导式里迭代 dict/set 也要在容器大小变化时抛可捕获的 RuntimeError。
    # 之前没有守卫，会落到 DictKeyAt 的越界 panic（进程退出、except 接不住）；
    # 语句级 for 从 0.363 起就有守卫。


    d: dict[int, int] = {1: 1, 2: 2, 3: 3}
    try:
        xs = [d.pop(k) for k in d]
    except RuntimeError as e:
        print("caught", e)

    s: set[int] = set([1, 2])
    ys = [x * 2 for x in s]
    print(ys)

    e: dict[str, int] = {"a": 1}
    try:
        zs = [e.setdefault(k + "x", 1) for k in e]
    except RuntimeError as err:
        print("caught2", err)

_case_1()
_case_2()
_case_3()

# --- expect ---
# RuntimeError: dictionary changed size during iteration
# Set changed size during iteration
# RuntimeError: dictionary changed size during iteration
# RuntimeError: dictionary changed size during iteration
# 1=10 2=20 /15
# RuntimeError: dictionary changed size during iteration
# never
# RuntimeError: dictionary changed size during iteration
# Set changed size during iteration
# axby/14
# caught dictionary changed size during iteration
# [2, 4]
# caught2 dictionary changed size during iteration
