# 写 pya-lsp 时撞到的语言 / 运行时缺口（最小复现）

环境：PyAdamas 仓库，编译器 build/lead/pyadamas（2026-11-23 15:28 构建，下文称「当前编译器」）。
所有复现均在本机实测。标 **[仍存在]** 的是 15:28 版本仍然复现的，标 **[已修复]** 的仅供验收参考。

---

## 1. [仍存在] 文本模式 sys.stdin 在管道上阻塞到 EOF，交互式 stdio 协议无法使用

~~~python
# repro_stdin.py
import sys, time

t0 = time.time()
line = sys.stdin.readline()      # 客户端已经写完这一整行，管道保持打开
print("readline elapsed=%.3f line=%r" % (time.time() - t0, line.strip()))

t0 = time.time()
data = sys.stdin.read(5)         # 同理
print("read(5) elapsed=%.3f data=%r" % (time.time() - t0, data))
~~~

~~~bash
{ printf 'Content-Length: 5\r\n'; sleep 2; } | ./repro_stdin
# 期望：立即打印 elapsed≈0
# 实际：readline elapsed≈2.0（sleep 结束 / EOF 才返回）；read(5) 同样阻塞
~~~

根因（只读排查，未改代码）：runtime/rt_file.c 的 file_fill 用
fread(raw + rn, 1, sizeof raw, fp)，sizeof raw = 8192。glibc 的 fread 会循环 read()
直到凑满 8192 字节或 EOF；所以写端不关时，文本模式 read / readline / for line in
全部卡住。文件输入或写端关闭时「碰巧」正常。

影响：任何 LSP / JSON-RPC / 交互式 REPL 式的 stdio 协议都会死锁（客户端写完一帧等回包，
服务器在等 8KB）。文档里写的 for line in sys.stdin: 在管道交互场景实际不可用。

修复建议：file_fill 改成一次 read(fileno(fp), raw, sizeof raw)（EINTR 重试），
不要用 fread 的大块读；或给 stdin 提供 sys.stdin.buffer / 二进制读接口。
注：二进制分支（open("/dev/stdin", "rb")）走 fread(chunk, 1, n)，n 字节到齐即返回，
readline 走 fgetc（underflow 一次 read(2)）——这两条路径是好的，也是 pya-lsp 现在的 workaround。

---

## 2. [仍存在] 跨模块 class 的字段读 / 方法调用不可用

~~~python
# a.py
class Point:
    x: int
    y: int

    def __init__(self, x: int, y: int) -> None:
        self.x = x
        self.y = y

    def norm(self) -> int:
        return self.x * self.x + self.y * self.y


def make(x: int, y: int) -> Point:
    return Point(x, y)
~~~

~~~python
# b.py
import a

def main() -> None:
    p: a.Point = a.make(3, 4)   # 跨模块构造 OK
    print(p.x)                  # error: attribute access is only supported as a method call
    print(p.norm())             # error: method 'norm' is not supported by the code generator yet

main()
~~~

~~~bash
pyadamas build b.py -o b
# b.py:6:12: error: attribute access is only supported as a method call
# b.py:7:17: error: method 'norm' is not supported by the code generator yet
# 2 errors in code generation
~~~

workaround（pya-lsp 采用）：把跨模块 class 值当**不透明句柄**，只通过定义模块的函数访问
（kind(v) / get(v, key) / at(v, i)），字段访问全部留在定义模块内。实测函数式 API 完全可用。

---

## 3. [已修复] 链式属性读 / 写

~~~python
class Inner:
    value: int
    def __init__(self) -> None:
        self.value = 0

class Outer:
    inner: Inner
    def __init__(self) -> None:
        self.inner = Inner()

def main() -> None:
    o: Outer = Outer()
    o.inner.value = 42      # 旧版：error: unsupported assignment target
    print(o.inner.value)    # 旧版：error: attribute access is only supported as a method call

main()
~~~

14:54 的 build/lead 复现；15:28 的 build/lead 上 build + 运行都正常（输出 42），
self.inner.value 在方法内也正常。**当前不需要 workaround**。

---

## 4. [仍存在] 二进制模式 file.read(n) 返回 str 而不是 bytes（语义含糊）

~~~python
f = open("/dev/stdin", "rb")
chunk = f.read(5)       # 运行期拿到的是「按字节读的 str」，类型系统里也是 str
# chunk: bytes = f.read(5)  -> error: annotated as 'bytes' but assigned 'str'
~~~

影响：二进制读只能当 str 用；好在 pya-lsp 读到的 Content-Length 帧体本来就是合法 UTF-8，
所以能用。建议要么让 read 在二进制模式返回 bytes，要么文档明确「binary mode 只是按字节计数的 str」。

---

## 5. [仍存在] json.parse 尚未落地，且 pyadamas check 不会报「模块没有这个属性」

~~~python
import json

def main() -> None:
    v = json.parse('{"a": 1}')
    print(v["a"])

main()
~~~

~~~bash
pyadamas check t.py     # ok: t.py (... 3 statements ...)   <-- check 放行
pyadamas build t.py     # t.py:4:19: error: module 'json' has no attribute 'parse'
~~~

踩坑：不能用 check 判断模块 API 是否存在，要 build/run。pya-lsp 因此先用手写
tools/lsp/minijson.py；等 json.parse 落地后只需替换 minijson.parse 的实现（其余函数式访问层不变）。

---

## 6. [踩坑] 旧编译器构建出的可执行文件在退出时崩（refcount increment on a freed object）

用 2026-11-18 的 build/release（或 build/dev）构建 tools/lsp/main.py 能成功，但产物一启动/退出就报：

~~~bash
$ ./pya-lsp-release < /dev/null
pyadamas: runtime error: refcount increment on a freed object
$ echo $?
1
~~~

15:28 的 build/lead 构建同一份源码正常。tools/lsp/build.sh 因此对每个候选编译器做
「构建 + 空 stdin 自检」，自检失败就换下一个候选，避免把坏产物交给冒烟测试。
（这条更像旧构建/运行时的 bug，不是语言特性缺口，但对「用编译器自己写工具」影响很直接。）
