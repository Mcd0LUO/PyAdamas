# 标准库第二轮优化（stdlib-opt2）

分支 `stdlib-opt2`（从 master 分出，含 0.487）。目标：把 `stdlib/**/*.py` 的 54 个文件
逐个过一遍，能优化的优化、不能的给结论。**不碰** `fractions.py` / `decimal.py`
（另一条线在改），**不改** `src/` `runtime/` `CHANGELOG.md` `HANDOFF.md` 现有文档。

## 0. 结论摘要

- **覆盖 54/54**：12 个文件有改动（见 §3），其余 42 个逐个给「无变化 / 判负 / 跳过」结论（§2 矩阵）。
- **新增 21 个基准**：`tests/bench/cases/` 从 17 个变成 38 个（§5）。
- **新增 5 个 CPython 对拍用例**：`tests/run/570–574`，expect 全部来自 CPython 3.14 实跑（§4）。
- **修掉 1 个真 bug**：`fnmatch` 此前不是整串匹配 —— `fnmatch.fnmatchcase(".hidden", "[a-z]*")`
  返回 True（CPython False），根因是 pa 的 `re` 引擎里 `Pattern.match` 与 `search` 同义、
  `\Z` 不当作结尾锚点。现在编译时前置 `^`，`fnmatch` 侧补 `end()` 判定（§3.2 / §7.1）。
- **可测收益（背靠背 min-of-5，pa 自身）**：fnmatch −24%（89.5→67.7ms）、
  urllib.parse −10%（152.2→136.5ms）、pprint −11%（493.5→439.8ms）、csv −15%（79.0→67.0ms）、
  shlex −16%（20.9→17.6ms）。其余改动是「消掉二次曲线」的防御性修复（pprint/textwrap 同类模式。
  n 大时才可见），以及无可测差异的整理（glob / calendar / zipfile / gzip）。
- **3 个编译器侧缺口**（只记录，不改 `src/`）：`re` 锚点与 `match` 语义、`unknown` 下标不报错、
  局部变量丢失类方法面。详见 §7。

## 1. 方法与测量口径

- 构建：`cmake -S . -B build/std -G Ninja -DCMAKE_BUILD_TYPE=Release`（测的是运行速度）。
- 基准：`python3 tests/bench/run_bench.py --pyadamas build/std/pyadamas`。每个用例都是
  「同一份源码分别给 pa 编译和 CPython 解释」，取进程 wall time 的最小值（`--repeat`）。
- 单项 before→after 用**背靠背两组 min-of-5**：`git stash push -- stdlib/<file>` → 跑 →
  `git stash pop` → 跑。stdlib 是编译器在 build 时读的源码，不需要重编 `pyadamas`，所以
  同一台机器上可以直接 A/B。
- **噪声警告**：本轮期间同一台机器上还有别的代理在编译/跑测试。全量 A/B（38 个用例，stash
  整个 `stdlib/`）里，**没改过的** heapq +37.7%、rc_cycles +61.3%、io +39.3%、argparse +36.2%，
  说明单次全量对照的方差可以到 ±40%。因此：全量对照表（附录 A）只用于看方向；
  §2 里的 before→after 一律取「同一时刻背靠背两组 min-of-5」，并且只报超过噪声的方向。
- 语义：每个改动的可观测行为都用 CPython 实跑钉桩（`tests/run/570–574`），另外对 fnmatch 做了
  两轮穷举/随机差分（§4）。

## 2. 54 个模块矩阵

`基准` 列：`已有` = 本轮之前就有 `bench_*.py`；`新增` = 本轮加的；`—` = 没有基准。
`daily` 列：`bench/daily/` 的 25 个日常语料是否直接用到（只有 csv / functools / statistics 命中；
math / re / os / sys / datetime / collections 是原生模块）。
`before→after` 单位 ms，是 pa 侧 wall time（背靠背 min-of-5）。行数是**本轮之后**的当前值

| # | 模块 | 行数 | 基准 | daily | 改动 | before→after | 结论 |
|---|------|-----:|------|:-----:|------|--------------|------|
| 1 | tarfile.py | 1158 | 新增 | — | 无 | 96.6（CPython 452.6，4.7x） | 无变化：整读整写 + 头解析，已是 4x；`_calc_chksums`/`_oct` 都是 ≤512 字节的小循环，改不动 |
| 2 | decimal.py | 995 | — | — | **跳过** | — | 边界外（另一条线在改） |
| 3 | configparser.py | 858 | 新增 | — | 无 | 80.5（CPython 290.9，3.6x） | 无变化：解析已是单遍状态机；`_indent_of`/`_repr_str` 是短串循环 |
| 4 | argparse.py | 806 | 新增 | — | 无 | 9.4→12.8（噪声内；CPython 157.7，12x+） | 无变化：`_fill` 的 `s += " "` 单次最多 width 个字符，实测不是热点 |
| 5 | zipfile.py | 713 | 新增 | — | `close()` 的 `out = out + p` → `b"".join` | 206.6→204.9（−1%） | 防御性修复（二次曲线），基准里 40 个成员规模看不出差别；瓶颈在 deflate/inflate 原生 + 文件 IO |
| 6 | urllib/parse.py | 623 | 新增 | — | 4 处查表 + `_clean` 短路（§3.3） | 152.2→136.5（−10%） | **优化了** |
| 7 | statistics.py | 562 | 已有 | ✓ | 无（第一轮已改 `_rank`） | 18.9（CPython 410.7，21x） | 无变化：第一轮改动未退化 |
| 8 | textwrap.py | 530 | 已有 | — | 无（第一轮已改缓冲） | 542.4（CPython 820.9，1.5x） | 无变化：第一轮改动未退化；剩余瓶颈是逐字符解释执行 |
| 9 | struct.py | 505 | 新增 | — | 无 | 25.6（CPython 33.3，1.3x） | 无变化：`pack`/`unpack` 每字段走 unknown 分派 + int→bytes 逐字节；要再快得给 `pack` 加静态格式特化（要动 src/） |
| 10 | difflib.py | 505 | 新增 | — | 无 | 185.9（CPython 288.6，1.5x） | 无变化：`find_longest_match` 已是 CPython 的 DP 口径；`_indices` 有双查但每次只一次 dict 命中 |
| 11 | pprint.py | 498 | 新增 | — | 6 处 `out = out + …` → `list[str]`+`join`，`_spaces` 改 `" "*n`（§3.1） | 493.5→439.8（−11%） | **优化了** |
| 12 | gzip.py | 364 | 新增 | — | 多成员 `result = result + p` → `b"".join` | 无可测差异（单成员走 early return） | 防御性修复：多成员 gzip（concatenated）在成员多时才是二次曲线 |
| 13 | calendar.py | 355 | 新增 | — | `formatmonth` 的行拼接 → 缓冲（§3.5） | 61.0→60.4（−1%） | 改动保留（目标模式），基准无可测差异：热点是每格 `str()`+`center` |
| 14 | shutil.py | 347 | — | — | 无 | — | 跳过：`copyfile` 是 64KB 分块 IO、`rmtree`/`copytree` 是 syscall 主导；`copytree` 的 `name in ignored` 是 O(n·m)，但目录名表通常很小，加 set 反而不划算（要把 list 转 set） |
| 15 | io.py | 317 | 新增 | — | 无 | 34.9（CPython 45.1，1.3x） | 判负：`StringIO.write` 每次 `buf[:pos]+s+buf[end:]` 是 O(n)（CPython 摊还 O(1)），pa 的 str 不可变且没有可变缓冲原语 —— 要动运行时 |
| 16 | hashlib.py | 288 | 已有 | — | `_hexdigest` 改成 `list[str]`+`join` | 已有基准只走 sha256 intrinsic，不覆盖；md5/sha1 的 hex 输出是 16/20 字节，量级太小 | 防御性修复（对长摘要成立的模式）；sha256 走 `__pa_sha256*` intrinsic，本来就快 |
| 17 | csv.py | 281 | 新增 | ✓ | `writerow`/`_format` 提升循环不变量，`DictReader` 提升 `len()`（§3.4） | 79.0→67.0（−15%） | **优化了** |
| 18 | tempfile.py | 266 | — | — | `_random_name` 的 8 字符拼接 → 缓冲 | 不可测（8 字符） | 改动保留（同一模式），收益在噪声下 |
| 19 | string.py | 263 | 已有 | — | 无（第一轮已改 Template 缓冲） | 含在 string_textwrap 里 | 无变化：第一轮改动未退化 |
| 20 | urllib/request.py | 246 | — | — | 无 | — | 跳过：libcurl 原生 + 网络，不构造本地基准（会引入端口/超时不确定性；既有 276/277 用例覆盖功能） |
| 21 | logging.py | 245 | 新增 | — | 无 | 6.3（CPython 97.9，16x） | 无变化 |
| 22 | heapq.py | 213 | 已有 | — | 无（第一轮已改 `list[int]`） | 18.3–27.0（CPython 62.6，2.7x） | 无变化：第一轮改动未退化。**注意**：`list[int]` 收窄后 `queue.PriorityQueue` 传 `list[unknown]` 会绕过静态检查（§7.3） |
| 23 | glob.py | 211 | 新增 | — | `escape` → 缓冲；`_match` 提升 `len()`（§3.6） | 60.0→61.9（噪声内） | 改动保留但**无可测收益**：热点是 `os.listdir`/`isdir` 系统调用，字符串层不是瓶颈 |
| 24 | queue.py | 194 | 新增 | — | 无 | 19.9（CPython 166.2，7.7x） | 无变化 |
| 25 | itertools.py | 169 | 已有 | — | 无 | 9.8（CPython 28.5，2.9x） | 无变化 |
| 26 | typing.py | 152 | — | — | 无 | — | 跳过：纯类型别名/标识符，没有可执行热点 |
| 27 | base64.py | 150 | 已有 | — | 无 | 23.1（CPython 65.2，2.8x） | 无变化：b64 编解码已走 C intrinsic |
| 28 | contextlib.py | 144 | — | — | 无 | — | 跳过：`@contextmanager` 只是生成器转发（0–4 位置参数定长分派），没有循环 |
| 29 | secrets.py | 141 | 新增 | — | 无 | 4.8（CPython 40.3，8.7x） | 无变化 |
| 30 | fractions.py | 140 | — | — | **跳过** | — | 边界外（另一条线在改） |
| 31 | shlex.py | 143 | 新增 | — | `quote` / `join` 的逐字符拼接 → 缓冲（§3.7） | 20.9→17.6（−16%） | **优化了** |
| 32 | urllib/error.py | 131 | — | — | 无 | — | 跳过：异常类 + 一小段消息拼接（`while i < len(lines)` 的 8 行以内） |
| 33 | linalg.py | 108 | — | — | 无 | — | 无变化：薄 BLAS 封装，热路径在 cblas（`dgemm`/`dgemv`）；`_zeros` 是 n 次 append |
| 34 | collections/abc.py | 106 | — | — | 无 | — | 跳过：纯 ABC 声明（`class Sequence(Container, Iterable)` 之类），无运行期代码 |
| 35 | functools.py | 87 | — | ✓ | 无 | — | **判负**：`cmp_to_key` 是 O(N³)（每次 key 调用重做一次插入排序 + 一次 `order.index` 回填）。改成「增量插入 + 名次 +1 批量回填」能降到 O(N²)，但 daily 语料与基准都不用它；且真正的 O(N log N) 需要「key 阶段算完后统一比较」，语言层不支持 —— 记在 §6 |
| 36 | hmac.py | 81 | — | — | `hexdigest` → 缓冲 | 不可测（16–32 字节摘要） | 改动保留（同一模式） |
| 37 | traceback.py | 76 | 新增 | — | 无 | 2.7–4.6（CPython 149.8，40x+） | 无变化：帧格式化 + 源行缓存 |
| 38 | copy.py | 76 | 新增 | — | 无 | 7.6–8.6（CPython 33.1，2–4x） | 无变化：`copy_list` 就是 `xs[:]`；`deepcopy_*` 的逐元素 `isinstance` 分派是设计口径 |
| 39 | zlib.py | 73 | 新增 | — | 无 | 33.3→36.5（CPython 32.8，账面 0.85–1.2x） | 无变化：`compress`/`decompress` 已直调系统 zlib；包装层只剩 `bytes(cap)` 分配 + 切片。基准里 pa 与 CPython 基本同速甚至更慢，说明**包装层之外的提升空间已经到顶** |
| 40 | fnmatch.py | 111 | 新增 | — | 缓冲 + `re.escape` 查表 + **锚定修正**（§3.2） | 89.5→67.7（−24%） | **优化了**（并且修了语义 bug，见 §4.2） |
| 41 | binascii.py | 58 | — | — | 无 | — | 无变化：已经是「256 项查表 + 逐字节 append」，与本次优化的模式同形，无可再改 |
| 42 | errno.py | 54 | — | — | 无 | — | 跳过：常量表 |
| 43 | enum.py | 51 | — | — | 无 | — | 跳过：真正的枚举语义在编译器侧（class 语句被前端拦截），本文件只有空基类与两个恒等占位函数 |
| 44 | bisect.py | 51 | 已有 | — | 无 | 7.9–13.5（CPython 46.1，4x+） | 无变化：`list[int]` 收窄是达成现状的原因；`list[float]` 扩展判负已记在模块头（需要编译期泛型） |
| 45 | stat.py | 47 | — | — | 无 | — | 跳过：常量 + 位掩码判定 |
| 46 | uuid.py | 44 | — | — | 无 | — | 无变化：`__str__` 是 5 次切片 + 4 次 `+`，常量级；不引入 f-string 是为了保持与 CPython 逐字一致的输出 |
| 47 | platform.py | 40 | — | — | 无 | — | 跳过：常量与环境变量读取 |
| 48 | abc.py | 32 | — | — | 无 | — | 跳过：编译期 ABC 语义的占位 |
| 49 | dataclasses.py | 28 | — | — | 无 | — | 跳过：装饰器占位（真正的 dataclass 生成在前端） |
| 50 | getpass.py | 27 | — | — | 无 | — | 跳过：终端 IO + 单个 `while` 读行 |
| 51 | codecs.py | 20 | — | — | 无 | — | 跳过：encode/decode 转发到原生 |
| 52 | _gzipio.py | 20 | — | — | 无 | — | 跳过：`read_bin`/`write_bin` 两个薄封装 |
| 53 | collections/__init__.py | 6 | — | — | 无 | — | 跳过：仅 re-export 原生 deque/Counter 等 |
| 54 | urllib/__init__.py | 5 | — | — | 无 | — | 跳过：包标记 |

合计 54：**优化 5 个**（pprint / fnmatch / urllib.parse / csv / shlex），**防御性修复或改动保留 7 个**
（zipfile / gzip / calendar / glob / hashlib / hmac / tempfile —— 同一模式，无可测差异或量级太小），
**无变化 21 个**，**判负 2 个**（io.StringIO.write / functools.cmp_to_key），
**跳过 19 个**（2 个边界外 + 17 个常量/薄封装/编译期占位）。

## 3. 改动明细

### 3.1 pprint（−11%）

6 处累加式字符串拼接改成 `list[str]` + `"".join`：`_safe_repr` 的 dict/list/tuple/set 四个分支、
`_dict_items_text`、`_seq_items_text`、`_str_text`、`_dict_text`。另外 `_spaces(n)` 从逐字符
`s = s + " "` 改成 `" " * n`（一次分配）。

语义：每个分支的输出逐字节不变（同一批 append 顺序、同一个 `delimnol` 状态机），只把
「每次拼接复制已积累的前缀」换成「末尾一次分配」。

**踩坑记录**：同一个函数里第二次 `parts = ["["]` 会丢掉第一次 `parts: list[str] = ["{"]` 的
注解，codegen 报 `method 'append' is not supported`。所以四个分支各用一个变量名
（`parts` / `lparts` / `tparts` / `sparts`）。见 §7.4。

### 3.2 fnmatch（−24%，并修 bug）

三件事：

1. `translate()` 的片段缓冲从 `res = res + …` 改成 `list[str]` + `join`；
2. `re.escape(c)`（每字符一次原生调用）换成 128 项查表 `_SPECIAL` + 非 ASCII 回落；
   表内容由「pa 的 `re.escape` 与 CPython 对全部 ASCII 逐字符比对」得出（两者完全一致：
   9–13、32、35、36、38、40–43、45、46、63、91–94、123–126）；
3. **锚定修正**：`fnmatch`/`filter` 在编译时前置 `^`（`translate()` 的输出保持原样，
   既有用例 539 钉着 `translate("*")[:6] == "(?s:.*"`），再用 `search()` +
   `m.end() == len(name)` 补齐终点。

为什么 (3) 是必须的：pa 的 `re` 引擎里 `Pattern.match` 与 `search` 同义（不锚定起点），
`\Z` / `\z` / `\A` 也不是锚点（被当成空操作）。所以原来的 `p.match(name)` + `(?s:…)\Z`
实际上退化成「子串匹配」：

```python
fnmatch.fnmatchcase(".hidden", "[a-z]*")   # 修前 True，CPython False
re.compile("[a-z]").match("1abc")          # True（CPython False）
re.compile("abc\\Z").match("xabc")         # True（CPython False）
```

改法是纯 stdlib 的：`^` 在 pa 上确实是起点锚（实测），`end()` 可用。这样修完反而更快
（失败路径在第一个字符就返回，不用整串回溯）。

### 3.3 urllib.parse（−10%）

- `_SCHEME_TAB` / `_SAFE_TAB` / `_HVAL_TAB`：三张 128 项查表，替换 `ch in _SCHEME_CHARS`
  （56 字符）/ `ch in _ALWAYS_SAFE`（66 字符）这类每字符线性扫描，以及 `_is_hex`；
- `_quote_text` 用 `_SAFE_TAB` 先判 ASCII 安全字符，再回落 `ch in safe`；
- `quote_from_bytes` 的 ASCII 安全字节直接 `chr(b)`，不再 `bytes([b]).decode("latin-1")`
  （每字节一次分配 + 一次解码）；
- `unquote_to_bytes` 的 `%XX` 用表直接拼字节，不再 `int(string[i+1:i+3], 16)`（两次切片 + 解析）；
- `_clean`：先 `"	" not in text and …` 短路，绝大多数 URL 不含控制字符时省掉 3 次整串 `replace`。

语义：所有分支与原实现等价（`_ALWAYS_SAFE` 与 `safe` 的判定顺序不变；非 ASCII 仍然走 `encode`；
`%` 后面不足两位或非十六进制仍然按字面）。对拍见 571。

### 3.4 csv（−15%）

`writerow` / `DictWriterText._format`：把「引号判定」相关的实例字段与派生值提到循环外，
`len(row)` / `len(vals)` 只取一次。`DictReader`：`len(table)` / `len(header)` 提到循环外。

语义：`need` 的判定顺序（`quoting` → `delimiter`/`quotechar` 包含 → CR/LF 包含 → `QUOTE_NONE`
报错）逐条保持；`str(row[i])` 仍然逐字段调用（这条是 0.322 修段错误留下的口径，不能动）。
对拍见 573。

### 3.5 calendar（−1%，改动保留）

`formatmonth` 的逐行 `out = out + …` 换成 `parts` + `join`。基准里看不出差别是因为一个月的
行数只有 8 行左右，二次曲线的 n 太小。保留是为了和 textwrap/pprint 同一口径。

### 3.6 glob（无可测收益，改动保留）

`escape` 逐字符拼接 → 缓冲；`_match` 把 `len(name)` / `len(pat)` 提到循环外（原先回溯分支
每轮各查最多 4 次长度）。A/B 是 60.0 → 61.9ms（噪声内）：**热点在 `os.listdir` / `isdir` 的
系统调用**，`_collect` 对每个目录项都要 `isdir`。要再快只能上 `os.scandir`（pa 没有）或减少
`isdir` 调用，都不是纯字符串层能解决的。

### 3.7 shlex（−16%）

`quote()` 的逐字符 `out = out + s[i]`、`join()` 的逐词拼接 → `list[str]` + `join`；顺带把
`len(s)` 提到循环外。语义：单引号内 `'"'"'` 的转义序列、空串返回 `''`、安全字符集判定
都不变。对拍见 572。

### 3.8 其余 5 个文件（防御性修复，量级太小或无差异）

| 文件 | 改动 | 为什么保留 |
|------|------|------------|
| zipfile.py | `close()` 末尾 `out = out + p` → `b"".join(parts)` | 成员数多时是 O(段数 × 总长)；40 成员规模看不出 |
| gzip.py | 多成员解的 `result = result + p` → `b"".join` | 拼接式 gzip（concatenated members）成员多时是二次曲线 |
| hashlib.py | `_hexdigest` 逐字节拼接 → 缓冲 | md5/sha1 的 hex 输出；bench 只覆盖 sha256 intrinsic |
| hmac.py | `hexdigest` 同上 | 同模式 |
| tempfile.py | `_random_name` 8 字符拼接 → 缓冲 | 同模式，量级 8 字符 |

## 4. CPython 对拍证据

### 4.1 新增用例（`tests/run/570–574`）

全部用「程序体 + `# --- expect ---`」，expect 由 CPython 3.14 实跑生成，`diff` 为空：

| 用例 | 覆盖 | 结果 |
|------|------|------|
| 570_fnmatch_anchor.py | 15 个模式 × 9 个名字的 `filter` + `fnmatchcase` 矩阵 | 逐字节一致 |
| 571_urllib_quote_edges.py | `quote`/`quote_plus` 14 组输入、`unquote`/`unquote_plus` 往返、`quote_from_bytes`（含 0x00/0x80/0xff）、`unquote_to_bytes`（截断 `%4`、孤立 `%`） | 逐字节一致 |
| 572_shlex_quote_roundtrip.py | 12 个词的 `quote` → `split` 往返、`join`、3 个未闭合引号/结尾反斜杠的错误文本 | 逐字节一致 |
| 573_csv_quoting.py | 默认 dialect、自定义 `delimiter`/`quotechar`/`lineterminator`、`QUOTE_ALL`、`\r\n` 与空行解析、`delimiter="\|"` | 逐字节一致 |
| 574_glob_match_escape.py | 固定目录树上的 12 个模式 + `**` 递归 + `escape` | 逐字节一致（比集合前先 `sorted()`，见下） |

### 4.2 fnmatch 穷举/随机差分（证明改写是保语义的）

1. **`translate()` 等价性**：用 CPython 同时跑「旧实现（`res = res + …` + `re.escape`）」和
   「新实现（缓冲 + 查表）」两个纯函数，输入是 7082 个模式（全部 ASCII 单字符、前 30 个字符的
   全部二字符组合、30 个手写边界模式、4000 个随机模式）→ **0 处差异**。
2. **匹配矩阵**：120 个随机模式 × 120 个随机名字 = **14400 个 `fnmatchcase` 结果**，pa 与
   CPython **0 处差异**（修锚定之前 `[a-z]*` vs `.hidden` 这类会差）。
3. `translate()` 的**正则文本**与 CPython 3.14 不同（CPython 用 `\z` 与原子组 `(?>.*?b)`，
   pa 用 `\Z` 且没有原子组）—— 这是既有差异，不在本轮范围；本轮只保证「匹配结果」一致。

### 4.3 已知未钉桩的既有差异（记录，不在本轮修）

- `csv`：`QUOTE_NONE` 下遇到分隔符，CPython 报 `Error("need to escape, but no escapechar set")`，
  pa 报 `Error("need to escape delimiter or quotechar, but quoting is QUOTE_NONE")`；
  非法 `delimiter`（长度 2）CPython 抛 `TypeError`，pa 抛 `csv.Error`。
- `glob`：pa 返回**排序后**的 list，CPython 返回 `os.scandir` 的目录序（模块头已声明）。
- `unquote("%41%ff")`：`.decode("utf-8", "replace")` 的替换字符与 CPython 不同（编码层差异，
  不在 stdlib）；`unquote_to_bytes(...).decode("latin-1")` 对 0xFF 也不同。571 里只用
  不受影响的行钉桩。
- `re`：`Pattern.match` 与 `search` 同义、`\Z`/`\z`/`\A` 不锚定（§7.1）。fnmatch 已绕开，
  但 `re` 的用户面仍然错。

## 5. 新增基准（21 个）

`tests/bench/cases/`：`pprint` `fnmatch` `shlex` `urllib_parse` `csv` `calendar` `struct`
`zlib` `gzip` `configparser` `logging` `difflib` `io` `queue` `copy` `tarfile` `zipfile`
`argparse` `traceback` `secrets` `glob`。每个 <1s、输出确定性、`--filter <name>` 可单跑。

本轮结束时的全量（`--repeat 3`，pa / CPython / 加速比）：

```
bench_argparse               7.2ms     158.5ms    21.91x
bench_base64_hashlib        23.1ms      65.2ms     2.82x
bench_bisect                13.3ms      57.5ms     4.33x
bench_calendar              47.2ms     217.2ms     4.60x
bench_collections            5.3ms      49.6ms     9.35x
bench_configparser          66.5ms     289.5ms     4.35x
bench_copy                   9.6ms      19.7ms     2.07x
bench_csv                   70.0ms     106.1ms     1.52x
bench_datetime               9.9ms      51.4ms     5.22x
bench_difflib              198.7ms     284.7ms     1.43x
bench_fnmatch               67.7ms      83.0ms     1.23x
bench_glob                  61.9ms     106.6ms     1.72x
bench_gzip                  42.8ms      46.5ms     1.08x
bench_heapq                 27.0ms      72.4ms     2.68x
bench_io                    36.3ms      60.0ms     1.65x
bench_itertools              9.7ms      40.2ms     4.13x
bench_json                  10.6ms      91.1ms     8.61x
bench_logging                6.3ms     103.1ms    16.48x
bench_math                  30.5ms     139.8ms     4.59x
bench_pprint               432.6ms     560.1ms     1.29x
bench_queue                 21.6ms     166.5ms     7.70x
bench_secrets                4.2ms      36.9ms     8.76x
bench_shlex                 30.8ms     142.1ms     4.61x
bench_statistics            19.1ms     410.2ms    21.46x
bench_string_textwrap      556.1ms     819.5ms     1.47x
bench_struct                25.1ms      33.1ms     1.32x
bench_tarfile              114.8ms     470.0ms     4.09x
bench_traceback              3.6ms     149.9ms    42.03x
bench_urllib_parse         126.5ms     198.1ms     1.57x
bench_zipfile              201.2ms     327.5ms     1.63x
bench_zlib                  43.1ms      36.7ms     0.85x
```

（另有 7 个 `bench_rc_*` RC 基准，与本轮无关，未列出。）

## 6. 判负项与根因

| 项 | 现状 | 根因 | 要动哪一层 |
|----|------|------|------------|
| `io.StringIO/BytesIO.write` | 每次写入 O(n)（CPython 摊还 O(1)） | pa 的 `str`/`bytes` 不可变，没有可变缓冲原语（没有 `bytearray`） | 运行时：加可变字符串缓冲 / `bytearray` |
| `functools.cmp_to_key` | O(N³)：每次 key 调用重做插入排序 + `order.index` 回填 | pa 的 `sorted(key=…)` 先算完所有 key 再按 key 的机器字比较，没有「key 对象 `__lt__`」可用；CPython 的 O(N log N) 依赖它 | 运行时的排序协议；纯 stdlib 最多降到 O(N²) |
| `zlib` 包装层 | pa 33–43ms vs CPython 33ms（0.85–1.2x） | 已经直调系统 zlib，包装层只剩 `bytes(cap)` 分配 + 切片；剩余差距是每次调用的固定开销 | 运行时的 `bytes(n)` 零填充分配（`calloc` + tag 初始化） |
| `struct.pack/unpack` | 1.3x | 每字段走 unknown 分派 + int→bytes 逐字节；没有静态格式特化 | 编译期：对字面量 fmt 做特化（要动 src/） |
| `string_textwrap` | 1.47x | 第一轮已把拼接换成缓冲；剩余是逐字符解释执行（每字符 ~10 条 IR） | 编译器（字符循环向量化/内联） |
| `pprint` / `difflib` / `csv` | 1.29x / 1.43x / 1.52x | 都是「每次调用重建整棵树的中间串/字典」，算法与 CPython 同形；差距在字符串分配与 unknown 比较 | 运行时的字符串分配与 tag 比较（`pa_any_cmp_t`） |

## 7. 编译器侧瓶颈（**只记录，不改 `src/`**）

### 7.1 `re` 引擎没有起点/终点锚（严重）

```python
re.compile("a.c").match("xabc")     # pa True / CPython False  ← match 与 search 同义
re.compile("[a-z]").match("1abc")   # pa True / CPython False
re.compile("abc\\Z").match("xabc")  # pa True / CPython False  ← \Z 被忽略
re.compile("abc$").search("abc\n") # pa 0..3 / CPython 0..3（$ 正常）
re.compile("^a.c").search("xabc")   # pa None / CPython None    ← ^ 正常
```

`^` 与 `$` 工作，`\A` / `\Z` / `\z` 不工作，且 `match()` 不复刻「从 0 开始」。影响面：所有
用 `re.match` 做「整串/前缀」判定的 stdlib 代码（fnmatch 已绕开，其余模块没有用）。
建议在 `re` 的 `Pattern.match` 里显式加起点锚，并把 `\A`/`\Z` 映射到首/尾。

### 7.2 「先把返回值赋给局部变量」会丢类方法面

```python
rd.extractfile("a.txt").read()        # 可以
f = rd.extractfile("a.txt")           # f 是 BytesIO
f.read()                              # error: method 'read' is not supported by the code generator yet
```

同理 `m = re.search(...)` 在函数里 `m.start()` 也会报同样的错（在模块顶层可以）。
代码生成阶段对「局部变量的方法调用」比「链式调用」窄。

### 7.3 `unknown` 上的下标/方法不报错，直接按机器语义解码

```python
v = pq.get_nowait()      # 返回 unknown
total += v[0]            # 编译通过；运行期把 8B 值当指针 → SIGSEGV
```

本轮在写 `bench_queue` 时踩到：`queue.PriorityQueue` 里放元组、再取出来 `t[0]` →
段错误。这条与 7.1 叠加后还有一个隐患：`heapq` 第一轮把形参收成 `list[int]`，但
`queue.PriorityQueue` 传的是 `list[unknown]`，静态检查被绕过、运行期按 int 解释堆元素
——建议给 `heappush` 等加「实参元素类型必须与形参一致」的检查（或者给 `queue` 一条
`list[int]` 的专用路径）。

### 7.4 分支内重新赋值会丢掉 `list[T]` 注解

```python
parts: list[str] = ["{"]
…
parts = ["["]          # 这里之后 parts 不再是 list[str]
parts.append(", ")     # error: method 'append' is not supported
```

本轮 pprint 改造时踩到，最后每个分支用独立变量名绕开。同类现象：`"".join(parts)` 在
`parts` 失去 `list[str]` 时也会报 `str.join() expects a list of strings`。

### 7.5 其它（有用的正向事实）

- `chr(ord)` 对 >127 正确，可以用于构建 128+ 项的字符查表。
- `b"".join(list[bytes])` 支持且比 `out = out + p` 快（zipfile/gzip 已用）。
- `" " * n` / `"\n" * n` 是一次分配，`s = s + " "` 是 O(n²)。
- `re.escape` 的 ASCII 特殊集与 CPython 完全一致（可安全查表替代）。

## 8. 语义影响（相对 0.487）

**没有新的类型收窄**：本轮没有把任何 `list` 改成 `list[int]` / `dict[str, int]`，所以不存在
「此前能编译、现在编译期被拒」的用例（这是本轮刻意避开的：heapq/bisect 那种收窄要有
明确的语义收益才做，本轮的可选收窄都不具备）。

唯一的行为变化是 **fnmatch 的整串匹配**（修 bug）：此前 `fnmatch("x.hidden", "[a-z]*")` 这类
「子串命中」为 True，现在是 False。已有的 185/539 两个 fnmatch 用例在修前修后都 ok
（它们用的模式在两种语义下结果相同）。

| 门 | 结果 |
|----|------|
| `cmake --build build/std -j8` | ok |
| `PYADAMAS_TEST_JOBS=8 bash tests/run_tests.sh build/std build/std/pyadamas` | **485 ran ok / 0 失败**（480 旧 + 5 新） |
| `bash tools/difftest.sh build/std/pyadamas` | **455 个用例 / 0 个不一致**（450 旧 + 5 新） |
| `bash tests/check_size.sh` | PASS（最大文件 tarfile.py 1158 行 < 1200） |
| `ctest --test-dir build/std` | 全过 |

## 附录 A：全量 A/B 对照（含噪声，只用于看方向）

`git stash push -- stdlib/` 前后各跑一次 `--repeat 5`。注意**没改过**的模块也在 ±40% 内波动
（heapq +37.7%、rc_cycles +61.3%、io +39.3%、argparse +36.2%），原因是同一台机器上还有
别的代理在编译。真正的收益幅度以上文背靠背的单项数据为准。

| 用例 | before | after | delta |
|------|-------:|------:|------:|
| bench_argparse | 9.4 | 12.8 | +36.2% |
| bench_base64_hashlib | 30.4 | 22.9 | -24.7% |
| bench_bisect | 13.5 | 7.9 | -41.5% |
| bench_calendar | 47.7 | 47.0 | -1.5% |
| bench_collections | 7.5 | 6.7 | -10.7% |
| bench_configparser | 80.5 | 66.1 | -17.9% |
| bench_copy | 8.6 | 7.6 | -11.6% |
| bench_csv | 65.8 | 67.0 | +1.8% |
| bench_datetime | 14.1 | 10.6 | -24.8% |
| bench_difflib | 185.9 | 191.8 | +3.2% |
| bench_fnmatch | 99.4 | 67.9 | -31.7% |
| bench_gzip | 45.9 | 34.3 | -25.3% |
| bench_heapq | 18.3 | 25.2 | +37.7% |
| bench_io | 34.9 | 48.6 | +39.3% |
| bench_itertools | 11.3 | 9.8 | -13.3% |
| bench_json | 6.3 | 8.1 | +28.6% |
| bench_logging | 6.3 | 5.4 | -14.3% |
| bench_math | 16.9 | 16.9 | +0.0% |
| bench_pprint | 493.9 | 421.9 | -14.6% |
| bench_queue | 20.7 | 19.9 | -3.9% |
| bench_rc_calls | 4.8 | 3.9 | -18.8% |
| bench_rc_cycles | 13.7 | 22.1 | +61.3% |
| bench_rc_gen | 67.8 | 65.7 | -3.1% |
| bench_rc_lists | 1.6 | 1.3 | -18.8% |
| bench_rc_literals | 11.6 | 12.3 | +6.0% |
| bench_rc_live | 8.4 | 4.9 | -41.7% |
| bench_rc_strings | 3.8 | 3.9 | +2.6% |
| bench_secrets | 4.8 | 2.5 | -47.9% |
| bench_shlex | 28.8 | 24.4 | -15.3% |
| bench_statistics | 18.9 | 19.0 | +0.5% |
| bench_string_textwrap | 542.4 | 554.6 | +2.2% |
| bench_struct | 25.6 | 30.7 | +19.9% |
| bench_tarfile | 96.6 | 111.3 | +15.2% |
| bench_traceback | 4.6 | 2.7 | -41.3% |
| bench_urllib_parse | 157.7 | 124.4 | -21.1% |
| bench_zipfile | 192.3 | 201.0 | +4.5% |
| bench_zlib | 33.3 | 36.5 | +9.6% |

