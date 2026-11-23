# tools/lsp —— 用 PyAdamas 写的 Language Server

pya-lsp 完全由 .pya 实现、由 pyadamas build 编译成原生可执行文件。
这是 dogfooding：它把「协议要什么、语言缺什么」直接暴露出来。

## 文件

| 文件 | 说明 |
|---|---|
| main.py | LSP 服务器主体：stdio 传输、生命周期、文档同步、诊断、documentSymbol、hover、completion、definition、semanticTokens、references/rename、signatureHelp、documentHighlight、foldingRange、formatting、workspace/symbol |
| minijson.py | 动态 JSON 的**函数式访问薄层**：parse/stringify 走 runtime 的 json.parse / json.stringify，其余保留旧的宽容访问器语义（取不到返回 null、""、0、[]） |
| build.sh | 构建脚本，输出 pya-lsp |
| ../../tests/lsp/smoke.py | python3 冒烟测试（被测对象是编译出的 .pya 二进制），当前 **228 条断言** |
| ../../tests/lsp/smoke.sh | 冒烟测试入口（缺二进制时先构建） |
| LANGUAGE_GAPS.md | 写这个服务器撞到的语言/运行时缺口（含最小复现、现状） |

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
- PYA_LSP_DIAG_DEBOUNCE_MS：didChange 诊断合并窗口（毫秒，默认 300；0 = 关闭合并）。
- PYA_LSP_SKIP_BUILD=1：tests/lsp/smoke.sh 跳过构建。

## 传输层

- 读：Content-Length: N 头（其余头忽略）→ 空行 → 精确读 N 字节。
  **open(0, "rb", 0)**（二进制、无缓冲）；保底 /dev/stdin → sys.stdin。
  - 为什么仍是二进制：LSP 帧是字节语义（Content-Length 是 UTF-8 字节数），二进制读
    不经过换行翻译，也不依赖 stdin 的文本缓冲行为，跨平台/跨客户端更稳。
    0.447 之后文本模式 stdin 的流式语义已修（read(n)/readline 不再等 8KB/EOF），
    理论上 sys.stdin 也能用；保留二进制是为了**可靠性**，见 LANGUAGE_GAPS 第 1 条。
  - 为什么无缓冲（buffering=0）：fd 层的可读性 == 是否还有待处理字节，
    didChange debounce 用 asyncio.wait_readable 轮询 fd 判断「是否有下一帧」才可靠
    （有缓冲时下一帧可能已躺在 FILE 缓冲里，fd 却不可读）。每帧只有几百字节，
    逐字节 read(2) 的开销可忽略。
- 写：同样的 Content-Length 帧，sys.stdout.write + flush()。
  所有输出 JSON 都是纯 ASCII（非 ASCII 走 \\uXXXX），长度用 UTF-8 字节数计算（len_bytes）。
- 帧读取是同步阻塞的；debounce 的「等下一帧」用带超时的 fd poll（asyncio.wait_readable +
  wait_for），不消费数据、不额外线程。

## JSON 层（minijson.py）

0.447 起 runtime 有动态 JSON（json.parse / json.stringify / json.get / get_or / has /
type / as_dict / as_list / as_str / as_int / as_bool / len / keys），minijson.py 原来
543 行的手写 parser/stringifier 已删除，改成**约 210 行的薄封装**：

- parse / stringify 直接调 json.*；
- 访问器保留旧 JVal 版本的**宽容语义**（get 缺失键返回 null、as_str 非串返回 ""、
  size 非容器返回 0、at 越界返回 null……），LSP 主程序调用点因此一行没改；
- obj_set / arr_add 用于构造测试数据（原地改 dict/list）。

## 已实现的方法

| 方法 | 说明 |
|---|---|
| initialize | capabilities：full sync / hover / documentSymbol / completion(. 触发) / definition / semanticTokens(full) / references / rename / signatureHelp / documentHighlight / foldingRange / formatting / workspaceSymbol + legend |
| initialized / exit | 通知，不回包；shutdown 后的 exit 退出码 0，否则 1 |
| shutdown | 回 result: null，之后请求回 -32600 |
| textDocument/didOpen / didChange / didClose | 内存文档表；didChange 支持全量与增量（range）替换；**didChange 有 debounce**（见下） |
| textDocument/publishDiagnostics | 通知：临时文件 + pyadamas check，解析 file:line:col: severity: msg，转 0-based；同一次突发输入只发布最终版本 |
| textDocument/documentSymbol | 顶层 def/class/赋值（缩进敏感），SymbolKind 12/5/13 |
| textDocument/hover | 标识符 -> 定义处签名/类型（函数、形参、class、标注变量），拿不到返回 null |
| textDocument/completion | 三类候选：当前文档顶层符号、import 过的模块成员（静态表 + 模块名）、Python 关键字；光标前缀过滤，上限 100 条，返回 label/kind/detail |
| textDocument/definition | 光标标识符 -> 定义位置：任意缩进的 def/async def/class、顶层赋值、形参（跳所在函数）；找不到返回 null |
| textDocument/semanticTokens/full | 关键字/字符串/数字/注释/类名/函数名/变量；标准 5 元组 delta 编码，长度与列按 **UTF-16 code unit** |
| textDocument/references | 光标符号 -> 文档内所有整词出现位置（跳过注释与普通/三引号字符串），返回 Location[] |
| textDocument/rename | 文档内整词重命名，返回 WorkspaceEdit（单文件 changes）；不支持 prepareRename/documentChanges |
| textDocument/signatureHelp | 最内层调用括号 -> 文档内 def 的签名；activeParameter 按深度 0 逗号数（跳过字符串/注释/嵌套括号），支持跨行调用 |
| textDocument/documentHighlight | 整词扫描：写入/赋值目标/形参 Write(3)，其余 Text(1)；range 按 UTF-16 列 |
| textDocument/foldingRange | def/class 块（到块最后一行）、≥2 行连续注释、跨行三引号字符串；kind=region/comment，按 startLine 排序 |
| textDocument/formatting | 保守版：行首缩进按块层级归一到 4 空格 + 行尾空白清理；不重排表达式、不合并/拆分代码行；返回全文 TextEdit[] |
| workspace/symbol | 用 initialize 的 rootUri 递归扫 `.py`/`.pyi`（上限 2000 文件），复用 scan_symbols 返回 SymbolInformation[]；query 按子串过滤 |
| 其他 | 请求回 -32601 MethodNotFound；通知静默忽略 |

## completion 实现要点

- 光标前缀：取当前行光标前的标识符片段（UTF-16 列 -> 码点下标），label.startswith(prefix)。
- 候选来源与顺序：文档顶层符号（scan_symbols，函数/类/变量）→ import 过的模块
  （模块名本身 + 静态成员表 MODULE_MEMBERS，覆盖 math/sys/os/json/re/collections/
  itertools/functools/datetime/pathlib/time/subprocess/random/asyncio/base64/hashlib/logging/string
  的常用成员）→ Python 关键字。同名候选去重，先到先得（文档符号优先）。
- 不是作用域分析：局部变量/函数体成员/self. 后的类成员不会补；模块成员是静态表，
  新模块需要往 MODULE_MEMBERS 里加。对编辑器体验够用，但不是语义精确的补全。

## definition 实现要点

- 只做**文档内**跳转：光标词 -> 扫全文件所有 def/async def/class（含方法）与顶层赋值；
  形参命中时跳到所在函数定义（hover 用的是同一套参数解析）。
- 没有 workspace 索引：跨文件符号（import 的名字）不跳转，返回 null（合法 LSP 行为）。

## semanticTokens 实现要点

- legend：["class","function","variable","keyword","string","number","comment","parameter"]
  （tokenModifiers 为空）；类型索引必须与 initialize 里回给客户端的 legend 顺序一致。
- **UTF-16 编码**：deltaStart 和 length 都按 UTF-16 code unit 计（BMP 外字符如 😀 占 2），
  与 LSP 规范一致；冒烟测试用含「中文 + 😀（BMP 外，占 2 个 unit）」的字面量/注释/docstring 逐 token 校验长度与列。
- token 化：# 到行尾是注释；普通引号字符串（含反斜杠转义）；三引号跨行字符串按行
  继续（docstring 每行都是 string token，闭合行只覆盖引号本身）；数字（十进制/小数/
  指数/0x 0b 0o 前缀）；标识符 -> 关键字/类名/函数名/变量。

## references / rename 实现要点

- 两者共用一份「整词扫描」：跳过 `#` 注释与普通/三引号字符串，其余按标识符边界匹配，
  range 列按 UTF-16 code unit 计。
- references 只返回**当前文档**内的出现位置（没有 workspace 索引；跨文件符号返回空）。
- rename 生成单文件 `WorkspaceEdit.changes`，把命中的整词替换为新名字；
  不实现 `textDocument/prepareRename`，也不返回 `documentChanges` / 版本化编辑。

## signatureHelp / documentHighlight / foldingRange 实现要点

- signatureHelp：从文件头正向扫描并用括号栈找「包含光标的最内层 `(`」，取 `(` 前的标识符，
  复用 hover 的 `find_def_line` 提签名；`activeParameter` = 该 `(` 到光标间括号深度 0 的
  逗号数（跳过字符串/注释/嵌套括号）。支持跨行调用与无参函数（`parameters=[]`）；
  光标不在调用括号内返回 null。签名只认**当前文档**里的 def，跨文件/内建模块不识别。
- documentHighlight：整词扫描（跳过注释/字符串），命中定义、赋值目标、形参标 Write(3)，
  其余标 Text(1)。它是文本级扫描，不是作用域分析——同名但不同作用域的标识符会一起高亮。
- foldingRange：折叠 def/class 块（startLine 到块最后一行）、≥2 行连续注释、跨行三引号
  字符串；kind 用 region/comment，按 startLine 排序，空文档返回 []。

## formatting / workspace/symbol 实现要点

- formatting 只改两件事：**行首缩进**（按块层级归一到 4 空格，tab 按 4 计）与
  **行尾空白**。括号续行（上一行有未闭合的 `(`/`[`/`{`）与三引号字符串内部原样保留；
  不重排表达式、不合并/拆分代码行。`else/elif/except/finally` 先退一层再对齐。
  输出是逐行整行替换的 TextEdit（start.character 恒为 0），格式化已规范的文本返回 []（幂等）。
- workspace/symbol 用 initialize 的 `rootUri`（缺失时 `workspaceFolders[0].uri`，再退
  `rootPath`）；递归遍历时跳过隐藏目录、`__pycache__`、`build`、`node_modules`，最多 2000 个
  `.py`/`.pyi`；只索引顶层 def/async def/class/赋值（与 documentSymbol 同口径），返回
  `SymbolInformation`（location.uri 为 `file://` URI，range 指向定义行）。
- 已知限制：不实现 `formatting` 的 range 格式化（忽略 options.range）；不做表达式换行/
  对齐；workspace/symbol 没有 workspace 级缓存，每次请求重新扫盘；多根工作区只用第一个。

## didChange debounce（增量合并）

LSP 是单线程读循环，没有 timer 线程。实现方式：

1. didChange 到来后先把 contentChanges 全部应用到文档表；
2. 在最多 PYA_LSP_DIAG_DEBOUNCE_MS（默认 300ms）的窗口里，用 asyncio.wait_readable(0)
   带超时地轮询 stdin：如果紧接着还有 didChange，就一并吃掉（每来一帧重置窗口），
   最终只为最后状态跑一次 pyadamas check 并发布一次 publishDiagnostics；
3. 窗口内如果先到了**别的消息**（hover/请求等），把它放进 pending_bodies 队列，
   立刻发布诊断、回到主循环处理该消息——请求不会被 debounce 窗口拖住；
4. 客户端本来就把多次编辑合并成一帧 didChange 时，这里只是多等一个窗口就发布，无额外开销；
   PYA_LSP_DIAG_DEBOUNCE_MS=0 可关闭合并（每帧立即诊断）。

冒烟测试覆盖：连发两次 didChange（中间不读回包）只收到一次 publishDiagnostics，
且版本/内容都是最后一次（没有中间态闪屏）。

## 已知限制

- **补全不是作用域感知**：只补顶层符号 + import 模块成员 + 关键字；局部变量、类成员、
  字典 key 不补。模块成员来自静态表，新增内置模块要维护 MODULE_MEMBERS。
- **definition 只看文档内**：跨文件 import 的名字不跳转；没有 workspace/symbol 索引。
- **semanticTokens 仍非完整解析器**：支持三引号跨行字符串，但 f-string 里的表达式不细分
  成多个 token；变量一律 variable（形参暂未细分 parameter）；# 注释从行内任意位置到行尾。
- **诊断仍走全量 pyadamas check**：只是把突发 didChange 合并成一次；增量编译/只查改动
  区域还没做。诊断参数按码点计列（LSP 默认 UTF-16），非 BMP 字符同行错误列会有偏差。
- **传输层依赖无缓冲 fd 0 的 poll**：非 POSIX 平台上 asyncio.wait_readable 不可用时，
  stdin_ready() 会吞异常返回 False（退化成「不合并、每帧立即诊断」，仍然能用）。
- **只支持全量文档同步**（change=1）：增量 range 在服务器端应用，但客户端配置仍是 Full。
- **references / rename / signatureHelp 只认当前文档**：跨文件 import 的符号不参与；
  rename 不含 `prepareRename` 与 `documentChanges`。
- **formatting 是保守版**：只归一缩进与行尾空白，不重排/换行表达式；workspace/symbol 只索引
  顶层符号、每次重新扫盘。
- 没有实现（留给下一轮）：codeAction、completionItem/resolve、workspace 级诊断、
  semanticTokens/range 和 delta。
- 冒烟测试含时序护栏：每帧往返必须 < 1s（防止 stdin 读语义回退成「等 EOF」）。
