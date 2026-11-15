# M1：LSDA 字节级 ground truth（gcc 实测）

> 任务：把 docs/zero-cost-exceptions.md 37-124 行里「从 GCC 产物反推」的 LSDA 布局，用本机 gcc 实测钉死，
> 并给出 M2/Z1 落地时可以直接照抄的最小 catch-all LSDA。
>
> 环境：Debian WSL，g++ (Debian 14.2.0-19) 14.2.0，GNU readelf/objdump/objcopy (GNU Binutils for Debian) 2.44，
> Python 3.13.5。本机默认生成 PIE（Debian gcc 默认 -pie）。
>
> 复现：bash tools/lsda_ref/gen.sh（重建全部 dump + 逐字段解码）；bash tools/lsda_ref/check.sh（硬断言，退出码 0）。
> 所有原始 dump 在 tools/lsda_ref/out/ 下。

---

## 0. 给 M2/Z1 的 TL;DR

### 0.1 我们要发射的 catch-all LSDA（GCC 14 同款，20 字节）

```text
ff 9b 11 01 08 04 05 13 01 1b 05 00 00 01 00 00 00 00 00 00
```

逐字节含义（假定函数起点 = FDE region start，被保护 call 在函数 +0x4、长度 5，
landing pad 在函数 +0x13，landing pad 里再有一条可能抛出的 call 在 +0x1b 长度 5）：

```text
偏移  字节  字段
0x00  ff    lpstart_encoding   = DW_EH_PE_omit（LPStart 取 FDE 的 region start，即函数起点）
0x01  9b    ttype_encoding     = DW_EH_PE_indirect|DW_EH_PE_pcrel|DW_EH_PE_sdata4
0x02  11    ttype_table_offset = uleb128，0x11：从本字段之后到 TType 表“末尾”的偏移（表反向索引）
0x03  01    call_site_encoding = DW_EH_PE_uleb128
0x04  08    call_site_table_length = uleb128，8 字节 = 2 条 × 4 字段
0x05  04    call-site[1].start       = 0x4   （= .LEHB0 - 函数起点）
0x06  05    call-site[1].length      = 0x5
0x07  13    call-site[1].landing_pad = 0x13  （相对 LPStart）
0x08  01    call-site[1].action      = 1     （1 起下标；0 = 无 action，只做 cleanup）
0x09  1b    call-site[2].start       = 0x1b  （landing pad 内可能抛出的那条 call）
0x0a  05    call-site[2].length      = 0x5
0x0b  00    call-site[2].landing_pad = 0     （0 = 该区间无 landing pad）
0x0c  00    call-site[2].action      = 0
0x0d  01    action[0].filter = 1（SLEB128；1 起下标，指向 TType[1]）
0x0e  00    action[0].next   = 0（SLEB128，链尾）
0x0f  00    .align 4 的对齐 padding（不属于任何表，解析时别当 action 记录）
0x10-0x13 00 00 00 00   TType[1]：4 字节 0，且不要重定位 => null type_info* => catch(...)
```

函数级指令：

```text
.cfi_startproc
.cfi_personality 0x9b, DW.ref.<personality 符号>   # 0x9b 后面必须是 8 字节指针槽，不是函数本体
.cfi_lsda 0x1b, .LLSDA0                            # 0x1b 指向本地 .LLSDA 标签，实测可用
```

DW.ref.<personality> 槽（gcc 自己的写法，手写时要自己发）：

```text
.section .data.rel.local,"aw",@progbits
.align 8
DW.ref.pa_personality:
    .quad pa_personality
```

已经逐字节验证：mini.S 用上面这套写出来的 .gcc_except_table（20 字节）和 .eh_frame（64 字节）
与 g++ -O1 编 probe_catch_all.cpp 的产物完全相同；check.sh 里用 cmp 语义断言（见第 5 节）。

### 0.2 三条最关键的修正

1. catch-all 的 TType 表不能省。GCC 的 catch-all 仍然 ttype_encoding=0x9b + ttype_table_offset=0x11，
   TType 表里有且只有 1 个 4 字节零项（没有重定位），personality 解码得到 null type_info*，这就是 catch(...)。
   文档 101-104 行「类型表可以退化成空」是错的（会让 catch-all 变成 cleanup，永远抓不到异常）。
2. 0x9b / 0x1b 的字节没错，但语义名字错了：不是 udata4，是 sdata4（0x0b）。
   DW_EH_PE_udata4 是 0x03；0x0f 并不是合法的 udata4 组合（任务描述里这一点也要订正）。
3. personality 的返回值不只两个：除了 _URC_HANDLER_FOUND / _URC_CONTINUE_UNWIND，
   phase 2 必须返回 _URC_INSTALL_CONTEXT 才能真的跳到 landing pad（文档 100-104 行的简化说法漏了它）。

---

## 1. 「文档声称 → 实测 → 判定」总表

文档 = docs/zero-cost-exceptions.md。实测 = 本目录 gen.sh 的产物（out/ 下 dump + 解码）。

| # | 文档位置 / 声称 | 实测（本机） | 判定 |
|---|---|---|---|
| 1 | 54-55 行：.cfi_personality 0x9b, DW.ref.__gxx_personality_v0；.cfi_lsda 0x1b, .LLSDA0 | g++ -O1 -S 的三个用例逐字相同（out/*.O1.s） | 对 |
| 2 | 58 行：0x9b = DW_EH_PE_indirect\|pcrel\|udata4 | 字节确实是 0x9b；但 0x9b=0x80\|0x10\|0x0b，0x0b=DW_EH_PE_sdata4（udata4=0x03） | 字节对，名字错（应 sdata4） |
| 3 | 59 行：0x1b = pcrel\|udata4 | 0x1b=0x10\|0x0b = pcrel\|sdata4；readelf -wf 的 CIE augmentation data 就是 9b ... 1b 1b | 字节对，名字错（应 sdata4） |
| 4 | 59 行相关：任务描述称「0x0f 才是 udata4」 | DWARF 4 §7.7.1：udata4=0x03、sdata4=0x0b、0x0f 不是合法组合 | 任务描述也需订正；结论方向（0x1b/0x9b 是 sdata4）对 |
| 5 | 74-84 行：LSDA header「3 个 .byte + 2 个 uleb128」 | 有 TType 表的用例（catch int / catch all）正是 3 字节 + 2 个 uleb128；cleanup 用例 ttype_encoding=0xff，TType 偏移字段整段消失 → 2 字节 + 1 个 uleb128 | 需修正（分情形） |
| 6 | 78 行：LPStart 0xff = omit（用函数起点） | 三个用例首字节都是 ff；libsupc++ parse_lsda_header 里 omit 时 LPStart = info->Start = _Unwind_GetRegionStart()；call-site 的 start 实测 = .LEHB0 - .LFB0（catch-all 是 0x4，= 0x4） | 对 |
| 7 | 82-83 行：call-site 表编码 0x1、长度 uleb128 | 第 3 字节 01（uleb128），第 4 字节 08（8 = 2 条 × 4 字段） | 对 |
| 8 | 85-92 行：每项 start/len/landing-pad/action | 每项 4 字段；start/len 相对函数起点、landing pad 相对 LPStart；action 字段始终是 uleb128，与 call_site_encoding 无关 | 对，需补一句 |
| 9 | 88 行注释：action 下标（0 = 只做清理） | action=1 表示 action 表下标 1；0 = 没有 action 记录（personality 源码：action_record == 0 && landing_pad != 0 → found_cleanup） | 对，需说清 0 的语义 |
| 10 | 93-95 行：.byte 0x1「action 表：1 条」，.byte 0 | 这是 1 条 action 记录 (filter=1, next=0)，不是「条数」计数；两个字段都是 SLEB128 有符号 | 需修正（表述） |
| 11 | 101-104 行：catch-all 时「类型表可以退化成空」，action 表一条 0（cleanup） | catch-all 实测：ttype_encoding=0x9b，offset=0x11，TType 表 1 个 4 字节 0 项（无重定位）；action filter=1 指向它；filter=0 才是 cleanup，catch-all 用 filter>0 + null type_info | 错（最重要） |
| 12 | 100-104 行：personality 只需处理 HANDLER_FOUND / CONTINUE_UNWIND 两个返回码 | 要真正跳 landing pad 必须返回 _URC_INSTALL_CONTEXT；libsupc++ 还会返回 _URC_FATAL_PHASE1_ERROR 等 | 需修正（漏 INSTALL_CONTEXT） |
| 13 | 96-98 行：.align 4; .long DW.ref._ZTIi-. | catch(int) 完全一致；catch-all 是 .long 0（TType 表仍在）；cleanup 没有 TType 表项，也没有 TType 偏移字段 | 需分情形修正 |
| 14 | 既没写 TType 表偏移的方向 | 偏移指向 TType 表末尾，第 i 项在 ttype_end - i*4（反向索引）；libsupc++/libc++abi 都这样实现 | 需补充 |
| 15 | 既没写 action 表后的对齐 padding | catch-all/catch-int 的 action 记录在 0x0d-0x0e，0x0f 是 .align 4 的 padding，不属于任何表 | 需补充（解析坑） |
| 16 | 114-120 行：personality 契约（phase1/phase2） | 与 libsupc++ __gxx_personality_v0 一致；补充：PC 完全不在 call-site 表内时 gcc personality 会 found_terminate（std::terminate），见第 7 节 | 基本对，需补边界 |

---

## 2. .eh_frame 实测（三个参考用例的 CIE 完全相同）

readelf -wf out/probe_catch_all.O1.o（= --debug-dump=frames）的关键行：

```text
00000000 ... CIE
  Version:               1
  Augmentation:          "zPLR"
  Code alignment factor: 1
  Data alignment factor: -8
  Return address column: 16
  Augmentation data:     9b 00 00 00 00 1b 1b
...
00000020 ... FDE cie=00000000 pc=0000000000000000..0000000000000027
  Augmentation data:     00 00 00 00
```

readelf -x .eh_frame 原始字节（.o 里重定位尚未应用）：

```text
0x00  1c 00 00 00   length = 0x1c
0x04  00 00 00 00   CIE id = 0
0x08  01            version = 1
0x09  "zPLR\0"       augmentation 字符串
0x0e  01            code alignment = 1
0x0f  78            data alignment = -8（SLEB128 0x78）
0x10  10            return address register = 16
0x11  07            augmentation data 长度 = 7
0x12  9b            personality encoding = indirect|pcrel|sdata4
0x13  00 00 00 00   personality 指针字段（RELA 占位 0；reloc 在 0x13）
0x17  1b            LSDA 指针 encoding = pcrel|sdata4
0x18  1b            FDE 初始地址 encoding = pcrel|sdata4
0x20  1c 00 00 00   FDE length
0x24  24 00 00 00   CIE 指针（0x24 - 0x24 = 0 → CIE@0x00）
0x28  00 00 00 00   函数起点字段（reloc 在 0x28，目标 .text+0）
0x2c  27 00 00 00   range（catch-all 是 0x27；catch int 0x32；cleanup 0x36）
0x30  04            FDE augmentation data 长度 = 4
0x31  cf ff ff ff   LSDA 指针字段（RELA 占位；reloc 在 0x31，目标 .gcc_except_table+0）
```

out/probe_catch_all.readelf_r.txt 里对应的 3 条 .rela.eh_frame：

```text
0x13  R_X86_64_PC32  DW.ref.__gxx_personality_v0 + 0   # CIE：personality（间接槽）
0x28  R_X86_64_PC32  .text + 0                          # FDE：函数起点
0x31  R_X86_64_PC32  .gcc_except_table + 0              # FDE：LSDA
```

结论：文档对两个指令的写法没记错，但 CIE 的 augmentation data 是 7 字节
（P 带 1 字节编码 + 4 字节指针；L 只带 1 字节编码；R 只带 1 字节编码），
链接前指针字段在 .o 里是 RELA 占位 0，必须看 .rela.eh_frame 才能知道指向谁。

---

## 3. 三个用例的 LSDA 逐字节解码

原始 dump：out/probe_catch_int.objdump_s_except_table.txt / .readelf_x_except_table.txt / .decode.txt。

### 3.1 catch(int)：20 字节

readelf -x .gcc_except_table out/probe_catch_int.O1.o：

```text
0x00000000 ff9b1101 08040513 011c0500 00010000
0x00000010 00000000
```

| 偏移 | 字段 | 原始字节 | 值 / 解释 |
|---|---|---|---|
| 0x00 | lpstart_encoding | ff | DW_EH_PE_omit → LPStart = FDE region start = 函数起点 0 |
| 0x01 | ttype_encoding | 9b | indirect\|pcrel\|sdata4（TType 表存在） |
| 0x02 | ttype_table_offset | 11 | uleb128 = 17；0x03 + 0x11 = 0x14 = 20 = 节尾 = TType 表末尾 |
| 0x03 | call_site_encoding | 01 | uleb128 |
| 0x04 | call_site_table_length | 08 | 8 字节 |
| 0x05 | call-site[1].start | 04 | 0x4（= .LEHB0 0x4 - .LFB0 0） |
| 0x06 | call-site[1].length | 05 | 0x5（= .LEHE0 0x9 - .LEHB0 0x4，就是那条 call） |
| 0x07 | call-site[1].landing_pad | 13 | 0x13（= .L5，相对 LPStart） |
| 0x08 | call-site[1].action | 01 | 1 起下标 → action 表第 0 项 |
| 0x09 | call-site[2].start | 1c | 0x1c（= .LEHB1，landing pad 里 call _Unwind_Resume 区段） |
| 0x0a | call-site[2].length | 05 | 0x5 |
| 0x0b | call-site[2].landing_pad | 00 | 0 = 该区间无 landing pad |
| 0x0c | call-site[2].action | 00 | 0 = 无 action（展开器直接继续） |
| 0x0d | action[0].filter | 01 | SLEB128 filter=1 → catch，TType[1] |
| 0x0e | action[0].next | 00 | SLEB128 next=0（链尾） |
| 0x0f | （padding） | 00 | .align 4 的对齐字节，不属于任何表 |
| 0x10-0x13 | TType[1] | 00 00 00 00 | RELA 占位；.rela.gcc_except_table 在 0x10 有 R_X86_64_PC32 DW.ref._ZTIi |

TType 反查链：action.filter=1 → get_ttype_entry(info, 1) → TType - 1*4 = 节+0x10 →
pcrel 到 DW.ref._ZTIi 槽（.data.rel.local 里 .quad _ZTIi）→ _ZTIi（int 的 type_info）。
链接后 out/probe_catch_int.exe 的同一位置变成真实 pcrel 值（本机为 ac 1e 00 00）。

### 3.2 catch(...)：20 字节（我们最关心的形态）

readelf -x .gcc_except_table out/probe_catch_all.O1.o：

```text
0x00000000 ff9b1101 08040513 011b0500 00010000
0x00000010 00000000
```

与 catch(int) 只差两处：

| 偏移 | 字段 | 原始字节 | 值 / 解释 |
|---|---|---|---|
| 0x09 | call-site[2].start | 1b | 0x1b（= .LEHB1，landing pad 里 call __cxa_end_catch） |
| 0x10-0x13 | TType[1] | 00 00 00 00 | 没有任何重定位 → 解码值 0 → null type_info* → catch-all |

关键点（第 8 节有源码依据）：

- ttype_encoding 仍然是 0x9b，不是 0xff；ttype_table_offset=0x11，不是 0。
- TType 表没有消失，而是用「4 字节 0 + 无重定位」表示 null type_info*；
  action.filter=1 指向它，于是 personality 得到 catch_type == null → 判定 catch-all。
- 为什么是 4 字节 0 而不是 pcrel 0：因为 0x9b 带 indirect，正常 TType 项是 pcrel 偏移；
  libgcc 的 read_encoded_value_with_base 对 result == 0 有特判（直接返回 0，不做 pcrel/indirect），
  所以「编码值 0」= 空指针是合法的（详见第 8 节参考源码）。

### 3.3 cleanup-only：12 字节

readelf -x .gcc_except_table out/probe_cleanup.O1.o：

```text
0x00000000 ffff0108 05050c00 1c050000
```

| 偏移 | 字段 | 原始字节 | 值 / 解释 |
|---|---|---|---|
| 0x00 | lpstart_encoding | ff | omit |
| 0x01 | ttype_encoding | ff | omit：TType 偏移字段整段消失，TType 表不存在 |
| 0x02 | call_site_encoding | 01 | uleb128 |
| 0x03 | call_site_table_length | 08 | 8 字节 |
| 0x04 | call-site[1].start | 05 | 0x5（.LEHB0） |
| 0x05 | call-site[1].length | 05 | 0x5 |
| 0x06 | call-site[1].landing_pad | 0c | 0xc（.L3，清理块：调用 Guard::~Guard） |
| 0x07 | call-site[1].action | 00 | 0 = 没有 action 记录，只有 cleanup |
| 0x08 | call-site[2].start | 1c | 0x1c（.LEHB1，清理块里 call _Unwind_Resume） |
| 0x09 | call-site[2].length | 05 | 0x5 |
| 0x0a | call-site[2].landing_pad | 00 | 0 |
| 0x0b | call-site[2].action | 00 | 0 |

后面没有 action 表（action_table_start == 节长度 12），也没有 TType 表。

---

## 4. 任务里 a)-e) 的逐条回答

a) 每个字段是定长还是 uleb128？文档说「3 个 .byte + 2 个 uleb128」对不对？

- header 部分（有 TType 表时）：u8 lpstart_enc + u8 ttype_enc + uleb128 ttype_table_offset
  + u8 call_site_enc + uleb128 call_site_table_length → 3 个 .byte + 2 个 uleb128，对。
- cleanup 用例：ttype_enc=0xff，TType 偏移字段整段不存在 → 2 个 .byte + 1 个 uleb128。
- call-site 表：start/len/landing_pad 用 call_site_encoding（gcc 是 uleb128）编码；
  action 字段始终 uleb128，与 call_site_encoding 无关（libsupc++/libc++abi 都固定 read_uleb128）。
- action 表记录：filter/next 是 SLEB128（有符号），不是 uleb128，也不是定长 .byte。
- TType 表项：按 ttype_encoding 定宽（本机 sdata4 → 每项 4 字节）。

b) LPStart encoding = 0xff 是不是真的「用函数起点」？实测字节是 0xff 吗？
是。三个用例 LSDA 第 0 字节都是 ff；libsupc++ parse_lsda_header() 里 omit 时
info->LPStart = info->Start = _Unwind_GetRegionStart(context)（FDE 的初始地址）。
进一步证明 call-site 的 start 相对函数起点：符号表 .LFB0=0、.LEHB0=0x4、.LEHE0=0x9，
objdump -dr -j .text 显示 call 指令地址就是 0x4，而 LSDA 里写的是 04 05。

c) catch-all 用例的 TType 表偏移是不是 0？TType 表整段是不是直接消失？
都不是。实测 ttype_encoding=0x9b、ttype_table_offset=0x11，表里有 1 个 4 字节零项（无重定位）。
catch-all 的编码方式是 null type_info* 项，不是省掉 TType 表。
（如果硬把 ttype_encoding 改成 0xff，action 里 filter>0 就无法解析；而 filter=0 在 ABI 里是 cleanup，
personality 会把它当成「只清理」，catch-all 抓不到异常。）

d) call-site 表里 landing pad 为 0 表示「该区间无 handler」——实测有没有这种项？
有，三个用例都有第二项 landing_pad=0, action=0：
catch-all 是 start=0x1b len=5，catch-int 是 start=0x1c len=5，cleanup 是 start=0x1c len=5。
语义：该 PC 落在表内但没有 landing pad，personality 返回 _URC_CONTINUE_UNWIND（继续展开）。
注意另一个边界：PC 完全不在 call-site 表内时，__gxx_personality_v0 会 found_terminate（std::terminate），
所以 M2 发射表时要么覆盖所有可能抛出的 PC，要么自己的 personality 对未覆盖 PC 明确返回 CONTINUE。

e) action 表在 catch(int) 里长什么样？如何索引 TType？cleanup 里是 0 还是别的？
- catch(int)：action_table 从 LSDA+0x0d 开始，字节 01 00 = (filter=1, next=0)（两个 SLEB128）。
  call-site 的 action=1 → action_table + (1-1) → filter=1 → TType 第 1 项（反向索引：ttype_end - 1*4
  = LSDA+0x10）→ pcrel DW.ref._ZTIi → _ZTIi。
- catch-all：action 记录与 catch(int) 字节相同（01 00），但 TType[1] 是 0（null）→ catch-all。
- cleanup：action 表完全为空（0 字节），call-site 里 action=0，landing pad 非 0；
  personality 走 action_record == 0 分支 → found_cleanup，phase 2 只做清理、不捕获。

---

## 5. 手写汇编验证（mini.S 系列）与「省掉 DW.ref」的取舍

### 5.1 手写 catch-all LSDA 与 gcc 逐字节一致

mini.S 是「0x9b 间接 personality + 手写 20 字节 LSDA」的最小函数（布局刻意对齐 gcc 的 catch-all 用例：
call 在 +0x4 长 5、landing pad 在 +0x13、第二保护区 +0x1b 长 5、函数长 0x27）。

实测（out/gen.log 第 6 节，check.sh 里也有同样断言）：

```text
[逐字节一致] .gcc_except_table：mini_indirect.o == probe_catch_all.O1.o（20 字节）
              ff9b110108040513011b05000001000000000000
[逐字节一致] .eh_frame：mini_indirect.o == probe_catch_all.O1.o（64 字节）
              1c00000000000000017a504c5200017810079b000000001b1b0c070890010000
              1c0000002400000000000000270000000400000000440e104e0a0e08410b0000
```

也就是说：.cfi_personality / .cfi_lsda 指令 + .section .gcc_except_table 手写 LSDA
这条路完全可行，汇编器/链接器都接受，且产物与 gcc 可以做到字节级一致。

### 5.2 personality 指针：直接 0x1b（省 DW.ref）vs 间接 0x9b

四个变体：

| 文件 | 写法 | 指向 |
|---|---|---|
| mini.S | .cfi_personality 0x9b, DW.ref.mini_personality + 8 字节槽 | 本文件里的 personality |
| mini_direct.S | .cfi_personality 0x1b, .Lmini_pers | 本文件里的本地函数符号 |
| mini_direct_xo.S | .cfi_personality 0x1b, ext_personality | 另一个 .cpp 里定义的函数（同一可执行文件） |
| mini_direct_ext.S | .cfi_personality 0x1b, __gxx_personality_v0 | libstdc++.so 里的函数（跨共享库） |

链接矩阵实测（out/gen.log 第 7 节；check.sh 会重跑并断言）：

```text
[OK]   mini_indirect_pie / mini_indirect_nopie / mini_indirect_shared      （0x9b + DW.ref）
[OK]   mini_direct_pie / mini_direct_nopie / mini_direct_shared            （0x1b 本地符号）
[OK]   mini_direct_xo_pie / mini_direct_xo_nopie                           （0x1b 同一可执行文件其它 TU）
[FAIL] mini_direct_ext_pie
   /usr/bin/ld: out/mini_direct_ext.o: relocation R_X86_64_PC32 against symbol
   "__gxx_personality_v0@@CXXABI_1.3" can not be used when making a PIE object; recompile with -fPIE
[FAIL] mini_direct_ext_shared
   /usr/bin/ld: out/mini_direct_ext.o: warning: relocation against
   "__gxx_personality_v0@@CXXABI_1.3" in read-only section ".eh_frame"
   /usr/bin/ld: ... relocation R_X86_64_PC32 against symbol "__gxx_personality_v0@@CXXABI_1.3"
   can not be used when making a shared object; recompile with -fPIC
[OK]   mini_direct_ext_nopie     （个人不建议依赖：0x1b 直连共享库符号在 no-pie 下能被 ld 放过）
```

结论：

- .cfi_lsda 0x1b + 本地 .LLSDA 标签：可以工作（gcc 自己也这么发；mini 也验证通过）。
- personality 用 0x1b 直接 pcrel 指向本可执行文件内定义的函数（本地符号或另一个 TU 的符号）：可以链接，
  PIE/no-pie/共享库三种方式都试过；可以省掉 DW.ref 间接层。
- personality 来自共享库（或任何链接期无法定址的符号）时：PIE 和 -shared 都会被 ld 拒绝
  （R_X86_64_PC32 不能用于 PIE/shared）；这时必须用 gcc 的 0x9b + DW.ref 方案。
- 建议：M2 发射时统一用 0x9b + DW.ref（gcc 同款）。等 runtime 的 personality 确定链接形态后，
  如果它和代码在同一个可执行文件里、且不需要跨 .so 复用，再考虑换 0x1b 省一层。

### 5.3 一个危险的「能链接但语义错」写法

.cfi_personality 0x9b, <函数符号>（0x9b 却直接指向函数本体、不给指针槽）：
GAS/ld 都不报错，实测 mini_9b_wrong.o 链接成功，CIE augmentation data 是
9b 17 00 00 00 1b 1b（pcrel 指向 .text+0x2a，也就是函数代码本身）。
运行期展开器会把这个地址当成 void* 去解引用 → 崩溃/UB。
0x9b 后面的操作数必须是「指向函数指针的槽」，不是函数本体。

---

## 6. 给实现者的极简清单（M2 照抄）

### 6.1 发射顺序（伪汇编）

```asm
    .text
    .globl  f
f:
.LFB0:
    .cfi_startproc
    .cfi_personality 0x9b, DW.ref.__pa_personality_v0   # 4 字节 pcrel（间接，指向下面的 8 字节槽）
    .cfi_lsda 0x1b, .LLSDA0                             # 4 字节 pcrel（直接，指向 LSDA）
    subq    $8, %rsp
    .cfi_def_cfa_offset 16
.LEHB0:
    call    maybe_throw@PLT          # try.body 里的调用
.LEHE0:
    movl    $1, %eax
.Lret:
    addq    $8, %rsp
    ret
.Llp:                               # landing pad：personality phase 2 跳到这里
    movq    %rax, %rdi
    call    pa_begin_catch           # 这里如果也可能抛，就要有第二条 call-site
.LEHB1:
    call    pa_end_catch
.LEHE1:
    movl    $2, %eax
    jmp     .Lret
    .cfi_endproc
.LFE0:

    # ---- personality 指针槽（0x9b 要求；gcc 用 .data.rel.local）----
    .section .data.rel.local,"aw",@progbits
    .align 8
DW.ref.__pa_personality_v0:
    .quad   __pa_personality_v0

    # ---- LSDA（20 字节的 catch-all 形态）----
    .section .gcc_except_table,"a",@progbits
    .align 4
.LLSDA0:
    .byte   0xff                        # LPStart enc = omit → 用 FDE region start
    .byte   0x9b                        # TType enc = indirect|pcrel|sdata4
    .uleb128 .LLSDATT0-.LLSDATTD0       # TType 表偏移（到表“末尾”，反向索引）
.LLSDATTD0:
    .byte   0x01                        # call-site enc = uleb128
    .uleb128 .LLSDACSE0-.LLSDACSB0      # call-site 表字节长度
.LLSDACSB0:
    # 项1：被保护区间 → landing pad，action=1（catch）
    .uleb128 .LEHB0-.LFB0               #   start（相对函数起点）
    .uleb128 .LEHE0-.LEHB0              #   length
    .uleb128 .Llp-.LFB0                 #   landing pad（相对 LPStart）
    .uleb128 0x1                        #   action = 1
    # 项2：landing pad 内那条可能抛出的 call → 无 landing pad（覆盖住，避免 terminate）
    .uleb128 .LEHB1-.LFB0
    .uleb128 .LEHE1-.LEHB1
    .uleb128 0
    .uleb128 0
.LLSDACSE0:
    .byte   0x1                         # action[0].filter = 1（SLEB128）→ TType[1]
    .byte   0x0                         # action[0].next   = 0（SLEB128）
    .align 4
    .long   0                           # TType[1] = 4 字节 0，无重定位 => catch(...)
.LLSDATT0:
```

### 6.2 数值速查（按函数内布局不同要重算）

- header 5 字节：ff、9b、uleb(ttype_offset)、01、uleb(cs_len)。
- 本清单布局下各偏移：cs 表起 0x05；cs_len = 4 × 条目数（2 条 → 0x08）；
  action 表起 = 0x05 + cs_len（2 条 → 0x0d）；一个 action 记录 2 字节（01 00）；
  .align 4 可能补 0~3 字节；TType 项 4 字节（00 00 00 00）放在最后。
- ttype_offset = 整个 LSDA 字节数 - 3（本布局 20 - 3 = 17 = 0x11）。
  注意 ttype_offset 是相对「该 uleb 字段结束位置」（= LSDA+0x03），指向表尾；不是表头。
- 每个 call-site 项：3 个 call_site_encoding 字段（gcc 用 uleb128）+ 1 个固定 uleb128 的 action。

### 6.3 personality 要做的读取（与本 LSDA 对应）

```text
1) LPStart: enc==0xff → 函数起点（_Unwind_GetRegionStart）；
2) ttype_enc==0x9b → 读 uleb off，TTEnd = 当前位置 + off；
3) cs_enc=0x01, 读 uleb cs_len → ActionTable = 当前位置 + cs_len；
4) 遍历 call-site（直到 ActionTable）：读 start/len/lp/action；
   若 PC ∈ [RegionStart+start, +len)：lp==0 → CONTINUE_UNWIND；
   action==0 → 该区间只有 cleanup（phase1 CONTINUE，phase2 INSTALL_CONTEXT 后重新 _Unwind_Resume）；
   action>0 → 从 ActionTable + (action-1) 起读 (SLEB filter, SLEB next)：
      filter==0 → cleanup；filter>0 → catch：type = 反向索引 TType[filter]（0 表示 catch-all）；
      next!=0 → 继续下一条记录（+next）。
5) phase1 找到匹配 → 返回 _URC_HANDLER_FOUND（并把 landing pad/上下文存起来）；
   phase2 到本帧 → 返回 _URC_INSTALL_CONTEXT（否则不会跳 landing pad）；
   没有 handler/cleanup → _URC_CONTINUE_UNWIND。
```

### 6.4 如果只追求「Z1 先跑通」，可以更简（但只适用自研 personality）

自研 personality 可以自己定义「landing pad = catch-all」，从而省掉 action 表和 TType 表：

```text
ff ff 01 04 <start> <len> <lp> 00     # 8 字节：LPStart omit / TType omit / cs enc uleb / len 4 / 1 条项
```

代价：这份 LSDA 不能被 __gxx_personality_v0 正确解释（filter 语义不同），
也就没法用「换成 libstdc++ personality」做交叉验证。建议 Z1/Z2 先发 6.1 的 GCC 同款 20 字节，
等端到端通了再讨论简化。

---

## 7. 踩到的坑（全部有实测支撑）

1. 文档把 action 表的一个记录当成了「条数」：.byte 0x1 / .byte 0x0 是 (filter=1, next=0) 两个 SLEB128，
   不是 count。判断 action 表长度只能靠 call-site 的 action 下标 + TType 表起点，不能靠读一个「长度字段」。
2. catch-all 不是「类型表退化成空」：它是 TType 表里一个值为 0 的项。如果按文档去发 ttype_enc=0xff
   或 filter=0，运行时 catch(...) 会变成 cleanup，异常永远抓不到。
3. .o 里 TType 零项和正常项在 readelf -x 里看起来一样（都是 0）：RELA 段的占位就是 0，
   必须看 .rela.gcc_except_table 有没有重定位才能区分：
   catch(int) 在 0x10 有 R_X86_64_PC32 DW.ref._ZTIi，catch-all 没有任何重定位。
   只看 hexdump 会得到错误结论。
4. TType 表偏移指向表「末尾」：第 i 项在 ttype_end - i*4。如果按「表头 + (i-1)*4」读，
   catch(int) 会读到表外，catch-all 会读错。
5. action 表与 TType 表之间有 .align 4 padding：catch 用例的 0x0f 字节是 padding，
   不是 action 记录，也不是 TType 表的一部分。别按「action 表长度」硬解析。
6. .cfi_personality 0x9b 的语义是「间接」：操作数必须是 8 字节指针槽。直接给函数符号也能汇编/链接
   （实测 mini_9b_wrong），但运行期会把函数代码当地址解引用。手写汇编时 DW.ref.* 槽要自己发，
   GAS 不会自动生成。
7. 0x1b 直连 personality 的可用范围：本可执行文件内定义的函数（任意 TU）可以；
   指向共享库符号时 PIE 和 -shared 会被 ld 拒绝（R_X86_64_PC32 不能用于 PIE/shared），
   no-pie 能过但建议不要依赖。跨 .so 的 personality 必须走 0x9b + DW.ref。
8. PC 不在 call-site 表内 = terminate（对 gcc personality 而言）：__gxx_personality_v0 在
   found_something 之外会 found_terminate。landing pad 内如果调用了可能抛出的函数，
   必须再发一条 call-site；否则二次抛出会 std::terminate。实测 gcc 的 catch-all/catch-int/cleanup
   都会给 landing pad 内的 call 再发一条（第二保护区）。
9. readelf -wf 显示的 augmentation data 在 .o 里是未重定位的 0：
   要看 readelf -rW 的 .rela.eh_frame 才知道 personality/LSDA 指向谁。
   readelf -x 里 cf ff ff ff 这种「负数」就是 S + A - P 的 P 部分，不是有意义的值。
10. gcc 默认 PIE（Debian 的 g++ 默认 -pie）：所以 gcc 才需要 DW.ref 间接层
    （personality 在 libstdc++.so 里，PC-relative 定不了址）。如果我们的 runtime personality
    是静态链进可执行文件的，理论上可以省掉这层；但为了和 gcc 产物/工具链一致，先用 0x9b。
11. objdump -s -j .gcc_except_table 与 readelf -x 都提示「重定位未应用」，dump 出的字段
    可能是占位 0；要结合 .rela 一起看。
12. 参考规范里的旧文档会误导：itanium-cxx-abi.github.io/cxx-abi/exceptions.pdf 是
    「C++ A.01.15」旧版（1999），其 §7.3.1 描述的 LSDA header 是「自相对偏移 + 低 4 位标志」，
    与现代 GCC/LSB 的 DWARF 编码格式不一致，不能照抄。
    现代格式以 libsupc++/libc++abi 的实现为准（两者一致，见第 8 节）。

---

## 8. 实现依据（交叉验证）

现代 LSDA 的逐字节定义并不在 Itanium ABI 的 abi-eh.html（它主要写 personality 契约）里，
而是事实标准，下面两份独立实现完全一致，本报告的每条语义都对照过：

- GCC libstdc++ __gxx_personality_v0：
  https://github.com/gcc-mirror/gcc/blob/master/libstdc++-v3/libsupc++/eh_personality.cc
  - parse_lsda_header()：LPStart 编码 / TType 编码+偏移 / call-site 编码+长度 / action 表位置；
  - get_ttype_entry()：info->TType - i * size_of_encoded_value(ttype_encoding)（反向索引）；
  - call-site 循环：ip < info.Start + cs_start + cs_len 匹配，landing_pad = LPStart + cs_lp；
  - action_record == 0 && landing_pad != 0 → found_cleanup；
  - action 循环：ar_filter == 0 → cleanup，> 0 → catch，null catch_type → catch-all
    （源码原话：// Null catch type is a catch-all handler）；ar_disp != 0 时继续下一条。
- LLVM libc++abi cxa_personality.cpp（注释里直接画了 LSDA 布局图，
  “actionEntry == 0 -> cleanup”、“ttypeIndex == 0 : cleanup”、
  “Nth type_info* ... ttypeIndex == N”、“0 means catch(...)”）：
  https://github.com/llvm/llvm-project/blob/main/libcxxabi/src/cxa_personality.cpp
- DWARF 编码常量与 read_encoded_value_with_base 对 result == 0 的特判：
  libgcc unwind-pe.h：https://gcc.gnu.org/git/gitweb.cgi?p=gcc.git;a=blob_plain;f=libgcc/unwind-pe.h;hb=HEAD
  （DW_EH_PE_udata4=0x03、DW_EH_PE_sdata4=0x0B、DW_EH_PE_pcrel=0x10、DW_EH_PE_indirect=0x80；
  result != 0 时才做 pcrel 加法和 indirect 解引用）。
- personality 契约（phase1/phase2/返回码/landing pad 参数）：
  Itanium C++ ABI, Exception Handling, §2.5.2：
  https://itanium-cxx-abi.github.io/cxx-abi/abi-eh.html

---

## 9. 复现与产物清单

```bash
cd tools/lsda_ref
bash gen.sh      # 重新编译 + dump + 逐字段解码（全部写到 out/）
bash check.sh    # 重新编译 + 硬编码字节断言 + 链接矩阵断言；退出码 0 表示全部通过
```

本机实测：GEN_EXIT=0  CHECK_EXIT=0（干净目录从零跑）。

文件：

| 文件 | 作用 |
|---|---|
| probe_catch_int.cpp / probe_catch_all.cpp / probe_cleanup.cpp | 三个 gcc 参考用例 |
| probe_main.cpp | 链接参考用例/mini 用的 main + 外部符号桩 |
| mini.S | 手写 catch-all LSDA + 0x9b 间接 personality（与 gcc 逐字节一致） |
| mini_direct.S | 同上，但 0x1b 直接指向本地 personality |
| mini_direct_xo.S | 0x1b 直接指向另一个 TU 的 personality |
| mini_direct_ext.S | 0x1b 直接指向共享库 __gxx_personality_v0（验证会被 ld 拒绝） |
| decode_eh_lsda.py | 自写 ELF64 解析 + .eh_frame/LSDA 逐字段解码（--json 供 check.sh 用） |
| gen.sh | 一键重建全部 dump 并打印解码 |
| check.sh + check_assert.py | 硬断言（关键字节、链接矩阵），可复现校验 |
| out/ | 全部生成物（gen.sh 可随时重建；不必提交） |

产物里值得直接看的文件（gen.sh 重建后）：

- out/probe_catch_all.objdump_s_except_table.txt、out/probe_catch_all.readelf_x_except_table.txt：原始字节
- out/probe_catch_all.readelf_wf.txt：.eh_frame 里的 personality/LSDA 编码
- out/probe_catch_all.objdump_dr_text.txt：.LEHB/.LEHE/.L4 对应的真实地址
- out/probe_catch_all.readelf_r.txt：重定位（TType 项、personality 槽、LSDA 指针）
- out/probe_catch_all.decode.txt：逐字段解码（含校验结果）
- out/mini_indirect.decode.txt、out/mini_direct*.decode.txt：手写变体的解码与链接结论
