# 只读借用槽细化（0.338）vthread/chan 保守跳过：共享容器跨 vthread
# 修改、chan 收发接管引用。flag-on/off 行为一致（非 CPython 语法）。

def worker(xs: list[str], ch) -> None:
    for i in range(100):
        s = xs[0]
        ch.send(len(s))
    xs.append("w")

def vt_main() -> int:
    xs: list[str] = ["vt"]
    ch = make_chan(int, 0)
    h = go worker(xs, ch)
    total = 0
    for i in range(100):
        total += ch.recv()
    join(h)
    return total + len(xs)

def borrow_local_vt(n: int) -> int:
    xs: list[str] = ["local", "borrow"]
    ch = make_chan(int, 0)
    h = go worker(xs, ch)
    total = 0
    for i in range(n):
        total += ch.recv()
    join(h)
    return total + len(xs)

print(vt_main())
print(borrow_local_vt(100))

# --- expect ---
# 202
# 503
