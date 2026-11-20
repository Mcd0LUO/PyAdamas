# 标准库自举第四批（stdlib-batch4）：tarfile

> 分支 **tar1**，基线 master 7924474（0.346）。这一批仍是**纯 PyAdamas
> 源码**：stdlib/tarfile.py + 模块表登记，不碰 lower / runtime / codegen，
> 不新增 C 胶水。CHANGELOG 预留 **0.347**。
>
> tarfile 在 [stdlib-batch3](stdlib-batch3.md) 里是「跳过：纯 py 侧没有
> open()/通用文件对象与流式 IO」。P4 块2 落地 **file 协议泛化 +
> io.StringIO/BytesIO**（0.343/0.346，见
> [lang-gaps-landing.md](lang-gaps-landing.md) §3b）之后，fileobj 协议面
> 可用，这条跳过理由不再成立——本批补上。

## 一、可行性复评

| 依赖面 | batch3 时的结论 | 本批实测 |
|---|---|---|
| 通用文件对象 | 没有 open()/fileobj | P4 块2：`file` 形参/字段收 FileLike（门控 `PYADAMAS_LANG_GAPS=1`），`f.read()/write()/seek()/tell()/flush()` 经 `pa_file_*` 分派到 BytesIO/用户类实例 |
| 内存流 + seek | 没有 | `io.BytesIO`（P4 块2）够 tar 的整读整写 |
| 二进制字节面 | 无 | bytes 类型（0.79+）与 `len_bytes`/`bytes([...])`/切片/拼接齐备 |
| stat/readlink/symlink/utime | os 原生面没有 | 复用既有 `stdlib/_shutilc.cpya` 胶水（shutil 已在用，**未新增 C**） |
| 数值字段 | 无 BigInt | int64 够 ustar（size 8^11 上限、mtime 8^11），base-256 只收 63 位内 |
| gzip 透明 | 有 gzip 模块（zlib 胶水） | 复用 `stdlib/gzip.py` 的 compress/decompress |

结论：**ustar 核心可纯 pa 实现**；pax/GNU 的**写**不做（保守跳过，见四），
**读**做成 ustar + GNU L/K + pax x/g 的最小面。

## 二、交付清单

| 文件 | 内容 |
|---|---|
| stdlib/tarfile.py | 常量/异常族；TarInfo（字段、类型谓词、get_info、frombuf/tobuf）；TarFile（r/w/a/x、:gz、fileobj 或路径、getmembers/getnames/getmember、extractfile、add/addfile/gettarinfo、extract/extractall、close、with、迭代）；open/is_tarfile；内部 `_oct/_itn/_nti/_stn/_nts/_calc_chksums/_posix_split_name/_parse_pax/_apply_pax` |
| src/driver_build.cpp | 模块表加 `{"tarfile", Python, "tarfile.py", "gzip,_gzipio,os,os.path"}`（+1 行） |
| tests/run/360 | TarInfo 面 + ustar 头字节（base64 钉死） |
| tests/run/361 | TarFile 内存面（fileobj=BytesIO）+ 路径面 + 归档字节对拍 |
| tests/run/362 | 磁盘面（add 递归/gettarinfo/extract/extractall/软链接/硬链接回退） |
| tests/run/363 | gzip 透明 + a/x 模式 + 错误面 |
| tests/run/364 | GNU/PAX 扩展头读取 + format 参数 + ustar prefix 长名整档 |

## 三、语义边界（轻量标尺）

### 头编解码
- **只写 ustar**：DEFAULT_FORMAT 与 CPython 同值（PAX_FORMAT=2），但写入永远
  是 ustar 头。**短 ASCII 名成员与 CPython 默认写出逐字节一致**（360/361
  用 base64 钉死）；长名按 ustar 的 `prefix(155) + name(100)` 在 `/` 处拆
  分（与 CPython `format=USTAR_FORMAT` 相同），拆不开抛
  `ValueError("name is too long")`；linkname > 100 字节抛
  `ValueError("linkname is too long")`。
- **数值字段 int64**：写侧八进制（`8**(digits-1)` 上限，超了
  `ValueError("overflow in number field")`；CPython 在 GNU 格式下转 base-256
  而不报错）；读侧收 base-256（0o200/0o377 前导），字段超 63 位抛
  `InvalidHeaderError`。
- **校验和**按 CPython 口径双算（无符号 + 有符号），任一命中即通过；
  生成用无符号，`"%06o\0"` 后保留第 8 字节空格。
- 目录名在头里补尾斜杠（`get_info` 口径），读回时剥掉。
- 老 V7 的「名字带尾斜杠的 AREGTYPE」归一成 DIRTYPE。

### 读侧扩展头
- GNU 长名/长链接（L/K）：**支持连续叠加**（GNU 会先写 K 再写 L 再写真头；
  逐层递归会丢前一层，实现用累积循环）。
- pax x/g：x 覆盖下一个成员（path/linkpath/size/mtime/uid/gid/uname/gname），
  g 存进 TarFile 并对后续全体成员生效；扩展头优先于全局头。
- **不做**：稀疏文件（S / GNU.sparse.*）、Solaris X 头、hdrcharset=BINARY
  的多编码回退（名字一律 UTF-8 + `replace`）。

### IO 模型
- **整读整写**（同 zipfile/GzipFile）：读一次性载入内存，写把块攒在内存、
  close 一次落盘。写入尺寸与 CPython 一致：两块全零尾 + 补齐到 RECORDSIZE
  （10240 的倍数）。
- 压缩只做 gzip（`stdlib/gzip.py`）：写出 mtime=0（可复现，但不与 CPython
  字节级对齐），**解压出的载荷与 CPython 逐字节相同**（363 钉了
  `gzip.decompress(gz) == 明文归档`）；bz2/xz/zst 抛
  `CompressionError("unknown compression type ...")`；`a:gz` 不支持。
- `r`/`r:*` 按魔数（`1f 8b`）嗅探；`r:` 显式明文；`r|/w|` 流式模式按
  普通 r/w 处理（不模拟 _Stream 分块语义）。
- `a`：读入既有归档 → 按解析出的成员末端截断 → 追加新成员。
- `x`：目标已存在抛 `FileExistsError`。
- `ignore_zeros`：`open(..., ignore_zeros=True)` 跳过零块继续读（整读整写
  口径下解析在构造期完成，事后给 `tar_file.ignore_zeros = True` 无效）；
  只做「跳过零块」，不做 CPython 的「跳过垃圾块」。

### 元数据与解压
- `gettarinfo` 用 `_shutilc.stat`（`dereference=False` 走 lstat）：**uid/gid
  恒 0、uname/gname 空**（CPython 填真实属主/名）；没有 inode/nlink，所以
  `add()` 不做硬链接合并，LNKTYPE 只在手写 TarInfo 时出现。
- `extract`/`extractall` 落盘 + `chmod(mode & 0o7777)` + `utime(mtime, mtime)`；
  **不做 chown**；`extractall` 的目录属性延迟到全部解完再补（子项解压会重置
  mtime）。目录用默认 mode 建、后面再 chmod（pa `os.mkdir` 只收路径）。
- **不实现 CPython 3.12+ 的 data_filter**：`filter=`/`numeric_owner=` 参数
  不存在（传了是 TypeError）。名字一律先净化（剥前导 `/`、丢 `''/'.'/..`
  段，同 zipfile），所以 `../x` 落到解压根下而不是抛
  `OutsideDestinationError`。
- LNKTYPE 解压：pa 没有 `os.link`，按 CPython「链接建不出来就复制内容」的
  回退口径落成普通文件。

### 错误面
- 类型同名同继承：TarError / ReadError / CompressionError / StreamError /
  ExtractError / HeaderError / InvalidHeaderError / EmptyHeaderError /
  TruncatedHeaderError / EOFHeaderError；消息合理但不逐字。
- `r`/`r:*` 首个头坏掉包成 `ReadError("file could not be opened successfully: …")`；
  `r:` 保留原始 `InvalidHeaderError`（CPython 同口径）。
- 关闭后操作 `OSError("TarFile is closed")`、模式不符
  `OSError("bad operation for mode 'w'")`（CPython 3.14 用 OSError，不是
  ValueError）。
- `getmember` 缺失抛 `KeyError("filename 'x' not found")`（同 CPython 文本）。
- `is_tarfile` 对不存在的路径返回 False（CPython 3.14 会漏出
  FileNotFoundError）。

### 门控与 pa 语言面限制（影响用法，不影响功能）
- **模块源码在 `PYADAMAS_LANG_GAPS=1` 开/关都能编译**：内部写入走
  `_write_member` 的字节面（不往 `file` 形参塞 BytesIO），所以 `add`/
  `addfile_bytes`/`addfile` 的 fileobj 面都不受门控影响。路径面
  （`open(路径)/add/extract/extractall/extractfile`）门控关即可用；
  **只有 `fileobj=BytesIO`/`addfile(ti, BytesIO(...))` 需要门控开**（那是
  P4 块2「file 协议泛化」的 assignable 放宽，属于语言门控本身）。
- `extractfile` 静态返回是 `BytesIO`（pa 的 Optional 窄化不支持方法调用），
  目录/设备成员返回**空 BytesIO**（CPython 返回 None）。
- `gettarinfo` 对 fifo/设备/socket 抛 `ValueError`（CPython 返回 None）。
- 导入类的**类方法经模块限定名**调用不支持（`tarfile.TarInfo.frombuf`），
  要 `from tarfile import TarInfo` 后用 `TarInfo.frombuf`；形参也别标成
  导入类（`f: TarFile` 会退化成不可调用），用局部变量或 object + cast。
- `bytes in tuple` 对切片产出的 bytes 判不等（既有运行时快路径问题），
  模块内一律用显式 `==` 比较类型常量。

## 四、跳过/负面结论

| 项 | 结论 | 依据 |
|---|---|---|
| pax/GNU 扩展头**写** | 跳过 | 只写 ustar；长名走 prefix 拆分。CPython 默认 PAX 在短名场景仍是 ustar 头，所以常见归档字节级一致 |
| 稀疏文件（S） | 跳过 | 需要 GNU sparse 结构表（0.0/0.1/1.0 三种），属长尾 |
| bz2/xz/zst | 跳过 | 系统库未绑定（zipfile 同口径抛 NotImplementedError，这里抛 CompressionError） |
| data_filter / filter= / numeric_owner | 跳过 | 需要把「成员 → 过滤后属性」的对象拷贝与 FilterError 族建模；且 pa 无法把函数当参数再调用 |
| chown / uid/gid 保留 | 跳过 | `os` 原生面无 chown，`_shutilc` 也没有 |
| `TarFile.list` / `debuglevel` 输出 | 跳过 | 需要 stat.filemode 与 localtime 格式化，属展示面 |
| `from python import tarfile` 桥接 | 保留 | 超大归档/流式/稀疏场景的退路 |

## 五、CPython 对拍

5 个用例都是**同一份 .py 在 CPython 3.14.4 与 pa 下跑、输出逐字节一致**
（expect 块由 CPython 输出钉死）。对拍覆盖：

| 用例 | 对拍点 |
|---|---|
| 360 | 头字节 base64（普通/目录/长名/软链/非 ASCII）、frombuf 字段、5 类头错误、溢出/超长 ValueError |
| 361 | 内存归档字节 base64、getnames/迭代/字段、extractfile（含软链跟随）、KeyError、关闭后 OSError、路径模式、is_tarfile、ignore_zeros |
| 362 | add 递归顺序、gettarinfo 字段（mode 含 S_IFREG 位）、extractall/extract/members 子集、软链链式解压、硬链回退、mtime 保留、绝对路径净化 |
| 363 | w:gz/r:gz 往返、r:* 嗅探、r: 显式、外部 gzip 包出来的 tar.gz 可读、gz 载荷 == 明文归档、a 追加、x 已存在、坏校验和/截断/非 tar/非 gz/未知压缩、空归档 |
| 364 | GNU_FORMAT/PAX_FORMAT 写入往返、手搓 L/K 长名长链、手搓 pax（含 size 覆盖）、ustar prefix 长名整档 |

额外的跨实现互操作（一次性手工验证，未进用例，避免测试间依赖）：

- pa 写 `w:gz` → **CPython `tarfile.open(..., "r:gz")` 读出**名字/内容/mtime/mode 全对。
- CPython 写 `w:gz`（默认 PAX，含子目录）→ **pa 读**名字/内容全对。
- CPython 写 GNU/PAX 的 186 字节长名 + 长链接 → **pa 读**名字、linkname、数据全对。
- `tar -tzf` 能列 pa 写的 `.tar.gz`。

## 六、已知交互（不属于本批改动）

**两个以上「except 里 raise 另一种异常」的站点在 `detect_leaks=1` 下各留一个
异常对象**（约 110B/站点，含 traceback 帧链）：exc 值与捕获的帧链构成 RC
环，只能靠周期 GC 回收，进程退出前没有收集机会就报泄漏。最小复现：

```python
import gzip
class MyErr(Exception): pass

def a() -> None:
    try:
        gzip.decompress(b"\x00\x01")     # gzip 里 raise BadGzipFile
    except OSError as e:
        raise MyErr("w")                   # 站点 1

def c3() -> None:
    try:
        gzip.decompress(b"\x00\x03")
    except OSError as e:
        raise MyErr("w3")                  # 站点 2（单站点时 0 泄漏）

a(); c3()
```

（门控开时 raise 会 `pa_trace_capture` 帧链；两个站点各 1 个对象，
单站点/单异常类不触发。）363 的 badsum（`InvalidHeaderError`→
`ReadError`）与 notgz（`BadGzipFile`→`ReadError`）两个站点命中，其余
360/361/362/364 `detect_leaks=1` 全 0。套件闸门用 `detect_leaks=0`
（`tests/run_tests.sh` 默认）不受影响；修这个要动运行期的异常/GC 释放链，
不在本标准库批次范围。

## 七、闸门（实测，HEAD 见仓库）

| 闸门 | 结果 |
|---|---|
| pa 编译（tarfile.py 在门控开/关两种模式都能编译） | ✅ 两种模式都编译；门控关时路径面实测可写/读/列/提取（fileobj 面才要门控） |
| 新用例 360-364 CPython 3.14 对拍 | **5/5 逐字节一致** |
| 全套件 normal（`PYADAMAS_TEST_JOBS=1`） | **EXIT=0，343 个 ran ok**，0 FAILED/DIFF |
| 全套件 ASan（`PYADAMAS_TEST_ASAN=1`，JOBS=1） | **EXIT=0，343 个 ran ok**，0 FAILED/DIFF |
| 现有程序 asm 不变 | **489/489 cmp 全同**（tests/run + tests/*.py + tests/modules + bench 全量，对基线 7924474 的独立 worktree 构建） |
| check_size | 无**新增**超限文件（5 个超限文件基线就有：codegen_regalloc 2581、driver_build 1604（本批 +1 行）、ir_ops.h 1398、lower_impl.h 1356、typecheck_types.h 1593）；stdlib/tarfile.py 1143 行 |
| 新用例 ASan detect_leaks=1 | 360/361/362/364 全 0；363 见第六节（两站点 RC 环） |
