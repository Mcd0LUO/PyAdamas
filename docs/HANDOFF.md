# 接续 Handoff（额度耗尽暂停点）

> 写于 2026-11-21 上午。用户额度耗尽叫停。本文档供下一会话无缝接续。
> **当前 master = origin/master = `8e8873e`（0.356），已同步。夜间自主集成 24 版（0.333-0.356）。**

## 0. 立即接续：P3 big BigInt（用户点名要做，未完成）

**活跃 worktree `/tmp/wt-big2`，分支 big2，HEAD `0e70907`**（基线 master 8e8873e/0.356）。代理名 big-int2（qwen4-max-preview，已停，可 send_message 恢复或派新代理）。

### 已完成 commit
| commit | 块 | 内容 | 验证状态 |
|---|---|---|---|
| b6005d8 | A 运行时 | rt_big.c（985 行，从 big1 拷贝适配）+ rt_internal.h PYADAMAS_RC_BIGINT=9 + PyAdamasBig + 30 个 pa_big_* 声明 + rt_format.c pa_big_fmt + CMake RT_PARTS 加 big（rt.o/rt_asan.o 合并）+ tools/bigfuzz | ✅ **bigfuzz CPython 对拍 normal 98324 + ASan/GC 36926 例零差异，ASan leak/UAF 全 0** |
| 32cc212 | B 类型+字面量 | TyKind::Big/CK::Big/t_big()/`a:big` 标注/int_promote·join·assignable big 顶层/BIG tag=12(static_tag_of)/ir_is_heap·ref/超 u64 十进制字面量→big（u64 路径逐字不动）/±big 折叠/u64_lit big 上下文消费/跨模块 type=24·default tag=9/IrOp::ConstBig 永生静态字面量（编译期 Horner 转 limb，.Lbig 数据段+strtab）/codegen_big.cpp | ✅ 构建+冒烟，365 u64 回归过，limb 编码 CPython 逐位一致 |
| 0e70907 | C 降级 WIP | RtFn 28 个 big 入口+装填分类/lower_binop big 分支(add·sub·mul·floordiv·mod·pow·and·or·xor·shift)/big 真除 pa_big_truediv 精确舍入/一元 neg·not/**比较重构 lower_cmp_single**(big/big·big/int 转 big 比·big/float 精确混比含 NaN·链式比较按静态类型落槽)/convert_to·coerce 进出 big/truth_bool/print(big) | ⚠️ **仅构建过(-Werror)，未跑任何运行回归** |

### 剩余工作（按优先级，big-int2 接手清单）
1. **块C 运行验证**：写最小 big 程序 `build/pyadamas run -o build/p build/smoke_big.py` 冒烟（print/算术/比较/真除/链式比较已接好），修运行期问题。
2. **块D unknown 链路**（关键缺符号）：`pa_any_to_big` 运行时函数**还没写**（RtFn/声明已占位，big 程序走 unknown→big 会缺符号链接失败）。rt_any.c：探针 BIGINT→inc 返回/int→from_i64/float 盒→from_f64；load_tag/ANY_BIG 算术·比较；rt_str.c repr/print_any/str_any BIGINT kind 分派；pa_any_type BIG→类型码。
3. **块E builtins**：str/repr/hex/bin/oct/abs/pow/format/int(big)/float(big)/f-string Big 分派（lower_builtin_text/misc 位置已勘明未改）；typecheck int(big)→big 恒等。
4. **补拦截**：int<<big 计数（typecheck 放行但降级会错，需拦）。
5. 回归 tests/run/395+（big 算术/大数 2^100·阶乘/转换 i64↔big/repr/位运算/幂/除零/unknown 进出/builtins，**CPython 逐字节对拍**）。
6. **全闸门**（块B/C asm 不变性目前只有设计论证无实测）：现有 int 程序 asm 逐字节不变(38 基准 cmp)+difftest 0+全套件 normal/ASan(JOBS=1)+CPython big 零差异+语料零差异+ASan leak 13/174 钉子+big 用例+check_size 无新增超限。
7. docs/big-landing.md + CHANGELOG 0.357 + 集成 master（merge --no-ff，重建 build/lead 复验，SSH 推送，清理 big2 worktree）。

### BigInt 已定死边界（负面，文档化）
- big+float→big（float 侧 int(float) 截断，pa 格口径，任务点名）。
- big**负指数→ValueError（CPython 返 float）。
- big→int 是 cast 截断。
- list[big] 相等/排序/dict 键哈希仍机器字口径（未接）。
- 语义黄金标准 CPython：//向下取整/%与除数同号/负移位算术右移/位运算无限补码/除零 ZeroDivisionError 可捕获。

### 运行时设计要点（rt_big.c）
RC 头+{int64_t nd; uint32_t d[]}，nd 带符号 limb 数(CPython ob_size 口径)，基 2^32 小端规范化无最高位零 limb，柔性数组内联同一 malloc 块，**immutable**（运算产新对象，free(h) 即可无 dtor）。kind=PYADAMAS_RC_BIGINT tracked rc_alloc，无堆出边不成环，unknown 探针认识。永生静态字面量 pa_register_literals。入口操作数借用、返回 rc=1，中间量裸 malloc/free(PA_FREE_SCOPE)或 pa_gc_pin，别裸露半成品 RC 跨分配点。

## 1. 其它 worktree
- `/tmp/wt-big1`（big1，06106e5）：**旧** P3 partial（基线 0.341，重构前），rt_big.c 原始 985 行 + 未提交 Big TyKind/codegen WIP。big2 已拷贝其 rt_big.c，big1 可清理（git worktree remove --force /tmp/wt-big1 && git branch -D big1）。

## 2. 夜间已交付（0.333-0.356，24 版，全推送）
详见 docs/optimization-roadmap.md 迭代日志。摘要：
- 性能：SIMD 数值核(特化胜 Go/C++)/整数 BCE(matmul −25%)/regalloc/递归展开(fib −29%)/borrow(dict_wordfreq −4.5% 通用)/**编译速度 −46.8%(≈1.88×)**/**SIMD 通用化(非基准 1.93×)**/SIMD 深化(int 乘 2.36×·Convert 2.34×·多 reduction 1.38×，复活死代码 reduction 修 5 潜伏 bug)。
- 语言：P1 定宽整数全(i8-i64/u8-u64/char/short+收尾)/P2 unknown 类型标签(pprint 解锁)/P4 全 4 块(异常类当值·suppress·file-io.StringIO·traceback·dataclass)。
- stdlib 纯 pa：7 模块/tarfile/textwrap·string·pprint/statistics/configparser。
- 重构：拆 5 超限文件(check_size 清零)。内存安全：ir.cpp vreg_str UAF/lower_impl.h 求值顺序 UB/异常捕获 leak(RC 环误判修正)/插值字符串 leak。

## 3. 其它 backlog（待用户定）
- **编译器早返回漏 dec 通用修**：lowering release_slots 早返回用单遍部分槽标记漏 dec（var/v 堆写入前 slot_owned 未置）；通用修 fixup_early_return_releases 会改 a2_config_parse 基准 asm（其 coerce 自身潜伏 leak）违闸门→需连同基准 asm 基线独立处理。详见 docs/strfix-rc-early-return.md。
- float32（F32 TyKind，P3 可选部分）。
- SIMD AVX2（cpuid+xgetbv+ymm 池）/改序 float reduction（输出风险）。
- stdlib 扩展（fractions/decimal 需 big；logging/unittest 复杂）。

## 4. 纪律 / 环境（接续必读）
- cwd /src/luo-proj/pyadamas。build/lead=当前 Release 对照（0.356）；**build/release=0.271 oracle 绝不重建**。
- git 需 `-c safe.directory=<path>`。SSH 推送：`ssh -i /home/celestea/.ssh/id_ed25519_root_mc -o BatchMode=yes -o UserKnownHostsFile=/home/celestea/.ssh/known_hosts -o ConnectTimeout=8 root@127.0.0.1 'cd /src/luo-proj/pyadamas && git -c safe.directory=/src/luo-proj/pyadamas push origin master'`。
- 集成套路：branch merge master 解冲突→独立复核闸门→master merge --no-ff→CHANGELOG 定版→重建 build/lead 复验→SSH 推→worktree remove --force + branch -d。CHANGELOG 最新在前，选择性 git add。
- 每轮闸门：difftest 0+套件 normal(JOBS=8)/ASan(JOBS=1，避免 env flake)+CPython 对拍+语料零差异+ASan detect_leaks=1 钉子(13/174/88/192/92/93/94/119/153)+相关 asm 逐字节不变+check_size 无新增超限+性能 abbench 交替绑核中位。
- 门控默认关 flag-off asm 逐字节不变是最高不变量。默认开：gc-sections(PYADAMAS_GC_SECTIONS=0 关)/VECT(PYADAMAS_VECT=0 关，0.384 起)。默认关 opt-in：SIMD(PYADAMAS_SIMD=1 开，0.384 起 —— 照着 bench 循环形状写的模板，不进默认管线)/LOOP_OPT/RA_OPT/INLINE_OPT/BORROW_OPT/TYPE_TAGS/LANG_GAPS/GC_INCR_MARK。
- 模型：provider celestea（qwen4-max-preview 性能/运行时，deepseek-v4.1-flash stdlib）。harness 间歇早夭频繁→send_message 恢复（保留 WIP+上下文）。单槽位顺序（用户收回多代理并行）。
- run_code timeoutMs≤600000+run_in_background 长任务；TS 模板 literals 含 C++ 反引号/${} 会 parse fail→用双引号 JS 串+\n 或 lines.join。bash cwd 每次重置（用 cd 或 git -C）。
- 用户在线驱动， terse 指令，重通用性（反基准特化）/完整性/诚实负面结论。

## 5. 测试规模
tests/run 至 394（395+ 待 big 写）；difftest 346/0；套件 365 ok；check_size 通过（0 超限）。