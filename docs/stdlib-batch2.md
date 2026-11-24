# 标准库拓宽第二批（stdlib-batch2）

> 分支 **stdlib2**，基线 1501040（0.327）。需求驱动 + **轻量标尺**（用户明确
> 纠正过一批的过度对齐）：同参数同功能同结果即可——错误类型对、消息合理
> 不逐字；格式合法不字节级；差分只钉功能输出。与 p3next（lead 集成作 0.328）
> 并行，**不改主树、不推 master**；CHANGELOG 预留 **0.329**，集成时按落地
> 顺序重编号。
>
> 方法：沿用一批的差分基建——每个模块先对拍 CPython 3.14（正常/边界/错误
> 路径），再钉 tests/run/270–277（8 个），O0/O1/O2 三档逐字节一致。网络库
> 对拍用**本地 http.server 子进程**（端口 18326/18327），不打外网。

## 一、选型与理由

| 模块 | 日常频率/痛点 | 可行性 | 结论 |
|---|---|---|---|
| **zipfile** | 高：数据日常（打包/分发/读归档），一批明确 deferred | 一批 zlib 胶水（raw deflate/inflate/crc32）直接复用，纯 Python 解析 PKZIP 结构 | ✅ 本批 |
| **shutil** | 极高：copy/move/rmtree/copytree 是文件脚本第一刚需 | 二进制 IO 一批已验证（字节直通）；stat/utime/symlink 缺 → 小 C 胶水（~110 行） | ✅ 本批 |
| **tempfile** | 高：临时文件/目录是脚本卫生刚需 | mkstemp 需 O_EXCL 原语 → 小 C 胶水（~60 行）；mkdtemp 用 os.mkdir 重试 | ✅ 本批 |
| **urllib.request**（lead 追加） | 高：HTTP 取数/POST/webhook | 系统 libcurl 8.18（头+库齐全）；cimport + C 胶水封装 easy API，TLS 自带 | ✅ 本批 |
| pathlib 补全 | 中：现有面已覆盖日常 | Path 是原生 TyKind::Path，补方法要动 builtin_sigs/lower_method_native；NativePython 扩展只合并模块级名字，加不了原生类方法 | ⏭ 后续（长尾 with_name/suffixes/rglob/stat/rename 桥接） |
| io/StringIO | 中 | 纯类可写，但 csv.writer 等收 `f: file` 静态类型，StringIO 互不通；价值打折 | ⏭ 后续（连同 file 协议泛化一起做） |
| decimal/fractions 深化 | 中 | 需任意精度 BigInt（一批评估记录在案） | ❌ 不做 |
| sqlite3 | 中 | 外部库 + 完整游标/事务语义（一批评估记录在案） | ❌ 不做 |

**libcurl 可用性确认**：/usr/include/x86_64-linux-gnu/curl/curl.h（multiarch，
cc 默认搜索路径可达）+ libcurl.so（libcurl4-openssl-dev，8.18.0），
`cc x.c -lcurl` 链接通过。不需要退化桥接。

## 二、交付清单

### 模块（stdlib/）

| 文件 | 内容 |
|---|---|
| stdlib/zipfile.py | ZipFile / ZipInfo / BadZipFile / LargeZipFile / is_zipfile / ZIP_* 常量 |
| stdlib/shutil.py | copyfile/copy/copy2/copymode/copystat/move/rmtree/copytree/ignore_patterns/which + Error/SameFileError/SpecialFileError |
| stdlib/shutil_glue.c + _shutilc.cpya | stat/lstat/utimensat/readlink/symlink 胶水（错误归一化负码，跨平台稳定） |
| stdlib/tempfile.py | mkstemp/mkdtemp/NamedTemporaryFile/TemporaryDirectory/gettempdir/gettempprefix/tempdir/TMP_MAX |
| stdlib/tempfile_glue.c + _tempfilec.cpya | open(O_CREAT|O_EXCL|O_RDWR, 0600) 胶水 |
| stdlib/urllib/__init__.py + error.py + request.py | urlopen/Request/Response/HTTPMessage；URLError/HTTPError |
| stdlib/curl_glue.c + _curlc.cpya | libcurl easy API 全封装（link curl + link "curl_glue.c"，**glue 在前库在后**——Ubuntu gcc 默认 --as-needed，库先于引用它的目标出现会被丢弃） |

模块表（src/driver/build.cpp kModuleTable）登记：zipfile / shutil / tempfile /
urllib / urllib.error / urllib.request。

### 前端/运行时使能（最小修复，都是基线问题）

| 编号 | 改动 | 位置 | 回归 |
|---|---|---|---|
| E1 | open() fopen 失败按 errno 映射 FileNotFoundError/FileExistsError/PermissionError/NotADirectoryError/IsADirectoryError（此前一律 OSError "cannot open file"）；消息 "[Errno N] strerror: 'path'" | runtime/rt_file.c | 275 |
| E2 | os.close(fd) + os.unlink 别名（mkstemp 裸 fd 关闭/脚本自然写法） | rt_os.c、rt_internal.h、ir/ops.h（**OsClose 枚举尾部**）、typecheck/builtin_sigs.cpp、lower/import.cpp、backend/call_rt.cpp | 274/275 |
| E3 | os.makedirs 的 exist_ok **关键字**此前被降级静默丢弃（签名声明了但只装位置参）→ copytree(dirs_exist_ok=True) 恒 FileExistsError | src/lower/import_native.cpp | 273/275 |
| E4 | os.path.islink（lstat S_ISLNK；symlink 脚本刚需） | rt_os.c、ir/ops.h（**OsPathIslink 枚举尾部**）、typecheck/builtin_sigs.cpp、lower/import.cpp、backend/call_rt.cpp | 273/275 |
| F1 | **cmp+branch 融合排除 Bytes**（同 Str 既有排除）：bytes 切片 eq/ne 在循环条件位置被融合成指针 cmpq，内容相等的新切片 vs 字面量恒判不等。基线 0.327 已存在（主树 release 二进制复现），zipfile 循环 magic 校验抓出；顶层"碰巧对"是常量折叠把两侧折成同一驻留字面量 | src/backend/block.cpp can_fuse_branch（一行） | 275 |

RtFn 新增（OsClose/OsPathIslink）全部追加枚举末尾，不改既有编号（一批约定）；
backend/call_rt.cpp 只在既有 switch 组里加 case 标签（无新区域）。

## 三、API 覆盖

### zipfile
- ZipFile(file, mode="r"/"w"/"a"/"x", compression=ZIP_STORED/ZIP_DEFLATED,
  allowZip64, compresslevel)：namelist/infolist/getinfo/read/write/writestr/
  extract/extractall/testzip/open(name, "r"/"w")/setpassword/close/comment/
  filename、上下文管理
- ZipInfo：filename/date_time/compress_type/comment/extra/create_system/
  create_version/extract_version/flag_bits/volume/internal_attr/external_attr/
  header_offset/CRC/compress_size/file_size、is_dir()/from_file()
- 读：中央目录定位（EOCD 回扫 + comment 一致性 + SFX concat 偏移）、
  data-descriptor（flag bit 3）条目、UTF-8 名（flag 0x800）、CRC/尺寸校验
- 写：local header + central directory + EOCD 组装，UTF-8 名自动置 0x800，
  目录条目 0x10 标志；append 模式保留原条目原始压缩字节
- extract/extractall 路径净化（过滤 ""/"."/".." 部件，同 CPython _extract_member）
- is_zipfile；BadZipFile/LargeZipFile

### shutil
- copyfile（64KB 分块、覆盖写、SameFileError、follow_symlinks=False 复制链接）、
  copy/copy2（目录目标自动 join basename）、copymode/copystat（权限位 +
  atime/mtime 纳秒，utimensat）
- move（目录目标入目录、重名 Error、rename 失败回退 copy+delete、自移 Error）、
  rmtree（朴素递归、符号链接不跟随、ignore_errors）、copytree（嵌套、
  symlinks、ignore 可调用、dirs_exist_ok、错误聚合 Error）
- ignore_patterns（闭包实现，*patterns）、which（PATH/os.getenv、mode、path 参数）

### tempfile
- mkstemp（O_EXCL 0600 原语 + 8 字符 [a-z0-9_] 随机名重试，返回 (fd, 绝对路径)）、
  mkdtemp（os.mkdir FileExistsError 重试）、gettempdir（TMPDIR/TEMP/TMP +
  /tmp,/var/tmp,/usr/tmp 候选，access 探测，缓存）、gettempprefix、tempdir 全局
- NamedTemporaryFile(mode/buffering/encoding/suffix/prefix/dir/delete/errors)：
  包装类代理 file 全方法面（read/readline/readlines/write/writelines/seek/tell/
  truncate/flush/fileno/isatty/readable/writable/seekable/迭代/上下文），
  close 按 delete 删文件
- TemporaryDirectory（name/cleanup 幂等/__enter__ 返回 name/__exit__ 清理）

### urllib.request（libcurl）
- urlopen(url|Request, data=None, timeout=-1.0, *, headers=None, method=None)
  → Response；Request(url, data, headers, method)/get_method()
- Response：status/code/url/headers/closed、read(size)/readline/readlines/
  close/geturl/getcode/info、上下文管理、迭代
- HTTPMessage：get（大小写不敏感、default）/get_all/items/keys/__contains__/
  __getitem__/__str__（原始文本）
- GET/POST（data 自动 POST + Content-Type: application/x-www-form-urlencoded）、
  自定义头、HEAD/PUT/DELETE（method）、超时（CURLOPT_TIMEOUT_MS）、重定向
  （FOLLOWLOCATION MAXREDIRS=10）、HTTPS（系统 CA，默认验证）
- URLError(OSError，reason，str "<urlopen error ...>")；HTTPError(URLError，
  url/code/status/msg/headers + read()/geturl()/getcode()/info()/close()，
  str "HTTP Error N: reason")；超时按 CURLINFO_CONNECT_TIME 区分连接阶段
  （URLError）/传输阶段（TimeoutError），同 CPython 口径

## 四、CPython 差异（文档化边界，轻量标尺）

沿用 language.md/compatibility.md 既有边界（print(bytes) 静态 bytes 出 repr、
bytes/str 比较按字节、None/0 同形、unknown 域 bool 裸值等），本批新增：

1. **zipfile**：整读整写（内存 ∝ 归档大小）；Zip64 读 → NotImplementedError、
   写超 32 位容量/65535 条目 → LargeZipFile（allowZip64 收下不生效）；
   bzip2/lzma → NotImplementedError；加密条目读 → RuntimeError（同 CPython
   无密码口径），setpassword 收下不解密；fileobj 不做；write()/from_file 的
   date_time 固定 (1980,1,1,0,0,0)（无 localtime）、external_attr 固定
   0o644/0o755（无 stat 权限位）；文件名非 UTF-8 遗留编码用 replace（不做
   cp437）；compress_type/compresslevel 默认 **-1 哨兵**（ZIP_STORED==0 与
   None 同形，显式传 None 会落成 ZIP_STORED——None/0 边界的直接后果）；
   writestr 的 str 数据按 utf-8 直通（内部表示即 utf-8 字节，len_bytes 计长）。
2. **shutil**：copyfile 不检查特殊文件（fifo）；copystat 不复制 flags；
   chmod 的 follow_symlinks=False 不建模（Linux 无 lchmod）；move 的
   copy_function 参数不做（固定 copy2）；rmtree 朴素递归（非 fd-based 防竞态）、
   onerror/onexc 不做；copytree 错误消息是合并文本（不是 (src,dst,why) 列表
   repr）；which 不查 PATHEXT（POSIX）。
3. **tempfile**：mkstemp 的 fd 用 open(fd, mode) 包装（os.fdopen 替代）、
   裸 fd 用 os.close；NamedTemporaryFile 的 newline 只支持 None（open 降级
   字面量限制）；**无 __del__ 终结器**——忘记 close 会留文件/目录（CPython
   有弱引用终结），用 with 或显式 close/cleanup；tempdir 全局默认空串
   （CPython None）；TemporaryFile/SpooledTemporaryFile 不做；随机名不与
   CPython 逐字一致（随机源不同，差分只钉属性）。
4. **urllib.request**：整读整写（响应体一次收进内存）；urlopen 签名是子集
   （headers/method 是直接关键字，CPython 要经 Request——Request 也提供了）；
   data 收 bytes（str 按 utf-8 直通，CPython 要 bytes）；timeout 是整请求
   超时（CPython 逐 socket 操作）；**307/308 的 POST 跟随按 libcurl 口径**
   （CPython 对 POST 307 抛 HTTPError 不跟随）；默认 User-Agent 是
   libcurl/x.y（可 headers 覆盖）；重定向上限 10；urlretrieve/OpenerDirector/
   handler 体系/urllib.parse 不做。
5. **异常值 isinstance**：except 层级匹配正确（HTTPError 被 except URLError
   捕获），但 isinstance(e, URLError) 对自定义异常类实例返回 False（异常值
   不带类身份，基线边界）——测试用 except 口径断言。

## 五、桥接兜底清单（from python import，~25µs/次）

- zipfile：Zip64/加密解密/bzip2/lzma/流式/fileobj/真实 mtime 与权限位
- shutil：disk_usage/get_terminal_size/make_archive/unpack_archive/
  rmtree onerror/copy_function 自定义/chown 系
- tempfile：TemporaryFile/SpooledTemporaryFile/newline 变体/终结器语义
- urllib：urlretrieve/Request 完整面（add_header/redirect 链）/urllib.parse/
  urllib.response/代理/认证 handler/POST 307 严格口径
- pathlib 长尾（with_name/with_stem/suffixes/rglob/stat/rename/expanduser/
  symlink_to 等）/ io.StringIO / decimal / fractions 大整数 / sqlite3：整模块桥接

## 六、已知问题（差分 harness 新发现，基线已验证，非本批引入）

1. **嵌套 try/finally 之间抛异常 → "invalid temporary stack mark"**：
   外层 try 的 rc_mark 与内层 try 的 rc_mark 之间抛异常，展开时临时栈
   mark 校验失败。最小复现 /tmp/ehx.py（open(dir,"wb") 在两层 try 之间抛
   IsADirectoryError）；主树 release 二进制同复现。**绕行**：shutil.copyfile
   改单层 try/finally（fdst 异常路径由帧 RC 兜底）；模块代码避免嵌套
   try/finally 的 mark 夹层抛异常。根治属 EH/RC 工作流，移交。
2. **复杂帧里 cimport ptr 装填错**：unknown 重赋值的 bytes 局部在含
   dict 循环/多容器的复杂帧里发 12 参 cimport 调用，ptr 实参传成对象+8
   （len 字段）而非 +24（data）——响应体变成 07 00 00...。最小复现
   /tmp/curl_probe2.py；主树 release 同复现。**绕行**：urllib._perform
   独立小帧 + 全类型化形参。根治属 cimport 降级/装填工作流，移交。
3. **cast(dict[str,str], unknown) 赋值 IndexError**：直接标注赋值
   `hd: dict[str,str] = unknown` 正确（B1 洞填充），但经 cast() 调用结果
   赋值同类型会 IndexError（基线 SEGV）。**绕行**：模块内一律直接标注
   赋值/重赋值，不走 cast 到容器。
4. **类实例 __call__ 的 unknown 间接调用跳进 vtable 数据**（SEGV）：
   ignore_patterns 原设计 __call__ 类实例，改闭包（函数值间接调用是一批
   验证过的路径）。基线问题，移交。
5. 一批移交的 leaf×unknown RC 不对称仍在。**本批已在 pristine 1501040
   上独立复现确认**（stash 本批改动 + detach 重建：cast(bytes, f.read())
   最小用例漏 200210B/2obj，与本批构建逐字相同；注意主树 build/release
   二进制是 11-18 旧物位、且主树已进 0.328 p3next「基线 leak 根治」，
   不能拿它当 0.327 基线）。**绕行已落**：
   - 模块内 object 形参→容器/字符串一律**直接标注赋值/重赋值**
     （`b: bytes = data`），不走 cast() 调用中转（实测 cast 漏、标注不漏，
     j14 口径）；zipfile/shutil/tempfile/urllib 全部照此改写。
   - 测试侧二进制读写助手改文本口径（ASCII 内容），二进制面由模块 API
     （静态 bytes 返回）覆盖——272 的泄漏从 600987B 降到 256B。
   - **剩余小量泄漏（记录不硬修，属一批 _gzipio cast 口径 + 基线容器/bytes
     临时 RC 洞）**：270 9379B/29obj、271 3939B/22obj、272 256B/2obj、
     273 118B/3obj、276 712B/6obj、277 120B/1obj；**274/275 CLEAN**。
     主要位点：_gzipio.read_bin/write_bin 的 cast（一批代码，不动）、
     zipfile.close 的 bytes 拼接链、HTTPMessage 解析的 str 切片临时。
     根治属 unknown 表示层工作流（p3next 0.328 已在主树根治同类洞，
     集成后预期自然收敛）。

## 七、验证

- 新增 tests/run/270–277（8 个），每个先对拍 CPython 3.14 再钉
  `# --- expect ---`，O0/O1/O2 逐字节一致（271 含 2 行文档化 NI 边界，
  CPython 有 bz2/lzma 而 PA 没有，手工钉入）。
- 网络用例自起本地 http.server 子进程（DEVNULL + try/finally kill/wait，
  端口 18326/18327 分开，平行套件安全），fixture 内嵌（data-descriptor/
  zip64/加密标志 zip 由 CPython 离线生成 base64）。
- difftest（O0/O2 自差分）：**260 用例 0 不一致**。
- 套件普通 + ASan（PYADAMAS_TEST_JOBS=8）：均 **278 ran ok EXIT=0**。
- ASan detect_leaks=1 抽查新用例（--asan -O2）：274/275 CLEAN；
  270/271/272/273/276/277 剩余 118B–9.4KB 小量基线泄漏（§六.5 分类与
  绕行记录，pristine 1501040 同机制复现）。
- check_size：无新增超限（stdlib/*.py 与 glue 不在 src/ 1200 行规则内；
  src 改动落在有余量文件，ir/ops.h 为既有超限文件的最小增量）。

## 八、集成建议（与 p3next 0.328 并行）

- 本批 RtFn 追加（OsClose/OsPathIslink）在枚举**尾部**；p3next 若也追加，
  按落地顺序重排编号（rt_name 同步）。backend/call_rt.cpp 只加 case 标签
  进既有组，与 p3next 的 gen_any_tagged 拦截区域不重叠。
- backend/block.cpp 的 can_fuse_branch 一行修复（Bytes 排除）与 p3next
  改动面（codegen_tag/rt_any）正交。
- 模块接口格式未动（无 v12→v13）；新模块登记即生效，旧 modcache 按源哈希
  自然失效。
- 合入前重跑：difftest → 270–277 → 普通+ASan 套件 → leak 抽查 → check_size。
  若主树期间改过 lower_import_native/ir_ops.h 相邻代码，重点复核 makedirs
  关键字装配与枚举尾部编号。
