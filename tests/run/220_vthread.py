# vthread（绿色线程）综合：go/join、通道收发、pingpong、睡眠顺序、join 异常、GC 挂起、churn、大量阻塞协程
# 0.382 合并自 9 个文件：220_vthread_go_join.py, 221_vthread_chan_unbuffered.py, 222_vthread_chan_pingpong.py, 223_vthread_sleep_order.py, 224_vthread_join_exc.py, 225_vthread_gc_suspend.py, 226_vthread_churn.py, 227_vthread_chan_types.py, 229_vthread_many_blocked.py
# 每段包在独立函数里，避免同名变量/类跨段冲突。

# ── 原 220_vthread_go_join.py ──
def _case_1() -> None:
    # 虚拟线程先验：go/join 基础、多参/str 参、嵌套 go、返回值丢弃
    def add(a, b):
        print("add", a + b)

    def greet(name):
        print("hello", name)

    h1 = go add(3, 4)
    h2 = go greet("vt")
    join(h1)
    join(h2)
    print("joined")

    def child(x):
        print("child", x)

    def parent():
        hc = go child(9)
        join(hc)
        print("parent done")

    hp = go parent()
    join(hp)

    def ret():
        return "unused"

    go ret()
    print("end")

# ── 原 221_vthread_chan_unbuffered.py ──
def _case_2() -> None:
    # 无缓冲 channel：生产者/消费者握手，值按序转移
    def producer(ch, n):
        i = 0
        while i < n:
            ch.send(i * i)
            i = i + 1

    ch = make_chan(int, 0)
    h = go producer(ch, 6)
    total = 0
    i = 0
    while i < 6:
        v = ch.recv()
        total = total + v
        i = i + 1
    join(h)
    print("sum", total)

# ── 原 222_vthread_chan_pingpong.py ──
def _case_3() -> None:
    # 缓冲 ping-pong N 轮 + 满阻塞让出 + 空阻塞让出
    def ping(ch1, ch2, n):
        i = 0
        while i < n:
            v = ch1.recv()
            ch2.send(v + 1)
            i = i + 1

    def pong(ch1, ch2, n):
        i = 0
        while i < n:
            v = ch2.recv()
            ch1.send(v + 1)
            i = i + 1

    a = make_chan(int, 1)
    b = make_chan(int, 1)
    a.send(0)
    hp = go ping(a, b, 50)
    hq = go pong(a, b, 50)
    join(hp)
    join(hq)
    print("last", a.recv())

    def filler(ch):
        ch.send(10)
        ch.send(20)
        ch.send(30)
        print("filler done")

    c = make_chan(int, 2)
    hf = go filler(c)
    sleep(5)
    print(c.recv(), c.recv(), c.recv())
    join(hf)

# ── 原 223_vthread_sleep_order.py ──
def _case_4() -> None:
    # sleep 让出：交错顺序由 deadline 确定
    def nap(name, ms):
        sleep(ms)
        print(name)

    h1 = go nap("a", 30)
    h2 = go nap("b", 10)
    h3 = go nap("c", 20)
    sleep(40)
    join(h1)
    join(h2)
    join(h3)
    print("awake")

# ── 原 224_vthread_join_exc.py ──
def _case_5() -> None:
    # join 异常传播：vthread 内逃出的异常在 join 处重抛
    def bad(n):
        if n > 0:
            raise ValueError("boom")

    h = go bad(3)
    try:
        join(h)
    except ValueError:
        print("caught value")
    print("after")

    def inner():
        raise KeyError("k")

    def outer():
        inner()

    h2 = go outer()
    try:
        join(h2)
    except KeyError:
        print("caught key")

    def selfcatch():
        try:
            raise RuntimeError("x")
        except RuntimeError:
            print("self caught")

    h3 = go selfcatch()
    join(h3)
    print("done")

# ── 原 225_vthread_gc_suspend.py ──
def _case_6() -> None:
    # 只被挂起 vthread 持有的对象（含自环）强制 GC 后存活
    def holder(ch):
        keep = ["held", "by", "suspended"]
        cycle = []
        cycle.append(cycle)
        ch.send(1)
        ch.recv()
        print(keep[0], keep[1], keep[2], len(cycle))

    ch = make_chan(int, 0)
    h = go holder(ch)
    ready = ch.recv()
    i = 0
    while i < 40000:
        junk = ["junk" + str(i), i, i * 2]
        i = i + 1
    ch.send(9)
    join(h)
    print("gc ok", ready)

# ── 原 226_vthread_churn.py ──
def _case_7() -> None:
    # 1000 次 spawn/join churn + 嵌套 go 深度 20
    def work(ch, i):
        ch.send(i)

    ch = make_chan(int, 0)
    total = 0
    i = 0
    while i < 1000:
        h = go work(ch, i)
        total = total + ch.recv()
        join(h)
        i = i + 1
    print("churn", total)

    def nest(depth):
        if depth > 0:
            h = go nest(depth - 1)
            join(h)

    hn = go nest(20)
    join(hn)
    print("nest ok")

# ── 原 227_vthread_chan_types.py ──
def _case_8() -> None:
    # channel 元素类型薄切片：float / str / unknown
    def sendf(ch):
        ch.send(2.5)
        ch.send(0.125)

    cf = make_chan(float, 0)
    hf = go sendf(cf)
    x = cf.recv()
    y = cf.recv()
    join(hf)
    print(x + y)

    def sends(ch):
        ch.send("hello")
        ch.send("world")

    cs = make_chan(str, 2)
    hs = go sends(cs)
    join(hs)
    print(cs.recv(), cs.recv())

    def sendu(ch):
        ch.send(42)
        ch.send("mixed")

    cu = make_chan(unknown, 0)
    hu = go sendu(cu)
    a = cu.recv()
    b = cu.recv()
    join(hu)
    print(a)
    print(b)

# ── 原 229_vthread_many_blocked.py ──
def _case_9() -> None:
    # 多个 vthread 阻塞在同一 channel，FIFO 唤醒
    def waiter(ch, tag):
        v = ch.recv()
        print(tag, v)

    ch = make_chan(int, 0)
    h1 = go waiter(ch, 1)
    h2 = go waiter(ch, 2)
    h3 = go waiter(ch, 3)
    sleep(2)
    ch.send(10)
    ch.send(20)
    ch.send(30)
    join(h1)
    join(h2)
    join(h3)
    print("all served")

_case_1()
_case_2()
_case_3()
_case_4()
_case_5()
_case_6()
_case_7()
_case_8()
_case_9()

# --- expect ---
# add 7
# hello vt
# joined
# child 9
# parent done
# end
# sum 55
# last 100
# 10 20 30
# filler done
# b
# c
# a
# awake
# caught value
# after
# caught key
# self caught
# done
# held by suspended 1
# gc ok 1
# churn 499500
# nest ok
# 2.625
# hello world
# 42
# mixed
# 1 10
# 2 20
# 3 30
# all served
