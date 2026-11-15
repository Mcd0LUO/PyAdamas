# 交叉核对：M2 实际发射的 LSDA（只用本目录解码器 + 独立判断）

> 对象：tests/exc/z1_rt/ 下 z1_step2.S / z1_step3.S / z1_broken_lsda.S 实际发射的 5 段 LSDA 字节，
> 以及同目录手写汇编的 .eh_frame augmentation。
> 方法：只读 tests/exc/ 源码 → 用 gcc 自己重编到 tools/lsda_ref/out/crosscheck_m2/ →
> 用本目录自写解码器 decode_eh_lsda.py 逐字段解码（不采用 M2 的结论）→
> 再用 libstdc++ 的 __gxx_personality_v0 做运行时裁决。
> 复现：`bash tools/lsda_ref/crosscheck_m2.sh`（全部证据落在 out/crosscheck_m2/）。
> 环境：gcc/g++ 14.2.0-19、binutils 2.44、libstdc++.so.6、libgcc_s.so.1。

## 0. 结论（先给裁决）

1. **字节结构层面：ABI 合法。** 5 段里有 4 段（step2、step3 handler/cleanup/nopad）的表头字段全部符合 Itanium ABI；
   `ttype_encoding = 0xff` 时「TType 偏移字段整段消失」这一点它们**算对了**（0xff 之后紧跟的就是 call-site 编码字节 0x01）。
   action 记录 `(filter=0, next=0)` 本身也是合法的 ABI 记录。
2. **语义层面：这不是 ABI 的 handler，而是它们自定义的解释。** ABI 规定 action 记录的 `filter=0` 是 **cleanup**
   （phase 1 不会认领 handler）；catch-all 必须是 `filter>0` + TType 表里的 null type_info* 项（ttype_encoding 不能是 0xff）。
   它们的 personality「只看 call-site 的 action 下标是否 !=0，不读 action 表 filter」——这是私有约定。
3. **用真正的 __gxx_personality_v0 实测**：M2 的「handler」字节（action=1、记录 filter=0）在 gcc personality 下
   被当成 **cleanup**：phase 1 不抓，phase 2 只跑 landing pad（实测 landing pad 跑 1 次），异常继续传到外层
   `catch(int)`（实测 caught=7）。而把 action 记录换成 filter=1（仍在 ttype=0xff 下）时，phase 1 直接
   **SIGABRT**（backtrace：__gxx_personality_v0 → abort）。
4. **0xfe 负向用例不是「合法但含义不同」，而是结构非法。** 0xfe != 0xff，会让 TType 偏移字段「重新出现」：
   紧随其后的 0x01 被吃成 TType offset，于是 call-site 编码被读成 0x04（=DW_EH_PE_udata8），
   call-site 表随后越界（表长 8 字节，但每项至少要 3×8 字节）。它测的是自家校验器能报 ENCODING 错，
   不是「ABI 对错误编码的处理」——libstdc++ 在这里会越界读而不是干净报错。
5. **互通会有障碍。** ttype=0xff 方案要接真实 C++ 异常（cimport），至少缺：类型表/类型匹配、
   filter 语义、`__cxa_exception`/`_UA_HANDLER_FRAME` 协议。另有独立隐患：cleanup 帧的 landing pad 里
   `call _Unwind_Resume` 的 PC 没有被 call-site 表覆盖（实测在 gcc personality 下会 found_terminate），
   GCC 会补一段「lp=0」的保护区——这跟 ttype 编码无关，但同样属于「与 gcc 不同构」的隐患。

---

## 1. 我重编得到的真实字节（与任务消息引用的值有出入）

用 `gcc -c -Wa,-L` 重编 tests/exc/z1_rt/ 的源码（不改动原文件），`readelf -x .gcc_except_table`：

| 夹具 | 任务消息引用的字节 | 我重编的实际字节 | 说明 |
|---|---|---|---|
| z1_step2 | `ff ff 01 04 08 0e 1d 01 00 00 00 00` | `ff ff 01 04 08 17 26 01 00 00 00 00` | 结构相同；len=0x17、lp=0x26（消息里的 0x0e/0x1d 对不上当前源码，可能引用了旧版本）。结论不受影响。 |
| z1_step3 handler | `ff ff 01 04 08 09 18 01 00 00 00 00` | 同左 | 12 字节 = LSDA 10 字节 + `.align 4` 的 2 字节 padding。 |
| z1_step3 cleanup | `ff ff 01 04 08 09 18 00 00 00 00`（11 字节） | `ff ff 01 04 08 09 18 00`（8 字节） | 引用的 11 字节里，后 3 个 0 不属于这段 LSDA（是 padding/后续数据）。 |
| z1_step3 nopad | `ff ff 01 04 08 09 00 00` | 同左（8 字节） | 一致。 |
| z1_broken_lsda | `ff fe 01 04 08 09 18 01 00 00 00 00` | 同左（12 字节） | 一致。 |

---

## 2. 逐字段解码（本目录 decode_eh_lsda.py）

### 2.1 z1_step2（实际字节）——结构合法，语义是 cleanup

```text
偏移 字节      字段                         解释
0x00 ff        lpstart_encoding             DW_EH_PE_omit → LPStart = FDE region start
0x01 ff        ttype_encoding               DW_EH_PE_omit → 没有类型表；TType 偏移字段不存在
0x02 01        call_site_encoding           DW_EH_PE_uleb128
0x03 04        call_site_table_length       4 字节（1 条）
0x04-0x07      08 17 26 01                  start=8, len=0x17, landing_pad=0x26, action=0x1
0x08-0x09      00 00                        action 记录 (filter=0, next=0)
0x0a-0x0b      00 00                        .align 4 padding（不属于任何表）
```

- 与源码符号对得上：start=8（.LEHB_step2 相对函数起点）、len=0x17（三条 call 的连续保护区）、
  landing_pad=0x26（z1_step2_lp）、action=1。
- ABI 判定：**header 合法；action 记录 (filter=0,next=0) 在 ABI 里是「cleanup」**，不是 catch-all。
- 逻辑长度 0xa（10 字节），尾部 2 字节是 `.align 4` padding。

### 2.2 z1_step3 handler / cleanup / nopad

```text
handler: ff ff 01 04 | 08 09 18 01 | 00 00 | 00 00
         LPStart omit | TType omit | cs enc=uleb | cs len=4
         start=8 len=9 lp=0x18 action=1 | action(filter=0,next=0) | pad 2B
cleanup: ff ff 01 04 | 08 09 18 00
         start=8 len=9 lp=0x18 action=0        （action=0 = 没有 action 记录，纯 cleanup）
nopad:   ff ff 01 04 | 08 09 00 00
         start=8 len=9 lp=0     action=0        （lp=0 = 该区间没有 landing pad）
```

- 三段都：`ttype_encoding=0xff`，TType 偏移字段确实不存在，紧跟的是 call-site 编码 0x01，**这一点它们做对了**。
- ABI 判定：handler 那段在 ABI 里也是 **cleanup**（filter=0）；cleanup 那段本来就合法（action=0）；
  nopad 那段合法（lp=0, action=0 = 无 landing pad，展开器直接穿过）。
- 「handler」这个称呼只在它们自己的 personality 语义下成立（action 下标 != 0 → HANDLER_FOUND）。

### 2.3 z1_broken_lsda（0xfe）——结构非法

```text
偏移 字节 字段                       我的解码结果
0x00 ff   lpstart_encoding          omit
0x01 fe   ttype_encoding            0xfe = indirect + 0x0e（未定义的 format 低 4 位）→ 非法编码
0x02 01   ttype_table_offset        ← 因为 0xfe != 0xff，这个字段“重新出现”，把原 0x01 吃成偏移=1
0x03 04   call_site_encoding        被读成 0x04 = DW_EH_PE_udata8（字段宽度 8 字节）
0x04 08   call_site_table_length    8 字节，但 udata8 下每项至少 3×8 字节 + action → 越界
```

判定：**结构非法**（不是「合法但含义不同」）。0xff→0xfe 不只是一个编码值变化：它改变了后续字段的**存在性/对齐**，
把整个 header 解析错位。它们的负向用例断言自家 `PA_LSDA_ERR_ENCODING`，这只证明自家校验器工作；
libstdc++ 遇到同字节会按 0x04=udata8 解析并越界读，不会给出干净的「非法编码」错误。

---

## 3. 关键裁决：这个组合在 ABI 上的含义 + libstdc++ 实测

### 3.1 ABI / libsupc++ 源码（gcc master，libstdc++-v3/libsupc++/eh_personality.cc）

- 表头解析（parse_lsda_header）：
  `if (info->ttype_encoding != DW_EH_PE_omit) { ... read_uleb128 ...; info->TType = p + tmp; } else info->TType = 0;`
  → 0xff 时确实不读 TType 偏移字段（M2 这一点与 ABI 一致）。
- action 记录解释（personality 扫描 action 表）：
  `if (ar_filter == 0) { /* Zero filter values are cleanups. */ saw_cleanup = true; }`
  `else if (ar_filter > 0) { /* Positive filter values are handlers. */ catch_type = get_ttype_entry(&info, ar_filter); ...}`
  `// Null catch type is a catch-all handler`
  → **filter=0 是 cleanup，不是 handler**；catch-all 必须是 filter>0 且 TType 项解出 null。
- call-site 的 action 下标为 0 时：`// If ip is present, has a non-null landing pad, and a null action table offset, then there are only cleanups present. ... found_type = found_cleanup;`
  即 action 下标 ≠0 只说明「有 action 记录」，记录内容（filter）才决定 handler/cleanup。

### 3.2 ttype_encoding=0xff 时 filter>0 会怎样

- get_ttype_entry：`i *= size_of_encoded_value(info->ttype_encoding); ... read_encoded_value_with_base(info->ttype_encoding, info->ttype_base, info->TType - i, &ptr);`
- libgcc/unwind-pe.h：`if (encoding == DW_EH_PE_omit) return 0;  /* size_of_encoded_value */` →
  ttype=0xff 时 size=0、i=0；`info->TType` 为 0；
  随后 read_encoded_value_with_base(0xff, ...) 的 `switch (encoding & 0x0f)` 落到 `default: __gxx_abort();`
  → **filter≥1 + ttype=0xff = 直接 abort**（不是「找不到类型」，也不是 catch-all）。

### 3.3 运行时实测（本目录 crosscheck_m2/ 复刻夹具，用 __gxx_personality_v0）

复刻夹具把 LSDA 做成 M2 的字节形态（ttype=0xff、action=1、action 记录 (filter,0)），
personality 用 gcc 的 `0x9b + DW.ref.__gxx_personality_v0`，外层是真正的 C++ `catch (int)`：

| 变体 | 实测结果 | 含义 |
|---|---|---|
| action 记录 filter=0（M2 的「handler」字节） | 退出码 0；stderr `[driver] caught=7 m2_lp_runs=1` | gcc personality 把这一帧当 **cleanup**：landing pad 在 phase 2 跑 1 次，异常继续送给外层 catch。**不是 handler**。 |
| action 记录 filter=1 | 退出码 134（SIGABRT）；gdb backtrace：`__cxa_throw → _Unwind_RaiseException → __gxx_personality_v0 → abort` | phase 1 里 get_ttype_entry 走 __gxx_abort。 |

附带实测到的一个坑（与 ttype 编码无关，但同属「gcc 不会这么发」）：
cleanup 帧的 landing pad 里 `call _Unwind_Resume` 必须被一条 landing pad=0 的 call-site 记录覆盖，
否则 `_Unwind_Resume` 阶段 personality 会拿到「表外 PC」→ found_terminate → std::terminate。
我最初复刻时没有第二段保护区，实测就是 `terminate called after throwing an instance of 'int'`
（gdb: _Unwind_Resume → __gxx_personality_v0 → __cxa_call_terminate）；补上第二段后行为才符合预期。
M2 的 z1_step3 cleanup 夹具只有一条 call-site 记录，在它自家 personality 下没问题
（它把未命中当 CONTINUE），但换成 gcc personality/unwinder 会 terminate。GCC 对这种帧会发第二段保护区。

更重要的是：M2 的 handler landing pad（z1_step2_lp / z1_step3_handler_lp）是**正常 ret 返回**，不是 _Unwind_Resume。
在 gcc personality 下这些帧被当成 cleanup，而「cleanup landing pad 正常返回」是非法路径（展开被中途截断、异常对象泄漏、后续行为未定义）。
所以「换成 gcc personality 也能跑」不成立——只有 cleanup 形态的帧才勉强能跑通。

---

## 4. .eh_frame FDE augmentation：.cfi_lsda 0x1b + .cfi_personality 0x1b, pa_personality

我重编的 .o（readelf -x .eh_frame / readelf -wf）：

```text
CIE: Augmentation "zPLR"
     augmentation data = 1b 00 00 00 00 1b 1b
       [0]=1b personality encoding = DW_EH_PE_pcrel|DW_EH_PE_sdata4（直接指针）
       [1..4] personality 指针字段（.o 里 RELA 占位 0；reloc @0x13 → pa_personality）
       [5]=1b LSDA 指针 encoding = pcrel|sdata4
       [6]=1b FDE 初始地址 encoding = pcrel|sdata4
FDE: augmentation data length=4；LSDA 指针字段 reloc → .gcc_except_table+{0, 0xc, 0x14}
```

与 gcc 的差别只有 personality 编码这一个字段：

```text
gcc   : 9b 00 00 00 00 1b 1b   → 0x9b = indirect|pcrel|sdata4，指向 DW.ref 指针槽，槽里才是函数地址
M2    : 1b 00 00 00 00 1b 1b   → 0x1b = pcrel|sdata4，直接指向 pa_personality 函数本体
LSDA 指针编码 / FDE 编码两者都是 0x1b，没有差别。
```

原因是 gcc 的 personality 在 libstdc++.so 里（链接期无法用 PC-relative 定址到共享库符号），必须走 DW.ref 间接层；
M2 的 pa_personality 是 `rt_personality.o` **静态链进同一个可执行文件**的，所以 0x1b 直接引用可以工作。

PIE / 非 PIE 差别（我按它们 run.sh 的方式建了两套，链接后的 CIE augmentation data）：

```text
-no-pie : 1b 3d dd ff ff 1b 1b
-pie    : 1b ad de ff ff 1b 1b
```

**编码字节完全相同**（0x1b, 0x1b, 0x1b），只有中间 4 字节 pcrel 指针值不同——这是预期的。
限制：一旦把 personality 放进共享库（或任何链接期无法定址的符号），0x1b 直接引用会让
PIE/-shared 链接被 ld 拒绝（R_X86_64_PC32 against shared symbol；见 FINDINGS.md 第 5.2 节的实测报错）。
也就是说：M2 这套 0x1b 写法现在能过，前提是 runtime personality 与代码静态链进同一个可执行文件；
如果将来 runtime 变成 .so，.cfi_personality 必须改回 gcc 的 0x9b + DW.ref。

---

## 5. 总结论（能/不能算 ABI 合法）

- **表结构：合法**（header、0xff 省略 TType offset、call-site 表、(0,0) action 记录都符合 ABI）。
- **handler 语义：自定义扩展，不是 ABI 语义。** 「call-site action 下标 != 0 就是 catch-all handler」
  是 pa_personality 的私有约定；ABI 只认 action 记录里的 filter：filter=0 → cleanup，filter>0 → catch（查 TType），
  filter<0 → 异常规格。用 gcc personality 解析同一份字节，这些「handler」帧全部退化为 cleanup。
- 因此不能描述成「ABI 合法、只是和 gcc 不同构」；更准确的说法是：
  **字节容器是 ABI 合法的，但「handler」这个含义是本项目自定义的扩展；与 gcc/libstdc++ 的异常语义不互通。**
- **互通风险（cimport 调 C++ 代码）**：
  1) 我们的帧作为 C++ 异常展开路径时，gcc personality 不会把我们的 catch-all 当 handler（只会当 cleanup），
     所以「C++ 代码抛异常、被我们 cimport 的边界接住」在没有 ABI 兼容 LSDA 之前不成立；
  2) 我们处理 C++ 异常时没有类型表/filter，无法做 typed catch 匹配、也无法与 `__cxa_exception` 的
     handlerCount/rethrow/`_UA_HANDLER_FRAME` 协议对齐；
  3) 若将来切到 libstdc++ personality（哪怕只是测试对照），现有 5 段夹具里 handler 的语义全部会改变，
     cleanup 夹具还会因 landing pad 的 resume call 未被 call-site 覆盖而 terminate。
- 建议：Z4 之前若要保留 cimport 互通可能，把发射格式升级为 GCC 同款
  `ttype_encoding=0x9b + TType 表（catch-all 用 4 字节 0 项）+ action filter>0`，
  personality 增加 filter/TType 解析与类型匹配；同时 cleanup 帧补「landing pad 内 call」的 call-site 记录。
  若短期确定「只做自有异常、不接 C++」，当前方案可以继续用，但应在文档里明确标为**私有 ABI 扩展**，
  不能说成「Itanium ABI 兼容」。

---

## 6. 复现

```bash
bash tools/lsda_ref/crosscheck_m2.sh     # 重编 + dump + 逐字段解码 + 运行时裁决
```

关键证据文件（都在 out/crosscheck_m2/ 下）：

- `fixtures/z1_step2|z1_step3|z1_broken_lsda.{readelf_x_except_table,readelf_wf,readelf_r}.txt`：原始字节/重定位
- `fixtures/z1_step*.decode.txt`：本目录解码器的逐字段解码（含结构非法判定）
- `build_pie/readelf_wf.txt`、`build_nopie/readelf_wf.txt`：PIE/非 PIE 链接后的 CIE augmentation data
- `m2_cleanup_semantics.err`、`m2_filter1.err`：libstdc++ personality 下的运行时结果
- `refs/eh_personality.cc`、`refs/unwind-pe.h`：本报告引用的源码（gcc master）
