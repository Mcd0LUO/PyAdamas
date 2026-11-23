# 写 pya-lsp 时撞到的语言 / 运行时缺口（最小复现）

环境：PyAdamas 仓库，编译器 build/lead/pyadamas（2026-11-23 19:xx 构建）。
所有复现均在本机实测；**[已修复]** 标注修复的大致版本，**[仍存在]/[部分修复]** 是当前仍能复现的。

---

## 1. [已修复] 文本模式 sys.stdin 在管道上阻塞到 EOF

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
# 旧：readline elapsed≈2.0（等 EOF）；新：elapsed≈0.00007
~~~

**已修复**（0.447）：非普通文件的 file_fill 改成一次 read(2)，不再用 fread 凑 8192 字节；
管道上「有多少拿多少」，EOF 才返回 0。实测 `(printf 'hello\n'; sleep 2) | prog` 从 2.0s 降到 0.00007s。

pya-lsp 仍然用 `open(0, "rb", 0)`（二进制、无缓冲）而不是文本模式 sys.stdin：

- LSP 帧是字节语义（Content-Length 是 UTF-8 字节数），二进制读不经过换行翻译；
- 无缓冲读让 fd 可读性 == 是否有待处理字节，didChange debounce 的 fd 轮询才可靠
  （有缓冲时下一帧可能已在 FILE 缓冲里，fd 却不可读）。

所以「切回 sys.stdin」现在**可行但没必要**：文本模式能用，但二进制更稳、且是 debounce 的前提。

---

## 2. [已修复] 跨模块 class 的字段读 / 方法调用

~~~python
# xmod_a.py
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

# xmod_b.py
import xmod_a

def main() -> None:
    p: xmod_a.Point = xmod_a.make(3, 4)
    print(p.x)
    print(p.norm())

main()
~~~

~~~bash
pyadamas run --rebuild -o xmod_b xmod_b.py && ./xmod_b
# 3
# 25
~~~

**已修复**：跨模块 class 的字段读/写与方法调用都可用（本机实测输出 3 / 25）。
pya-lsp 现在也直接用跨模块 class 做文档表/符号表（`Symbol` 等），不再需要「不透明句柄」workaround；
minijson 的 JVal 包装层也已删除，值统一是动态 JSON。

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
f = open(0, "rb", 0)     # pya-lsp 现在的写法：fd 0 + 二进制 + 无缓冲
chunk = f.read(5)       # 运行期拿到的是「按字节读的 str」，类型系统里也是 str
# chunk: bytes = f.read(5)  -> error: annotated as 'bytes' but assigned 'str'
~~~

影响：二进制读只能当 str 用；好在 pya-lsp 读到的 Content-Length 帧体本来就是合法 UTF-8，
所以能用。建议要么让 read 在二进制模式返回 bytes，要么文档明确「binary mode 只是按字节计数的 str」。

---

## 5. [部分修复] json.parse 已落地；但 pyadamas check 仍不校验模块属性

动态 JSON 已落地（0.447）：`json.parse(text) -> unknown`、`json.stringify(v) -> str`，
外加 get/get_or/has/type/as_dict/as_list/as_str/as_int/as_bool/len/keys。pya-lsp 的
minijson.py 已改成这层的薄封装（543 行 -> 约 210 行），手写 parser/stringifier 删除。

仍存在的坑：**check 不报「模块没有这个属性」，build 才报**：

~~~python
import json

def main() -> None:
    x: int = json.nonexistent_attr(1)
    print(x)

main()
~~~

~~~bash
pyadamas check modattr.py   # ok: ... 3 statements ...     <-- check 放行
pyadamas build modattr.py   # error: module 'json' has no attribute 'nonexistent_attr'
~~~

踩坑：不能用 check 判断模块 API 是否存在，要 build/run。pya-lsp 的诊断因此只能反映
check 能发现的那类错误（类型/语法），模块成员错误要等到真正 build 才暴露。

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

## 7. [可用但有坑] asyncio.wait_readable 做 fd 轮询：没有 asyncio.TimeoutError

pya-lsp 的 didChange debounce 需要「等下一帧，但最多 N 毫秒」，语言里没有 select/poll，
但 asyncio 提供 `wait_readable(fd)`（协程）和 `wait_for(aw, timeout)`，可以在同步代码里用
`asyncio.run(...)` 包一层当带超时的 poll 用：

~~~python
import asyncio

async def _wait_readable_for(ms: int) -> bool:
    try:
        await asyncio.wait_for(asyncio.wait_readable(0), float(ms) / 1000.0)
        return True
    except TimeoutError:          # 注意：不是 asyncio.TimeoutError！
        return False
~~~

坑：`asyncio.TimeoutError` 不存在（编译期报 module 'asyncio' has no attribute 'TimeoutError'），
超时抛的是内置 `TimeoutError`（rt_async.c 的 timeout 注入）。
另外每次 asyncio.run 会建/销毁一个事件循环，只能在低频路径（didChange）用。

---

## 8. [可用] 动态 JSON 与 unknown 注解的几个实用点

- 函数参数/返回值可以直接标 `unknown`（typecheck_builtin 认 unknown/Any），
  动态值在模块内传递不用包装类；跨模块也可以（minijson 的访问器就是 `-> unknown`）。
- `dict[str, list[str]]` 这类「容器套容器」的模块级全局可以用（MODULE_MEMBERS 静态表）。
- `json.get_or(d, key, json.null())` 是「取不到给默认值」的正规写法；`json.get` 缺失键抛异常，
  做宽容访问器时要先 `json.type(v) == "dict"` 或改用 get_or。
- `json.stringify` 对 str 参数直接给出带引号转义的 JSON 字面量（等价于旧手写 quote），
  对 dict/list 给出紧凑 JSON；非 ASCII 默认 \\uXXXX，正好满足 LSP 回包纯 ASCII 的要求。
- `open(0, "rb", 0)`：fd 0 + 二进制 + 无缓冲，pya 接受 buffering=0；
  `len_bytes(s)` 是 O(1) 的 UTF-8 字节数（Content-Length 用）。

---
