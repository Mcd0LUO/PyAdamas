# 标准库拓宽第一批（stdlib-batch1）

> 分支 **stdlib1**，基线 fdeea25（0.324）。需求驱动：日常脚本语料
> （docs/daily-corpus.md）里被手工绕行的刚需模块。与 P2（容器表示层）/
> P3（rt_any.c）并行开发，**不改主树、不推 master**；集成时 CHANGELOG
> 编号按落地顺序重排（本批预留 0.326）。
>
> 方法：差分测试基建先行——每个 API 对拍 CPython 3.14（正常/边界/错误
> 路径），tests/run/250–261 共 12 个用例，O0/O1/O2 三档输出逐字节一致；
> 错误路径（argparse 退出码 2 + usage）用**自子进程**对拍（同一份源码
> 两边跑，sys.argv[0] 区分执行形态）。

## 一、选型与理由

| 模块 | 日常频率/痛点 | 可行性 | 结论 |
|---|---|---|---|
| **argparse** | 极高：语料 a1/a5 手工解析 argv；CLI 脚本第一刚需 | 0.165 动态属性 + 本批前端使能后可纯 Python 自举 | ✅ 本批 |
| **configparser** | 高：语料 a2 手工解析 INI；配置脚本刚需 | 纯 Python 源，str 方法足够，零前端依赖 | ✅ 本批 |
| **gzip** | 高：数据日常（压缩日志/备份/传输） | 系统 zlib（cimport + C 胶水）+ 现有二进制 IO（cast） | ✅ 本批 |
| zlib | gzip 的支撑层，顺带交付 | compress2/crc32/adler32 + inflate 胶水 | ✅ 附带 |
| pathlib 补全 | 中：现有面已覆盖日常（见 compatibility.md） | 原生模块，补 API 要动 builtin_sigs/lower | ⏭ 后续（长尾桥接） |
| zipfile | 中：可复用本批 zlib 胶水 | ZIP 格式解析量大，独立一批更稳 | ⏭ 后续 |
| decimal / fractions 深化 | 中：金融精确计算 | **需任意精度 BigInt**（int64 回绕是文档化语义），深 | ❌ 不做，另票 |
| sqlite3 | 中：本地数据 | 需外部库 + 游标/事务语义面大；cimport 可绑但性价比低 | ❌ 评估后不做 |

**decimal/fractions 评估记录**：pyadamas 整数是 int64 回绕（compatibility.md
第一节），Decimal 的任意精度十进制与 Fraction 的大整数分子分母都无法在
现有数值表示上正确实现；fractions.py 现状（int 构造 + gcd 归一化）只覆盖
小整数场景。真要做需要独立的 BigInt 运行时（表示层 + 算术 + 格式化），
与 P2/P3 的 unknown 表示层工作正交但量级更大，单独立项。

**sqlite3 评估记录**：系统有 libsqlite3，cimport 绑定 sqlite3_open/exec/
prepare/step 技术上可行（本批 zlib 已验证该路径），但游标对象、类型亲和、
事务/提交语义、行工厂是一个完整子项目；日常脚本可用 csv + dict 绕行或
`from python import sqlite3` 桥接（~25µs/次调用开销，长尾可接受）。

## 二、交付清单

### 模块（stdlib/，纯 Python 源 + C 胶水）

| 文件 | 内容 |
|---|---|
| stdlib/argparse.py | ArgumentParser / Namespace / ArgumentTypeError / ArgumentError |
| stdlib/configparser.py | ConfigParser + 11 个异常类（消息逐字对齐） |
| stdlib/gzip.py | compress / decompress / open / GzipFile / BadGzipFile |
| stdlib/zlib.py | compress / decompress / crc32 / adler32 / error / 常量 |
| stdlib/_gzipio.py | 内部二进制 IO 助手（gzip.py 的 open 遮蔽内置 open，读写必须放无遮蔽模块） |
| stdlib/zlib.cpya | zlib C 绑定（link z + link "gzip_glue.c"） |
| stdlib/gzip_glue.c | raw deflate/inflate + zlib inflate 的 C 胶水（z_stream 留在 C 侧，Python 只传缓冲区） |

模块表（src/driver/build.cpp kModuleTable）登记：argparse / configparser /
gzip / zlib / _gzipio。

### argparse API 覆盖

- ArgumentParser(prog, description, epilog, add_help, allow_abbrev, usage)
- add_argument(*name_or_flags, action, nargs, const, default, type, choices,
  required, help, metavar, dest)
  - action：store / store_true / store_false / store_const / append /
    append_const / count / help
  - nargs：None / "?" / "*" / "+" / 整数 N
  - type：int / float / str / bool / 自定义可调用（按 str(conv) 名字分发，
    跨模块安全；自定义函数走间接调用）
- 位置参数、--long=value、短选项 bundling（-abc / -fval / -f=val）、
  -- 分隔、长选项唯一前缀缩写（allow_abbrev=False 可关）
- parse_args / parse_known_args（argv 省略取 sys.argv[1:]）
- Namespace：动态属性直读/直写/getattr/setattr/hasattr/vars/in、repr
  （插入序，同 CPython）
- format_usage / format_help / print_usage / print_help / error / exit
  - help 排版按默认 HelpFormatter：帮助列 min(最长调用+4, 24)、宽度 78、
    options: 节名、usage 超长折行（optionals/positionals 分段，同 CPython
    get_lines 算法）、description/epilog 词折叠
- 错误消息逐字对齐：expected one argument / expected N argument(s) /
  expected at least one argument / invalid int value / invalid choice /
  ignored explicit argument / ambiguous option / unrecognized arguments /
  the following arguments are required / conflicting option string /
  unknown action / store_true 带 nargs 的 TypeError

### configparser API 覆盖

- read / read_string / read_file；sections / options / has_section /
  has_option / defaults
- get / getint / getfloat / getboolean（raw / fallback / vars）
- items / set / add_section / remove_section / remove_option / write
- optionxform（可覆盖，默认 lower）
- DEFAULT 节、续行（缩进）、空行计入多行值、整行 #/; 注释、=/: 分隔、
  值 join 后 rstrip、严格模式重复节/选项报错
- BasicInterpolation %(key)s：%% 转义、10 层深度上限、before_set 校验、
  缺失键/语法/深度三类插值错误
- 异常：Error / NoSectionError / DuplicateSectionError /
  DuplicateOptionError / NoOptionError / InterpolationError /
  InterpolationMissingOptionError / InterpolationSyntaxError /
  InterpolationDepthError / ParsingError / MissingSectionHeaderError
  （消息逐字对齐，含 [line %2d] 与 repr(line) 格式）

### gzip / zlib API 覆盖

- gzip.compress(data, compresslevel=9, *, mtime=0)：**与 CPython 逐字节
  一致**（XFL 9→2 / <2→4 / 其余 0，OS=255，mtime 默认 0 可复现）
- gzip.decompress：多 member、member 间零填充、FEXTRA/FNAME/FCOMMENT/
  FHCRC 头标志、CRC/ISIZE 校验、BadGzipFile/EOFError 消息对齐
- gzip.open / GzipFile：rb/wb/rt/wt（r/w 简写）、read(size)/readline/
  readlines/迭代/write/close/flush/tell/readable/writable/closed/name、
  上下文管理、文本通用换行折叠、关闭/只读只写错误消息对齐
- zlib.compress / decompress / crc32 / adler32 / error；错误消息
  Error -3 incorrect header check / Error -5 incomplete or truncated
  stream 对齐

### 前端使能（argparse 的前提，也是通用语言修复）

| 编号 | 改动 | 位置 | 回归 |
|---|---|---|---|
| F1 | 跨模块动态属性：模块接口导出 needs_dict（格式 v11→v12）；getattr 字面量名/属性直写尊重 needs_dict | ir/module.h/cpp、driver/build.cpp（flat 拷贝）、typecheck/builtin.cpp、typecheck/stmt.cpp | 257 |
| F2 | setattr 非字面量名：值 coerce 进 unknown（float 装箱，修复漏装箱） | lower/builtin/builtin_reflect.cpp | 257 |
| F3 | 显式 object/Any 形参保持 unknown 域，不回填单态化（float 实参调用点装箱）；接口序列化 ann_unknown；负数字面量默认值（-1/-2.5） | typecheck/types.h、typecheck/typecheck.cpp、typecheck/call.cpp、lower/call.cpp、ir/module.cpp | 258 |
| F4 | 类型名当值：int/float/str 等是 TypeObj（机器表示＝类型名字符串），同一性/相等/print/str/容器/isinstance/issubclass 全打通；类型对象值当被调者明确拒绝 | typecheck/expr.cpp、typecheck/typecheck.h、lower/expr_assign.cpp、lower/expr_ops.cpp、lower/builtin/builtin_reflect.cpp、lower/call.cpp、lower/expr_dunder.cpp | 259 |
| B1 | 空容器字面量类型洞按目标填：字段赋值/重赋值、dict/list 下标存、append、return、实参（merge_elem + declare + bind_param + stmt_return）——修复 str 键退化成指针哈希、内容相同的键查不回 | typecheck/types.h、typecheck/typecheck.cpp、typecheck/stmt.cpp、typecheck/stmt_forms.cpp、typecheck/call.cpp | 260 |
| B2 | is/is not 对 unknown/Optional 操作数补 Mov 到机器字——修复 cmp.eq 按底层 str 走内容比较、None 当 NULL 解引用段错误 | lower/expr_ops.cpp | 261 |
| B3 | unknown 迭代/物化：for/推导式/list()/tuple() 经 pa_any_iter_list 按运行期种类展开（list 零拷贝 +1、str 拆码点、dict 取键） | rt_class.c、ir/ops.h（RtFn 末尾追加）、lower/stmt.cpp、lower/comp.cpp、lower/call.cpp、lower/builtin/builtin_container.cpp、ir/ir.cpp | 257/261 |
| B4 | vars() 合并动态属性 dict（AttrDict 借用 + DictUpdate） | lower/builtin/builtin_reflect.cpp、rt_class.c | 257 |
| B5 | repr(unknown)（ReprAny → pa_repr_any） | lower/builtin/builtin_text.cpp、ir/ops.h | 261 |
| B6 | int()/float() 错误消息 CPython 口径（with base %d: %r / could not convert string to float: %r） | rt_str.c | 261 |
| B7 | cimport 的 link "file.c"：cwd 找不到时回退 .cpya 所在目录（转绝对路径），stdlib 分发不依赖用户 cwd | cimport/cimport.cpp | 255/256 |

**RtFn 新增全部追加在枚举末尾**（AttrDict / AnyIterList / ReprAny），
不改既有编号（0.324 集成约定）。**pa_any_iter_list 放 rt_class.c**（与
动态属性同族），刻意不动 rt_any.c（P3 并行在改，避免冲突）。

## 三、CPython 差异（文档化边界）

沿用 language.md/compatibility.md 的既有边界，本批新增/相关：

1. **bool/None 显示**：unknown 域 bool/None 按裸 1/0（0.324 边界）。
   print(args.flag) 得 1 而非 True；print(args.x)（x 默认 None）得 0。
   取值用比较/条件（args.flag == True / args.x is None），或打印整个
   Namespace 时避开 bool/None 字段。getboolean 同理。
2. **unknown 下标**：unknown 值的下标按 list 语义（既有行为）；unknown
   里装 dict 时先赋给带标注的局部变量再下标（`d: dict[str,int] = args.m`）。
3. **print(bytes)**：pyadamas 直出内容（bytes/str 同表示），CPython 打
   repr；对拍用 binascii.hexlify 或比较。
4. **str(unknown 里的类型对象)**：得类型名（"int"），CPython 得
   "<class 'int'>"；静态 str(int) 已对齐。argparse 内部按名字分发依赖
   此口径。
5. **argparse**：help 宽度固定 78（CPython 按终端）；type=自定义函数时
   转换错误消息的类型名用 value（函数值无 __name__）；prog 省略取
   sys.argv[0] basename（同规则，值随执行形态）；互斥组/参数组/parents/
   fromfile_prefix_chars/prefix_chars/argument_default/version 动作不做。
6. **configparser**：构造只区分 interpolation=None（关闭）与默认
   （BasicInterpolation）；fallback=None 与省略不可分（None/0 同形），
   缺失且 fallback 为 None 时抛 NoOptionError；set 的 value 只收 str；
   SectionProxy 下标/read_dict/allow_no_values/inline_comment_prefixes
   不做；错误对象的 .section/.option 属性不做（消息一致）。
7. **gzip**：GzipFile 整读整写（CPython 流式），内存与文件大小成正比；
   追加/创建模式（a/ab/x/xb）与 fileobj 不做；compress 的 mtime 只收
   int；损坏流的 zlib.error 消息细分（invalid distance 等）不保证逐字。
8. **zlib**：compress/decompress 的 wbits 固定 15（zlib 格式）；gzip
   格式走 gzip 模块；compressobj/decompressobj 不做。

## 四、桥接兜底清单（from python import，~25µs/次）

长尾/冷门 API 不原生实现，脚本可直接桥接（docs/cpybridge-design.md）：

- argparse：互斥组/参数组/parents/自定义 HelpFormatter/version 动作/
  fromfile_prefix_chars/parse_intermixed_args
- configparser：SectionProxy 下标、read_dict、自定义 Interpolation
  （ExtendedInterpolation ${section:option}）、allow_no_values/
  inline_comment_prefixes/空节名、错误对象属性
- gzip：GzipFile 流式/追加模式/fileobj、compressobj、文件名头写入
  （FNAME 由 GzipFile(filename) 自动写）、mtime=None
- zlib：compressobj/decompressobj、wbits 变体、zlib 流式
- decimal / fractions（大整数）/ sqlite3 / zipfile / pathlib 长尾
  （with_name/suffixes/rglob/stat/rename/expanduser 等）/ shutil /
  tempfile / struct：整模块桥接

## 四半、已知问题（差分 harness 新发现，基线已验证，非本批引入）

**leaf 值（str/bytes/float 盒）× unknown 域的 RC 不对称**：0.321 把 str 改成
untracked leaf（不进 RC 索引）后，`pa_rc_inc_unknown/pa_rc_dec_unknown` 的
探针对 leaf 失效；当 leaf 值经 **unknown 返回/形参/槽** 流转时，保留/释放
一侧走探针（no-op）、另一侧走直接 inc/dec，引用计数不对称 → 小泄漏。
float 盒（0.324）在同类路径（fresh 调用结果直接参与算术/打印）也漏。

最小复现（**基线 fdeea25 同样复现**，非本批编译器改动引入）：

```python
# j12：动态 str 经 object 返回 → 泄漏 104B
def f(s: str) -> object:
    return s.lower()
print(f("X"))

# m5：fresh object 调用结果直接算术 → 泄漏 72B 盒
def f() -> object:
    return 1.5
print(f() * 2)
```

特征：`PYADAMAS_GC_CONSERVATIVE=1`（str 全跟踪回退口径）下 leaf×unknown
泄漏全部消失；与 GC 收集无关（`PYADAMAS_GC_DISABLE=1` 仍漏，gcstats
runs=0）。另：253 的 fallback 路径有 1 处 32B 异常对象泄漏（raise→catch→
丢弃，保守口径仍在；rt_exc.c 本批未改，j17/j18 最小复现不漏、需更多上下文，
属异常路径 RC 既有问题，一并移交）。
**绕行**：object 返回的调用结果先赋给带标注的局部变量再用（j14 口径，
`v: str = f("X")` 不漏）；fresh 调用结果先绑变量再算术（m2 口径）。

影响面：configparser.get（-> object）每次调用漏一个结果 str；argparse
type=float 转换漏盒；gzip 内部 bytes 链在含 unknown 中转的上下文漏中间量。
量级小（每处 72–560B）、**12/12 用例程序输出逐字节正确**、普通/ASan 套件
（detect_leaks=0）全绿。
根治属于 unknown 表示层工作流（P2/P3：leaf 探针/装箱口径统一），本批不做
表示层改动，记录移交。本批 12 个用例的 detect_leaks=1 抽查结果见 CHANGELOG。

## 五、验证

- 新增 tests/run/250–261（12 个），每个先对拍 CPython 3.14 再钉
  `# --- expect ---`，O0/O1/O2 逐字节一致。
- difftest（O0/O2 自差分）：见 CHANGELOG 0.326 验证节。
- 套件普通 + ASan（PYADAMAS_TEST_JOBS=8）：见 CHANGELOG。
- ASan detect_leaks=1 抽查新用例：见 CHANGELOG。
- check_size：无新增超限文件（新增 stdlib/*.py 与 runtime 函数不在
  src/ 1200 行规则内；src 改动落在有余量文件，driver/build.cpp/
  ir/ops.h/typecheck_types.h 为既有超限文件的最小增量）。

## 六、集成建议

- 与 P2（容器表示层）/P3（rt_any.c）并行：本批**未动 rt_any.c**
  （pa_any_iter_list 在 rt_class.c）；RtFn 追加在末尾，集成时若 P2/P3
  也追加了枚举，按落地顺序重排编号即可（rt_name 同步）。
- 模块接口格式 v11→v12（needs_dict + ann_unknown）：集成后旧 modcache
  自动失效（版本号校验），无需手动清。
- 合入前重跑：difftest → 250–261 → 普通+ASan 套件 → leak 抽查 →
  check_size。若主树期间改过 lower_expr_ops/lower_builtin_reflect/
  typecheck_call/module.cpp 相邻代码，重点复核 is-None 的 Mov 路径与
  bind_param 的 ann_unknown 早退。
