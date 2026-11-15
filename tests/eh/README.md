# tests/eh —— LSDA 发射器（Z2-a）golden 字节测试

这是 [docs/zero-cost-exceptions.md](../../docs/zero-cost-exceptions.md) 里 **Z2** 的第一步落地：
只做「保护区计划 → GNU as 汇编文本」的发射器（`src/eh_emit.h` / `src/eh_emit.cpp`），
**不接 codegen、不改任何现有行为**。字节布局的权威口径是
[tools/lsda_ref/FINDINGS.md](../../tools/lsda_ref/FINDINGS.md) §0.1 / §6（gcc 14.2 实测标定）。

## 怎么跑

```bash
bash tests/eh/run.sh
```

- 依赖：Linux x86-64、gcc/g++ 14.2、binutils（readelf / objcopy / od）。构建产物放 mktemp，退出即删。
- **不碰仓库的 CMake/ninja 构建**：直接用 `g++ -std=c++20 -Wall -Wextra -Werror` 编
  `src/eh_emit.cpp + tests/eh/main.cpp`。
- 预期：`==== 结果：全部 PASS（33 项断言）====`，退出码 0。
- 测试驱动 `main.cpp` 会先跑 `eh_plan_check` 自检（合法/非法计划、空计划、非法计划抛异常），
  再为 6 个用例各生成一份 `.s`；`run.sh` 逐份 `gcc -c` 汇编、
  `objcopy -O binary --only-section=.gcc_except_table` 抽出原始字节、与期望字节逐字节比对，
  并做结构断言。

## 用例与期望字节

| 用例 | 形态 | 字节数 | 期望字节（hex） | 来源 |
|---|---|---|---|---|
| `catch_single` | 1 条 handler，无 LP 覆盖区间 | 16 | `ff9b0d01040405130101000000000000` | 按 §0.1 口径手算 |
| `catch_gcc20` | handler + lp=0，两条 range | 20 | `ff9b110108040513011b05000001000000000000` | **FINDINGS §0.1 gcc 基准** |
| `cleanup_gcc12` | 全 cleanup（ttype=0xff） | 12 | `ffff010805050c001c050000` | **FINDINGS §3.3 gcc 基准** |
| `multi_handler` | handler/cleanup/handler，含 2 字节 uleb | 28 | `ff9b19010e040513010a050b00a00205b00203010001000000000000` | 按 §0.1 口径手算 |
| `catch_gcc20_labels` | 标签模式，布局同 catch_gcc20 | 20 | 同 `catch_gcc20` | 验证 `.uleb128 .LEHE0-.LFB0` 与数值模式字节等价 |
| `combine_two` | 两个函数同一 TU | 32 | 两份 16 字节单 handler 拼接 | 验证同段多 LSDA 顺序/对齐、DW.ref 只发一次 |

`catch_gcc20` 与 `cleanup_gcc12` 就是 gcc 自己产物的逐字节复制 —— 这是本目录的硬验收基准。

结构断言（每个用例都做）：

- `.eh_frame` CIE augmentation 必须是 `zPLR`（personality + LSDA 都在）；
- CIE augmentation data 的 personality 编码必须是 `0x9b`（间接），0x9b 后面必须走 `DW.ref` 槽；
- `.data.rel.local` 必须有 `pa_personality` 重定位（DW.ref 指针槽）；
- `.gcc_except_table` 上**不能有任何重定位**（TType 的 null 项必须是裸 4 字节 0，否则不是 catch-all）；
- `combine_two` 里 `DW.ref.pa_personality` 符号只能出现一次（每个 TU 一次）。

## 发射器接口（与任务建议的差异）

任务建议的单函数 `emit_eh_function(out, plan)` 被拆成三段，原因不是风格：

> `.cfi_personality` / `.cfi_lsda` 必须落在 `.cfi_startproc` 与 `.cfi_endproc` 之间；
> LSDA 字节必须落在 `.gcc_except_table` 段；`DW.ref` 指针槽必须整个 TU 只发一次。
> 三者的插入点不同，一次调用不可能同时满足。

codegen 接入顺序（Z2-b）：

```cpp
// 1) codegen_block.cpp：emit_function 里，紧跟 cfi(".cfi_startproc") 之后
emit_eh_func_directives(out_, plan);
// 2) codegen.cpp：全部函数 emit 完之后（当前段不是 .text 的位置）
for (每个带计划的函数) emit_eh_lsda(out_, plan);
// 3) 每个 TU 一次
emit_eh_dwref(out_);   // .data.rel.local 里的 8 字节 .quad pa_personality
```

两种输入模式：

- **数值模式**：`EhRange.begin_off/end_off/lp_off` 是相对函数起点的字节偏移。golden 测试用它。
- **标签模式**（codegen 必须用）：`begin_label/end_label/lp_label` + `EhFuncPlan::func_label`
  发射 `.uleb128 .LEHE0-.LFB0`，数值和 uleb 编码宽度由汇编器算 —— codegen 在发射期根本
  算不出指令字节数。`func_label` 必须取「`.cfi_startproc` 所在 PC」的那个标签
  （注意 `.LfN` 与函数名标签之间可能有 `.p2align 4` 的 padding，别拿错标签）。

`EhFuncPlan::lsda_index` 生成 `.LLSDA<N>` / `.LLSDACSB<N>` 等本地标签；同一 TU 内不同函数
必须给不同 index。`ranges` 为空时三段都不发射。计划非法时 `emit_*` 抛
`std::invalid_argument`，`eh_plan_check` 可提前拿可读错误。

## 硬约束与实现的对应

| # | 硬约束 | 实现位置 |
|---|---|---|
| 1 | `0x9b` 后必须是 8 字节 DW.ref 指针槽 | `emit_eh_func_directives` + `emit_eh_dwref` |
| 2 | action 记录 `(filter>0, next)` 才是 catch-all；filter=0 是 cleanup | action 表段：`01 00` |
| 3 | TType 偏移指向表尾；表项 4 字节且无重定位 | 数值模式 `ttype_table_offset()`；`.long 0` |
| 4 | LSDA 末尾 `.align 4` padding 不属于任何表 | action 表后 `.align 4` |
| 5 | call-site 表长度是字节数 | `call_site_table_len()`（逐字段 uleb 宽度求和） |
| 6 | LP 覆盖 range（LP 里调可抛函数时需要） | 由调用方决定是否加一条 `lp=0` 的 range；Z2 约定 LP 只调不可抛的运行时函数 |

另外一条不在任务清单里、但实测必须的：**call-site 的 action 字段是 action 表内 1 起的
字节偏移，不是记录序号**（libsupc++/libc++abi 都按 `action_table + (cs_action - 1)` 定位）。
每条 `(filter=1,next=0)` 记录 2 字节，所以多条 handler 的 action 值是 1, 3, 5, …
（`multi_handler` 用例就盯这个）。

## 独立交叉核对（用 tools/lsda_ref 的解码器）

除了与 gcc 基准比字节，开发时还用 [tools/lsda_ref/decode_eh_lsda.py](../../tools/lsda_ref/decode_eh_lsda.py)
（另一套独立实现的 ELF/LSDA 解析器）解码发射产物：

```bash
python3 tools/lsda_ref/decode_eh_lsda.py <生成的>.o
```

它对 `catch_single` / `cleanup_gcc12` / `multi_handler` 的结构校验全 `[OK]`
（TType 表末尾、call-site 表长度与实际字节一致、TType 项数 = 最大正 filter 下标、逻辑长度不越界）。
输出里 `call_site[i] 起止地址能对回符号` 是 `[FAIL]` 属预期：golden 用例是合成函数，
数值模式的偏移本来就不对应真实标签（这正是它只做字节比对的原因）。

这次交叉核对抓到了一个真实 bug：最初把 action 字段按「记录序号」发成 1,2，解码器报
`TType 表起点 落在 action 表里，结构非法`；改成 1 起字节偏移后通过。

## 遗留 / 下一步

1. **未接 codegen**（Z2-b）：还需要 lower 把 try 区间标出来、codegen 在保护区边界发
   `.LEHB/.LEHE` 标签、生成 landing pad 区段与 CFI、收集 `EhFuncPlan` 列表并选
   `--zero-cost-exc` 开关；本发射器只负责字节。
2. **typed catch 未支持**：现在 TType 表固定一个 null 项（catch-all）。Z3 做类型过滤时
   需要扩展 `EhRange` 的 handler 描述并改 TType 表/action filter（filter 可以 >1）。
3. **LP 覆盖 range 未自动生成**：见硬约束 6，等 Z3/Z4 做 cleanup LP 里的 `_Unwind_Resume`
   时再补第二条 call-site。
4. Win64/SEH 不适用：`.gcc_except_table` 是 ELF 的东西，接的时候要按
   `TargetABI::gnu_cfi` 关掉。
