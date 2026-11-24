# unknown 上的下标：不许静默按机器语义解码（docs/stdlib-round2.md §7.3）。
#
# 静态类型 unknown 的值现在按**运行期真实种类**分派：list/tuple 取元素、
# dict 取值、str 取码点。以前一律发 IndexLoad（把 8 字节当 PyAdamasList
# 指针解引用）：unknown dict 上是「list index out of range」这种假象，
# unknown str / 小整数上直接段错误（0.495 实测）。
#
# 复现路径不依赖 tarfile：queue.Queue 的元素类型就是 unknown（list[unknown]），
# get_nowait() 返回 unknown —— 与原生函数返回 unknown 是同一条降级路径。
import queue


def fetch(q: queue.Queue) -> unknown:
    return q.get_nowait()


q = queue.Queue()

# tuple（元素是堆对象）/ list / 标量元组（未进 RC 索引）/ dict / str
q.put((1, "one"))
v = fetch(q)
print(v[0], v[1], v[-1])

q.put([10, 20, 30])
w = fetch(q)
print(w[1], w[-2])

q.put((7, 8))
t = fetch(q)
print(t[0], t[1])

q.put({"k": 7})
d = fetch(q)
print(d["k"])

q.put("abcd")
s = fetch(q)
print(s[1], s[-2])

# 非容器：小整数是能安全排除的（malloc 不返回 64KB 以下地址），抛 TypeError。
q.put(99)
n = fetch(q)
try:
    print(n[0])
except TypeError as e:
    print(type(e).__name__)

# None 同口径（P3 表示里 None 是裸 0）。
q.put(None)
m = fetch(q)
try:
    print(m[0])
except TypeError as e:
    print(type(e).__name__)

# 越界仍按容器口径报 IndexError（不是 TypeError）。
q.put([5])
o = fetch(q)
try:
    print(o[3])
except IndexError as e:
    print(type(e).__name__)

# --- expect ---
# 1 one one
# 20 20
# 7 8
# 7
# b c
# TypeError
# TypeError
# IndexError
