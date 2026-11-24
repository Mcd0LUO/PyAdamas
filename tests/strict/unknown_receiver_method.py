# expect: method 'read' is not supported by the code generator yet
# unknown 接收者上的方法调用：静态没有方法表，降级层只能明确拒绝（不静默按
# 机器字调用）。docs/stdlib-round2.md §7.2 记的「先把返回值赋给局部变量就
# 丢类方法面」复核后属于这一类——局部变量本身不丢类型（见
# tests/run/local_var_method.py），丢的是 unknown 值上的方法面。
import queue

q = queue.Queue()
q.put(1)
v = q.get_nowait()
v.read()
