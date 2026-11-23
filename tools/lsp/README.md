# tools/lsp —— 用 PyAdamas 写的 Language Server

pya-lsp 完全由 .pya 实现、由 pyadamas build 编译成原生可执行文件。
这是 dogfooding：它把「协议要什么、语言缺什么」直接暴露出来。

## 文件

| 文件 | 说明 |
|---|---|
| main.py | LSP 服务器主体：stdio 传输、生命周期、文档同步、诊断、documentSymbol、hover |
| minijson.py | 手写 JSON 解析/序列化（对象/数组/字符串/整数/浮点/bool/null + \uXXXX） |
| build.sh | 构建脚本，输出 pya-lsp |
| ../../tests/lsp/smoke.py | python3 冒烟测试（被测对象是编译出的 .pya 二进制） |
| ../../tests/lsp/smoke.sh | 冒烟测试入口（缺二进制时先构建） |
| LANGUAGE_GAPS.md | 写这个服务器撞到的语言/运行时缺口（含最小复现） |

## 构建 / 运行

    tools/lsp/build.sh                  # -> tools/lsp/pya-lsp
    tools/lsp/pya-lsp --stdio           # 直接作为 stdio LSP server
    PYADAMAS=/path/to/pyadamas tools/lsp/pya-lsp --stdio

build.sh 的编译器选择：显式 PYADAMAS 优先；否则按 mtime 从新到旧遍历 build/*/pyadamas，
对每个候选做「构建 + 空 stdin 自检」（老编译器构建的产物可能退出时报
refcount increment on a freed object），第一个自检通过的胜出。

环境变量：

- PYADAMAS：构建与诊断（pyadamas check）用的编译器路径；运行时若未设，用 argv[1]
  （不以 - 开头时）或 PATH 里的 pyadamas。
- PYA_LSP_DEBUG=1：把调试日志打到 stderr。
- PYA_LSP_SKIP_BUILD=1：tests/lsp/smoke.sh 跳过构建。

## 传输层

- 读：Content-Length: N 头（其余头忽略）→ 空行 → 精确读 N 字节。
  关键点：必须用二进制模式打开 stdin（open("/dev/stdin", "rb")）。
  sys.stdin 是文本模式，runtime 的 file_fill 用 fread(..., 8192, ...)，
  glibc 的 fread 会阻塞到填满 8192 字节或 EOF —— 管道上 LSP 客户端写完一帧等回包就死锁。
  二进制分支是 fread(chunk, 1, n)，n 字节到齐即返回；readline 走 fgetc 也只在行尾返回。
- 写：同样的 Content-Length 帧，sys.stdout.write + flush()。
  所有输出 JSON 都是纯 ASCII（非 ASCII 走 \uXXXX），长度用 UTF-8 字节数计算。

## 已实现的方法

| 方法 | 说明 |
|---|---|
| initialize | 返回 capabilities（full sync / hover / documentSymbol）、serverInfo |
| initialized / exit | 通知，不回包；shutdown 后的 exit 退出码 0，否则 1 |
| shutdown | 回 result: null，之后请求回 -32600 |
| textDocument/didOpen / didChange / didClose | 内存文档表；didChange 支持全量与增量（range）替换 |
| textDocument/publishDiagnostics | 通知：临时文件 + pyadamas check，解析 file:line:col: severity: msg，转 0-based |
| textDocument/documentSymbol | 顶层 def/class/赋值（缩进敏感），SymbolKind 12/5/13 |
| textDocument/hover | 标识符 -> 定义处签名/类型（函数、形参、class、标注变量），拿不到返回 null |
| 其他 | 请求回 -32601 MethodNotFound；通知静默忽略 |

## 已知限制

- **依赖 /dev/stdin（Linux）**：传输层用 open("/dev/stdin", "rb") 绕开文本模式 stdin 的
  阻塞 bug（见 LANGUAGE_GAPS.md 第 1 条）。将来 runtime 修好文本模式 stdin / 提供
  sys.stdin.buffer 后，只需要改 main.py 的 open_stdin() 一处即可切回 sys.stdin。
  非 Linux 平台没有 /dev/stdin 时会回退到 sys.stdin，但交互式管道场景会退化（等 8KB/EOF）。
- **二进制 f.read(n) 返回 str 而不是 bytes**：类型系统里 f.read(n) 恒为 str，二进制模式只是
  「按字节计数地读」；帧体是 UTF-8 所以能工作，但这是语义含糊点（LANGUAGE_GAPS.md 第 4 条）。
- 诊断走临时文件（$TMPDIR 或 /tmp），不按真实文件目录解析相对 import。
- 编译器诊断列号按码点处理；LSP 默认 UTF-16，非 BMP 字符在同行错误列上会有偏差。
- documentSymbol 只扫顶层（class/def 的 range 覆盖整个块），不展开成员。
- JSON 目前用手写 minijson.py（json.parse 尚未落地）。等它落地后只需替换
  minijson.parse 的实现，kind/get/at 这层函数式 API 不变。
- 冒烟测试含时序护栏：每帧往返必须 < 1s（防止 stdin 读语义回退成「等 EOF」）。
