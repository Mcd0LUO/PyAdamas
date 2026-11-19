# 变更记录

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。版本号是自己定的，规则：`0.x` 阶段每个「能跑通新一层」算一个次版本。

早期阶段没有逐条留痕，按阶段归纳；0.4 之后逐条记录。

## [0.311] - 2026-11-19

### 编译吞吐：直调 as/ld（build 全程 −9.4%，累计对 0.309 −11.6%）

- **取证**：build 的墙钟按模式阶梯拆开（无参启动 / check / ir / asm / build，绑核 11 次中位）：nbody 启动 2.4 + 类型检查 1.5 + 降级+优化 3.0 + 代码生成 3.3 = 前端 10.2 ms，build 54.9 ms ⇒ **cc 占 44.7 ms（81%）**；dict_wordfreq 41.4/48.3 ms（86%）；含 6 个 stdlib 模块的 140_stdlib_batch 124.1/134.5 ms（92%）。strace -f -e execve 显示每次 build 的 cc 链路是 `pyadamas → sh → cc → as`（汇编）与 `pyadamas → sh → cc → collect2 → ld`（链接）——**双重 fork/exec + sh**；`cc --version` 单独就 3.0 ms（/bin/true 1.4 ms），链接期 gcc 驱动（specs 解析/库搜索/两次 exec）约 11 ms，ld.bfd 25 ms。
- **优化（配置期复刻 gcc 命令行，运行期直调）**：CMake 在 configure 时用 `-print-file-name` / `-print-prog-name` / `-print-search-dirs` 探测出 gcc 实际传给 as 与 ld 的命令行（crt*.o、-L 目录、-pie -z now -z relro --hash-style=gnu --build-id --eh-frame-hdr、装载器路径、-lgcc/-lgcc_s/-lc 次序），编成 PYADAMAS_AS_EXECUTABLE / PYADAMAS_LD_FLAGS / PYADAMAS_LD_TAIL；driver 运行时直接 exec **as --64 -c** 与 **ld**（省掉 cc 驱动与 collect2 两层）。新文件 src/driver_link.cpp（71 行）+ driver_link.h，driver_build.cpp 只加 ~10 行接线。**五重回退保行为不变**：探测失败 → cc；PYADAMAS_CC_FLAGS 非空（--asan 等）→ cc；目标非 x86_64-sysv（Win64 交叉汇编）→ cc；link 参数含 .c/.其它非产物（cimport 的 structabi/callbacks/c_float32/cdecl/cfi 会现编 C 源）→ cc；直调 ld 失败 → 再走 cc 一次。环境变量 PYADAMAS_NO_DIRECT_LD=1 可整体关闭（A/B 与排障用）。
- **踩坑（都被验证抓住）**：① .interp 必须逐字节等于 gcc 写的 /lib64/ld-linux-x86-64.so.2——用 -print-file-name 得到的 /usr/lib/gcc/.../../../../x86_64-linux-gnu/... 会长 0x34 字节，段偏移全变（cmp 立刻发现）；② 链接参数里的 .c 源文件 ld 当链接脚本读，套件 108_structabi/112_callbacks/215_c_float32/cdecl/cfi 五项 OUTPUT DIFF，加 direct_ld_args_ok() 过滤后全绿；③ 直调 as 在带 -fsanitize 时会被喂无法识别的选项，故 PYADAMAS_CC_FLAGS 非空一律回退。
- **实测（交替 A/B、打乱顺序、绑核 24，7 轮中位；A=0.310 二进制，B=本轮）**：build -O2 --rebuild 11 个负载合计 **676.49→613.14 ms（−9.4%）**，单项 −7.3%~−10.9%（nbody −7.3%、spectral −10.5%、rec_memo −10.6%、queue_sim −9.5%），含 6 模块的 stdlib_batch −8.0%、stdlib54 −10.9%；**对 0.309 原点累计 711.79→629.56 ms（−11.6%）**。前端 asm 未动（10 负载合计 69.63→69.20 ms，−0.6%，噪声内）——本轮省的全是编译器之外的进程链。
- **验证**：行为零变化——21 个 bench/语料可执行文件「直调 as/ld」与「PYADAMAS_NO_DIRECT_LD=1 走 cc」链接结果 **cmp 逐字节相同**（含 .note.gnu.build-id）；243 个语料的 asm 输出与 0.309 逐字节一致；difftest **226/0**；普通与 ASan 套件各 **244 ran ok / EXIT=0**（--asan 走 cc 回退，插桩链接不受影响）；0.271 oracle 语料零差异（仅 201-229 新增用例 rc 差为已知预期）；规模检查仍只有原有 5 处超限（driver_build.cpp 1438→1461 行仍在既有名单内，新文件 driver_link.cpp 71 行）。
- **同一轮撤回的尝试**：gen_block 预扫描的两张 unordered_map（block_stores_/block_first_use_）改扁平数组 + 脏列表，calibrate 后 asm 合计 69.28→69.07 ms（−0.3%，噪声级），确认分配点不是当前瓶颈后退回（diff 保留在会话记录，不进仓库）。
## [0.310] - 2026-11-19

### 编译吞吐：静态 libstdc++ + 两处 O(n²) 编译期扫描（前端 −12.9%，语料 check 逐文件 −23.1%）

- **阶段占比（先量化，Xeon Gold 6252 绑核 24，多租户负载 3-5；单次 build 总耗时占比）**：cc 汇编（cc -c）≈ 17%（dict_wordfreq 8.2/51 ms），**链接（cc -o）≈ 70%**（36/51 ms：ld.bfd 25 ms 不可压缩 + gcc 驱动 11 ms），编译器前端（词法/解析/类型检查/降级/IR 优化/codegen/渲染）≈ 19%；逐阶段 PYADAMAS_TIMING：check 0.2-0.5 / lower 0.6-7.1 / asm 0.7-6.3 / cc 5.3-11.2 ms。语料批处理（tests/run 227 例逐文件 check）则是**进程启动占主导**：单进程 3.87 ms 里 1.24 ms 是动态装载器解析 libstdc++.so.6 的 ~800 个符号（LD_DEBUG=statistics：1972 次重定位 / 43 万拍，其中 1720 次来自符号查找缓存），语料本身平均只占 ~0.6 ms。perf（--children，整条 build 链）证实：链接期 bfd_link_hash_traverse 12.7% / bfd_elf_link_add_symbols 5.1% / bfd_hash_lookup 4.9%（全部在 ld 内；-z now、PIE、reflink、--threads（bfd 不支持）、strip 局部符号、输出到 tmpfs、-O1 逐项 A/B 均为 0 或不可用，本机无 lld/mold）；前端则 _int_malloc + _int_free ≈ 13%、assign_vreg_slots 6.3%、汇编行重解析（parse_instr + parse_operand）5.5%、rebuild_use_def 4.5%、std::vformat 2.1%。
- **优化 1（链接/启动，一行）**：-static-libstdc++（新 CMake 选项 PYADAMAS_STATIC_LIBSTDCXX，默认开）。编译器每次被调用都要装载 6.9 MB 的 libstdc++.so.6 并解析其符号；静态链接后进程启动 **5.01M→3.19M 条指令（−36%）、43.1 万→31.4 万拍（−27%）**，tests/run 逐文件 check 227 例 473.9→365.9 ms。不改生成代码/运行时（用户程序仍动态链接），二进制 5.2→7.0 MB（页缓存只读）。
- **优化 2（codegen）**：AsmGen::assign_vreg_slots 的跨块判定原本是「每个 vreg 扫全函数每条指令的 a/b/c/args（含内层 lambda 分配）」= O(vreg × 指令)，nbody 的 simulate/advance 各 3000+ 指令 × 4000+ vreg；改成一趟顺扫：对每条指令的每个操作数，若「所在块 ≠ 定义块」则置 cross_block_[r]。语义与原实现**逐位相同**（原来是「命中第一个非定义块的使用点即置位，之后保持」），243 个语料 asm 逐字节比对确认零差异。
- **优化 3（opt）**：dce_function 每轮 rebuild_use_def() 重建三张 unordered_map（inst_by_id/def_of/uses_of，逐指令哈希插入 = profile 里 _int_malloc 与 _Map_base::operator[] 的大头），而 DCE 只需要「某 vreg 被读了几次」。改为一次性线性 vector<uint32_t> 计数 + 删除时精确递减（删纯指令只会让计数变小，等价于下一轮重建），并保留外层不动点循环，保证「删除后**前面**已扫过的指令变死」的连锁与旧实现一致（首版漏了这一点，32_exception_value / 39_match_advanced 的 asm 多留一条 const.int -1，被逐字节比对抓出后修好）。
- **实测（同负载交替 A/B、打乱顺序、绑核，7-9 轮中位）**：前端 asm（10 个负载合计，排除 cc）**79.21→68.99 ms（−12.9%）**，单项 −11.1%~−14.6%，其中 −6.2% 来自静态 libstdc++（启动）、−4.8% 来自两处 O(n²) 扫描；build -O2 --rebuild（11 个负载合计，含 cc 汇编+链接）**654.3→633.9 ms（−3.1%）**，单项 −1.6%~−4.4%（前端只占 ~19%，故绝对值有限），stdlib_batch −3.8%、stdlib54 −3.8%；语料逐文件 check（bash 循环 227 文件）**474.8→365.1 ms（−23.1%，2.09→1.61 ms/次）**，--imports 525.2→412.8 ms（−21.4%）。
- **验证**：行为零变化——243 个语料（bench realistic/compare 11 + tests/run 227 + 其它）的 pyadamas asm 输出与 0.309 逐字节一致；difftest **226/0**；普通与 ASan 套件各 **244 ran ok / EXIT=0**；ASan detect_leaks=1 抽查 13_gc/88_defaultdict/192_dict_cache/66_genexpr/119_gen_except_suspend 全干净；0.271 oracle 语料零差异（build/release 未触碰；201-229 等新增用例 rc 差为已知预期）；规模检查仍只有原有 5 处超限（codegen_regalloc.cpp 2475→2477 行、driver_build.cpp 1438、ir_ops.h 1276、lower_impl.h 1242、typecheck_types.h 1329，无新增超限文件）。
- **未做（如实记录）**：链接占 build 总耗时 ~70%，其中 ld.bfd 的 25 ms 由「gcc 链接 rt.o（2064 个符号、~350 段、17 MB .bss）+ -z now 全符号绑定」决定；能砍掉它的只有换链接器（本机无 lld/mold，实测 clang -fuse-ld=lld 36→23 ms，但 /usr/bin/ld 只有 bfd 与 ld.so，lld 未安装）或缩 rt.o（运行时职责，超出前端范围），故本轮不硬优化前端之外的部分。
## [0.309] - 2026-11-19

### release preset -O1→-O2（保守退路）

- 0.308 的 release -O1 经三层吞吐测量中性，用户选择保守 -O2：构建收益仍有约 −10%（compile-speed.md 实测 −10.1%），编译器二进制运行余量更足。一行 CMAKE_CXX_FLAGS_RELEASE 改动。
- 生成代码与运行时不受影响（用户代码 -O2、rt.o -O2 本就独立于 preset）；build/lead 口径不用 release preset，行为无关，验证沿用 0.308 终态（226/0、244×2）。build/release（0.271 oracle）仍未触碰——release preset 的 binaryDir 指向它，重建会覆盖 oracle，需新 oracle 时另开目录。

## [0.308] - 2026-11-19

### 编译器构建速度优化（ccache + 模块扫描关闭 + release -O1）

- **集成**：speed 分支（8563c03）合入 0.307。测量报告随分支合入：docs/compile-speed.md（基线/top TU/-ftime-report：53% 时间逐 TU 模板实例化、29% 重载决议、预处理仅 3%；逐项 A/B 与放弃记录）。冲突仅 CMakeLists RT_PARTS 一处：保留 master 的 vthread + speed 的 launcher 接线。
- **生效手段**：① 关 CMake4 的 C++23 模块扫描（项目无 modules 却被注入 -fmodules-ts + 89 遍 -E 扫描，ccache 见之 100% 拒缓存）——墙钟中性，是 ccache 的前提，-DPYADAMAS_MODULE_SCAN=ON 可恢复；② ccache 自动探测（-DPYADAMAS_USE_CCACHE=OFF 可关）+ 接线 68 个运行时 custom command（不继承 launcher）；③ release preset -O3→-O1 -DNDEBUG——吞吐三层测量（pyadamas build 6 负载、重负载×10、216 用例纯前端 check）O1/O2/O3 全部中性（dev -O0 才慢 27%），取最大构建收益；保守退路 -O2（−10.1%）一行可改。
- **实测**（-j32 clean 全量，交替轮转抗噪 ×3 中位，负载 ~27-30 平稳窗口）：dev warm 全量 ~48s→**3.47s（13.9×**）；release cold 58.45s→**41.10s（−29.7%**）；release warm ~58s→**0.45s（130×**）；cold 无惩罚；真实源码改动的增量不变（改动 TU 必然 miss）。configure ~1.4s、链接 dev 3s/release 0.3s 均非瓶颈。Top1 重 TU driver_build.cpp（预处理 145k 行；全项目 89 TU 共 1050 万行，95% 标准库）。
- **放弃/记录**：lld/mold 仅 bfd 无安装权限且链接占比小；UNITY_BUILD 匿名命名空间同名 helper 跨文件 ODR 冲突（opt_dict_bump/fuse 的 is_rt/same_slot）；PCH 按配置匹配 -O 机制修好（dev 137MB/release 126MB 独立 .gch）但全量仍≈0（实例化不省）——保留默认 OFF 的 option（增量 −14/−18%）；std modules（GCC15 import std 实测可行，BMI 15.7s/30MB）是唯一根治实例化重复的方向，集成太深记录不做。typecheck_types.h 被 74/89 TU 间接包含，<format>/<ranges>/<expected> 最重扩散源。
- **验证**：行为零变化——分支口径（0.306 基）release 最终配置与 dev -Og 各 234 ran ok EXIT=0 + difftest 216/0；合入 0.307 后复核 difftest **226/0**、普通与 ASan 套件各 **244 ran ok EXIT=0**。裸 -O0 dev 的 118/154 两失败与原配置基线完全一致（既有 Debug 断言，另票）。build/release（0.271 oracle）未触碰；release preset 新旗标在分支已验证，合并未改 preset 内容。CI 装 ccache 并持久化 CCACHE_DIR 可拿满 warm 收益。

## [0.307] - 2026-11-19

### Go 式无色虚拟线程（go/channel/join，M=1 协作调度，合入 vthread）

- **集成**：把 vthread 分支（dbbb01e，基于 1e47057/0.304）合入 0.306（master 已含 0.305 cffi + 0.306 dylib 改名）。分支自编号 0.305 与 cffi 撞号，改编号 0.307。三件：① `go f(args)` spawn 任意普通函数（无色，被调函数零标注，阻塞原语在运行时内部让出，调用链无感）；② channel（make_chan/send/recv，无缓冲手递手 + 缓冲 FIFO，FIFO 唤醒）；③ join/sleep(ms) + 退出确定性 drain。与现有 async/await/生成器（rt_gen.c 无栈状态机）、CPython 桥、C FFI 完全并存互不改动。设计文档随分支合入保留：docs/vthread-design.md。
- **冲突解法（两边语义都保留）**：四处文本冲突，均为同位追加——① CHANGELOG.md：分支自编号 0.305 条目与 master 的 0.305 cffi/0.306 撞位，保留 master 历史、本条改写为 0.307；② CMakeLists.txt 的 PYADAMAS_RT_PARTS：cffi 加 cffi、vthread 加 vthread，合成 `… diag cpy cffi vthread`（同一列表喂 rt.o 与 rt_asan.o）；③ src/module.cpp enc_type：DYLIB 与 VThread/Chan 三 case 并存，都按 unknown 序列化；④ src/typecheck_types.h：TyKind 尾追加 DYLIB、VThread、Chan（master 存量值不位移），type_name 补 dylib/vthread/chan[T]。其余自动合并干净、两侧叠加（逐一核对语义）：ir_ops.h（DYLIBLoad/Close 保持 master 原位，VtSpawn/VtJoin/VtSleep/VtShutdown/ChanNew/ChanSend/ChanRecv/ChanSendF/ChanRecvF 追加枚举尾，rt_name 是 switch 无索引冲突）、codegen_call_rt.cpp（VtShutdown→none 组、VtSpawn/VtJoin/VtSleep/ChanRecv/ChanRecvF→i1 组、ChanNew/ChanSend→i2 组、ChanSendF→i1f1 组，DYLIB 留原指针组）、rt_internal.h/ir_types.h/lower_call.cpp/lower_impl.h/lower_method.cpp/typecheck_call.cpp 各分支两侧叠加。builtin_sigs.cpp 与 driver_build.cpp kModuleTable 仅 master 侧改动（vthread 不新增伪模块），无冲突。
- **模型（设计文档 §1）**：M=1 协作式有栈虚拟线程——单 OS 线程跑专用调度器（独立 mmap 栈，libco 式纯 C 循环）+ 全部 vthread。RC/GC/池/temp_stack 等 119 个单线程 static 全局态零改动、零原子零锁——M=1 是先验绕过线程安全大山的关键。每 vthread 一块 mmap 栈（默认 256KB + 底部 4KB PROT_NONE guard，池化复用上限 256 块，PYADAMAS_VT_STACK_KB 可调 16..65536）；切换 asm 存恢复 rsp+8/callee-saved/rip（.cfi_undefined 禁跨栈展开），生成代码全 %rsp 相对帧，任意栈透明运行。主流程是 root vthread（OS 主栈）。
- **调度器形态与切换（§1/§3，踩坑 §5 全部已修）**：所有 vthread 阻塞/死亡切回同一个 sched 上下文，不存在互相 resume 的链——第一版对称 inline 调度形成循环 ret 链（ping-pong 一方死亡后另一方 ret 指向死栈，「dead vthread resumed」实测），专用上下文从结构上消灭环。run queue 是 PyAdamasList 借用指针（每 VT 一份 spawn→死亡的调度器引用保活），sleep 走 intrusive 定时器链（sched 空闲 nanosleep 到最早 deadline 批量唤醒）。切换另修三个实测坑：sp 存 rsp+8（jmp 恢复不弹返回地址，存原始 rsp 每次恢复低 8 字节踩坏调用者 canary）；唤醒返回路径统一 rq_discard(self)（幽灵 run queue 条目 → 自切换）；还栈推迟到 sched 循环（池满 munmap 若在将死栈上执行，10k 死亡潮踩空）。root temp 数组退出时所有权收进 root 再清全局指针（否则 vt_destroy + pa_rc_shutdown 双重释放）。
- **栈水位全局随切换 save/load（§1）**：pa_frame_top / handler_top / pa_eh_depth / temp 三件套（**每 vthread 独立 temp 数组**，语句内阻塞 temp_len 可非零，共享数组会互相覆盖）/ 异常上下文节点链（rt_exc.c 新增 pa_exc_ctx_save/load accessor）/ exc_save_top / current_exc / current_exc_inst / pending_exc_args / pending_exc。
- **GC 跨栈策略（§4）**：GC 只在 vthread 切片间跑（切换点全局水位干净）。gc_collect 根扫描阶段调 pa_vt_gc_scan（major 必扫，minor 与既有保守根同门禁）：挂起栈按保存的帧链头走 pa_gc_scan_frames_from（rt_core.c 导出，与 gc_scan_frames 同一份 scan_mask 精确逻辑——**比逐字保守探针更精确**，帧记录物理上还在未退役的 mmap 栈上）+ 每 VT temp 数组逐字 pa_gc_mark_word；callee-saved 寄存器/fn 闭包/chan 寄存值在 VT payload 里由既有 OBJ 保守扫描覆盖（误钉不精确性文档写明）；VT 经 all_next 链保守互钉，生命周期仍归 RC。channel 缓冲是 PyAdamasList（LIST 精确扫描），无新增扫描逻辑。225 用例：只被挂起 vthread 持有的 list[str] + 自环 list 经 40000 轮 churn 强制多次 major GC 后存活且内容正确。
- **语法薄切片（§2/§6）**：go 软关键字（只有 go NAME… 才解析为 spawn，go=1/go()/go[0] 仍是普通名字），可作表达式或裸语句；ExprKind::Go + TyKind::VThread/Chan（枚举尾追加）。被 spawn 函数编译期校验：用户定义、签名已知、非 generator/coroutine/asyncgen、无 *args/**kwargs、无关键字实参、实参逐个显式给出（默认值省略暂不支持），返回值丢弃（Go 语义）。builtin join/sleep(ms)/make_chan(T,cap)（T=int/float/str/unknown，第 0 参编译期类型名特殊形式）；chan.send/recv 方法表。元素类型：int/bool/float 按裸机器字（float 走位模式 ChanSendF/RecvF i1f1 组）、str 按已知堆引用、unknown 按注册表探针（整数安全）；send 接管一份引用、recv 交出一份引用（容器所有权纪律同构）。降级 src/lower_vthread.cpp：go 合成包装闭包 __pa_go_N（build_lru_wrapper 同款形态，cell=[f,args…]，体调 f 丢弃返回值），VtSpawn 接管闭包引用；包装闭包是真实 pyadamas 函数，ABI/帧记录/异常区域全由既有 codegen 处理。入口退出 release_globals 前插 VtShutdown：**确定性 drain**（跑完 runnable/sleeping 的 vthread、放弃永久阻塞者、打印未 join 异常），与 Go「main 返回即杀」不同，方便 expect 钉输出。死锁全阻塞且无定时器 → panic: deadlock exit 1（Go 口径）。跨模块 handle/chan 签名按 unknown 序列化（与 Popen/datetime/dylib 同款口径）。
- **异常双路径（§2/§3）**：跳板 pa_vt_body_invoke（asm 外壳 + catch-all LSDA，与既有 personality/方案 G 帧清理兼容）接零开销 _Unwind 逃出；pa_vt_invoke_inner 的 setjmp 接 longjmp 路径。vthread 内逃出的异常存 handle，join 克隆重抛（先克隆再 dec 等待引用，避免级联释放 exc 本体后读悬空）；自己 try/except 接住的不传播；未 join 死亡异常在 handle 释放/退出 drain 时打印 stderr。ASan 纤维注解：切换包装 __sanitizer_start/finish_switch_fiber（仅 __SANITIZE_ADDRESS__），新生栈入口 finish(NULL)，root 主栈区间 pthread_getattr_np 捕获——警告消失，毒化记账正确。
- **实测（设计文档 §7，分支侧 Debug 编译器 -O2 产物，Xeon Gold 6252 @ 2.10GHz，容器 cpuset）**：无缓冲握手往返 **~150–300 ns/round**（100k 轮乒乓，每轮 2 切换 + 2 chan 操作；负载敏感，空闲 152/155、负载下 291/399）；spawn+join **~1.0 µs/对**（20k 空函数，栈池复用路径）；缓冲 channel **~69 ns/item**（cap 64，300k int）；**10k 并发阻塞 vthread：RSS ≈ 5.0 KB/个（合计 ~49MB），VSZ ≈ 2.5GB**（10k×260KB mmap 惰性触达），唤醒收尾后 RSS ~14MB（栈还池/munmap）。量级与 Go goroutine/channel 同级；切换 asm 本身 ~20–50ns，其余是调度器 C 路径 + 水位 save/load + chan 所有权操作。
- **限制（先验，设计文档 §8）**：**调度器重入不支持**——调度器栈上的 RC 析构级联若跑用户代码且再阻塞会重入调度器（与 asyncio 同款边界）；**M=1 无多核**（并发不并行，CPU 密集无加速）；**协作饥饿**（纯标量死循环不让出，饿死全部其他 vthread，无异步抢占）；固定栈深（溢出 SIGSEGV 撞 guard page，无信号恢复/增长）；无 select/close/超时 recv；缓冲 FIFO take_at(0) O(n)；chan 丢弃时阻塞者永久等待（退出按放弃处理）；go 形态限制见语法条；Windows 运行期 panic 桩（mmap/LSDA 都是 SysV 口径）；VT payload 保守扫描可能误钉恰好等于堆地址的标量字（既有 OBJ 同款哲学，挂起栈帧链扫描本身精确）；PYADAMAS_GC_STATS 分裂根统计不含挂起栈部分（诊断计数偏小，不影响正确性）。
- **后续（设计文档 §9，记录绕行不做）**：M>1 隔离堆、prange 数据并行、select/close/超时、异步抢占、可增长栈、async 改基到 vthread 调度器、退出语义开关。
- **验证（主树合后复跑，Release build/lead）**：cmake --build build/lead -j32 全 235 目标通过；220–229（vthread）/212–215（cffi）/207–211（桥）单跑 O0/O2 共 **38/38 ran ok**（rc=0，expect 段一致，O0==O2）；差分 **226/0**；普通与 ASan 套件各 **244 ran ok / EXIT=0**（234+10：227 run + 15 modules + cdecl:sample + cfi:unwind；180_codepoint_perf 是墙钟斜率验收用例，多租户机器负载 ~33 下默认 48 并发两次触发负载敏感失败，单跑与 PYADAMAS_TEST_JOBS=8 低并发复跑均过——与 vthread 分支同款纪律，非本轮回归）；ASan detect_leaks=1 抽查 224（join 异常）/225（挂起栈 GC）/226（churn）+ 13_gc 全干净；abbench 对 3e94c26 realistic 绑核交替 15 轮（BURST=5）go/chan 未使用，合计 **1262.14→1269.67ms（+0.6%）**——obj_dispatch +0.3% / queue_sim +0.5% / rec_memo +0.3% / sort_struct +0.1% / str_build +0.2% 均在 ±0.5% 内，dict_wordfreq 266.65→271.46ms（+1.8%，p25 +1.3%）超噪声带，按纪律 perf stat idq.dsb_uops/mite 诊断：**该负载指令数强 ASLR 布局敏感**（ASLR 开时基线 5 轮指令 1446–1592M 极差 10.1%、当前 1532–1607M 极差 4.9%，PYADAMAS_GC_DISABLE=1 仍在——既有负载的堆布局敏感路径，非本轮引入），setarch -R 确定性布局下指令 1591.0M→1547.9M（**−2.7%**）、cycles −2.0%、idq.dsb_uops −4.2% / idq.mite_uops +0.2%，绑核交替 7 轮（BURST=5）计时 280.23→274.91ms（**−1.9%**，p25 −2.0%）；objdump 逐函数对比热函数（dict_lookup/count_words/主循环/pa_dict_bump）逐字节相同，新增执行路径仅入口退出 pa_vt_shutdown 一次调用与每 GC 一次 pa_vt_gc_scan 调用（无 vthread 时空链早退），其余为 rt_core.c 增大后 GCC 对 GC 扫描助手的重内联（gc_scan_masked outline）/寄存器重分配与对齐 nop——判 +1.8% 为 ASLR 布局抽样噪声，**中性、无 DSB 布局回归亦无算法回归**；规模检查仍只有原有 5 处超限（codegen_regalloc.cpp 2475 / driver_build.cpp 1438 / ir_ops.h 1257→1276 / lower_impl.h 1238→1242 / typecheck_types.h 1310→1329，无新增超限文件；新文件 rt_vthread.c 937 行 / lower_vthread.cpp 200 行，codegen_call_rt.cpp 1099<1200）。
- **既有问题（不在本轮修）**：Debug 构建 118_suspend_expr / 154_nested_await 编译器侧 STL 越界断言崩溃（SIGABRT exit 134），0.304/0.305 已记录、base 同款复现，与 vthread/cffi/桥均无关，Release 口径不触发（建议单独立项）。

## [0.306] - 2026-11-19

### cdll 改名 dylib（平台中性命名）

- **理据**：DLL 是 Windows 术语（Linux .so / macOS .dylib），本功能是 dlopen/LoadLibrary 的跨平台 facade，cdll 让人误以为只管 Windows。ctypes 的 cdll 本意是调用约定命名空间（cdecl vs windll/stdcall），本项目无此区分，借来的名字连原始理据都不成立。用户拍板 dylib。
- **范围**：纯机械重命名，零语义变化——TyKind::CDLL→DYLIB、RtFn::CDLLLoad/Close→DYLIBLoad/Close、pa_cdll_*→pa_dylib_*、t_cdll→t_dylib、kModuleTable 条目、用户可见字符串（dylib.load: cannot open / in dylib handle / <dylib handle> / Windows NotImplementedError 文案）、tests/cdll/→tests/dylib/、213_cdll.py→213_dylib.py、214 错误路径 expect 同步、docs/cffi-design.md 示例与标识符同步（顶部加改名注记）。不留 cdll 别名（0.305 刚落地，无外部用户）。0.305 条目作为历史记录保留（其中 cdll 字样是当时命名）。
- **验证**：212–215 O2 单跑 OK；差分 216/0；普通与 ASan 套件各 234 ran ok / EXIT=0；abbench 对 f5d57b8 realistic 绑核交替 15 轮中性（合计 1262.59→1261.37ms，−0.1%，六项 ±0.3% 内）；规模检查无新增超限。

## [0.305] - 2026-11-19

### C FFI 免写直调与动态库（from c import / cdll.load，合入 cffi）

- **集成**：把 cffi 分支（9036fce，基于 f0a4af4/0.303）合入 0.304。三件：① 免写声明直调 C 函数（`from c import`，零 cdecl/.cpya）；② dlopen/dlsym 动态库（`cdll.load` 句柄 + 属性调用）；③ 既有 cimport/cdecl 健壮化（C float 单精度编组 + 变参 %al 钳制）。c/cdll 是独立 Native 伪模块，与 CPython 桥（python 伪模块）语义互不相交，测试编号 212+ 已避让 207–211。设计文档随分支合入保留：docs/cffi-design.md。
- **审计（设计文档 §1）**：既有管线 cimport→手写 .cpya 文本声明→FuncInfo(is_extern)→lower_cimport_call 逐实参 coerce→CallExtern→gen_call_extern SysV 分类（6 int/8 xmm/栈溢出/结构体 chunk/隐藏 sret/回调跳板/变参 %al）→链接期绑定；cdecl 把 C 头文件子集转 .cpya 走同一管线。审计确认已支持：CKind 标量/结构体按值/回调跳板/变参/extern var/ptr[T] 解引用/str NUL 终止不变量。本轮挑两个便宜洞修（C float 参数/返回整体跳过、变参 %al 未钳制）+ 两件新能力（免写直调、dlopen）；深坑（宏展开/回调重入/float32 字段等）记录绕行。
- **冲突解法（两边语义都保留）**：仅两处文本冲突，均为同位追加——CMakeLists.txt 的 PYADAMAS_RT_PARTS 桥加 cpy、cffi 加 cffi，合成 `… diag cpy cffi`（同一列表喂 rt.o 与 rt_asan.o）；driver_build.cpp kModuleTable 桥登记 python、cffi 登记 c/cdll，三条目并存。其余自动合并干净、两侧叠加：builtin_sigs.cpp（python/python.* 的 cpy_proxy 开集分支 + c 空 globals/cdll load/close 签名）、driver_build.cpp link_program（-lm → -lm -ldl，glibc 2.34+ 已并入 libc、-ldl 是空壳旧 glibc 仍需要）与 program_module/check_load 的 python.<module> 开集子代理并存、ir_ops.h（RtFn::CpyCall 与 CDLLLoad/CDLLClose 不同位，rt_name 是 switch 无索引冲突）、codegen_call_rt.cpp（CpyCall 归 i4 组、CDLL 归指针组）、rt_internal.h/lower_import.cpp/typecheck_stmt_forms.cpp/typecheck_call_forms.cpp/lower_method.cpp 各分支两侧叠加。
- **语法语义（设计文档 §3）**：`from c import sqrt, strlen`（可 as 别名，C 侧符号名不变）绑定默认库符号，符号名集合编译期不可知，每名建 FuncInfo{c_lazy,c_symbol}；调用点（check_c_lazy）拒绝关键字/`*` 展开、求实参类型、推断返回 ABI；降级（lower_c_lazy.cpp）按 (符号,绑定方式,返回种类) 去重登记 IRExtern{lazy=1} 发 CallExtern；首调 dlsym(RTLD_DEFAULT,name) 缓存到静态槽 .Lcsym<N>，此后只多 load+test+jne 与间接 call。`cdll.load(path)` → dlopen(RTLD_NOW|RTLD_LOCAL) 句柄（TyKind::CDLL，标量整数地址，非堆无 RC，可存容器/传参，print `<cdll handle>`，跨模块 enc_type→unknown）；句柄属性调用 lib.sym(...) 与免写路径共享 lower_c_lazy_call/CallExtern/gen_call_extern，lazy=2 句柄值在 Inst::a，逐次 dlsym(handle,name)（原型不做缓存）。错误路径统一 OSError 点名符号/路径（库不存在/默认符号不存在/句柄符号不存在；dlsym 的 NULL 用 dlerror 区分符号值合法为 0 的边界）。`from c import *` 拒绝；`import c; c.sqrt(...)` 不支持（报错指引 from-import）；c_lazy 函数不能当值。
- **ABI 契约（设计文档 §4）**：实参推断 float→double(xmm)/int,bool→i64/str→char*(data+24，NUL 终止不变量)/bytes→uint8_t*/list[float|int]→double*|i64*(data+16)/Optional[str]→可空 char*/Optional[int]→void*；其它/unknown build 期报错（cannot infer C ABI）。返回默认：有 float 实参→double 否则 i64（ctypes restype=c_int 同构契约）；**无标注时指针返回按 i64 读出垃圾值**（ctypes 同契约，213 用标注演示正确姿势）。标量标注覆盖返回 ABI：AnnAssign 与 return 标注（float/int/str/bool/None/Optional[str|int]），求值后覆写 expr_types_[call]。免写路径不做：C float 单精度/结构体按值/变参/回调（需 cimport .cpya）；float 实参永远按 double 传（调 C float 函数用 float32 声明，否则 ABI 错）。SysV 6 int/8 xmm 溢出落栈沿用；免写新增 fn_tmp 暂存（out_base+stack_bytes+48），outgoing_bytes 上界 48→56。
- **修复（设计文档 §5）**：① CKind::Float32（ckind_width=4，.cpya 类型名 float32，`float` 仍是 double 兼容不变，PyAdamas 侧统一 double）：参数 cvtsd2ss（寄存器/栈两路）、返回 cvtss2sd、extern var movss 加载/存储、ptr[float32] 按 fwidth=4 解引用（写回 cvtsd2ss+movss）；cdecl 的 C float 参数/返回从整体跳过函数改为生成 float32（sample.h scalef/global_ratiof 端到端）。刻意边界：float32 结构体字段（cfield_layout 显式报错）、含 float32 回调签名（callback_kind_ok 拒绝，cdecl 跳过并警告）——字段访问宽度/跳板单精度提升深度超出本轮。变参里的 float 仍按 double（C 默认实参提升，语义正确）。② 变参 %al 钳制：fi>8（浮点实参溢出到栈）时 %al=8（ABI 上限），原样塞 fi 会让 va_arg 寄存器保存区越界读；111_varargs 回归通过。
- **实测（设计文档 §6，分支侧 Debug 编译器，生成程序 O0/O2 均验证）**：212_c_lazy sqrt(2.0)=1.4142135623730951（O0==O2，别名/标注/函数返回标注/循环内缓存槽均正确）；213_cdll 现场 cc -shared -fPIC -O2 建 probe.so，probe_add(2,3)=5/probe_scale(4.0)=10.0/int+str 混合 probe_mix(10,"hello")=15/str 标注 probe_greet="hi"/close=0；214 三类错误消息 O0==O2；215 f32_mul(1.5,2.25)=3.375/f32_of_int(7)=3.5/roundtrip(π)=3.1415927410125732（单精度舍入可见）/float32 全局变量/ptr[float32] 读写。分支口径全套件 227 ran ok（基线 223+新 4，较主树同基线口径少 2，系 worktree 环境口径差；主树合后实际 234，构成见验证），与 base 逐条 diff 仅新增用例、零回归，fail/strict/cdecl/cfi 全过；ASan 212–215 与既有 FFI 用例（97/98/101/103/104/107–113）全过。性能注记（原型未优化）：lazy=1 首调多一次 dlsym（含 dlerror 清/查），此后 load+test+jne+间接 call（vs 链接期直 call）；lazy=2 逐次 dlsym（热路径 (handle,symbol) 缓存后续）；未做 PLT 式惰性绑定。
- **验证（主树合后复跑，Release build/lead）**：cmake --build build/lead -j32 全 223 目标通过；212–215（cffi）与 207–211（桥）单跑 O0/O2 共 18/18 ran ok（rc=0，expect 段一致）；差分 **216/0**；普通与 ASan 套件各 **234** ran ok / EXIT=0（230+4：217 run + 15 modules + cdecl:sample + cfi:unwind，OpenBLAS 在场无跳过；f27 关键字实参/s04 unknown 实参正确拒绝）；ASan detect_leaks=1 抽查 212–215 / 207–211 / 13_gc 全干净；abbench 对 1e47057 realistic 绑核交替 15 轮（BURST=5）桥/cffi 未使用呈中性——dict_wordfreq 265.80→268.37ms（+1.0%，p25 −0.3%）、obj_dispatch −0.1%、queue_sim 158.15→157.23ms（−0.6%）、rec_memo +0.1%、sort_struct +0.2%、str_build −0.4%，合计 **1259.49→1260.51ms（+0.1%）**，六项均在 ±1.0% 噪声带内（p25 均在 ±0.3% 内）、无回归（rt_cffi.c 并入 rt.o 未引发可测 DSB 布局回归）；规模检查仍只有原有 5 处超限（codegen_regalloc.cpp 2475 / driver_build.cpp 1438 / ir_ops.h 1257 / lower_impl.h 1238 / typecheck_types.h 1310，无新增超限文件；codegen_call.cpp 1132<1200）。
- **限制（先验，设计文档 §7）**：免写直调=任意符号调用，ABI 不符会**静默算错**（ctypes 同契约，生产代码建议 cimport 显式声明）；回调槽不可重入（既有，免写路径不接回调绕开）；cdecl 不展开函数式宏/条件编译（既有）；float32 结构体字段/含 float 回调显式拒绝；c_lazy 函数不能当值（需捕获句柄的闭包+统一约定 shim，后续）；lazy=2 逐次 dlsym、句柄无自动 dlclose（泄漏到进程退出，ctypes 同语义）、RTLD_GLOBAL/flags 未暴露；Windows rt_cffi.c 给 NotImplementedError 桩（LoadLibrary/GetProcAddress+Win64 调用约定后续）；CDLL 跨模块按 unknown 序列化会丢类型；标注覆盖只及 AnnAssign/return（float(sqrt(2)) 转换包装、容器元素标注不传播，推断默认兜底）。
- **既有问题（不在本轮修）**：Debug 构建 118_suspend_expr / 154_nested_await 编译器侧 vreg_nonneg STL 越界断言崩溃（SIGABRT exit 134），base f0a4af4 独立 worktree 复现完全相同，与 FFI/桥均无关，Release 口径不触发（与 0.304 同条，建议单独立项）。

## [0.304] - 2026-11-19

### CPython 互操作先验（from python import 子进程 IPC，合入 cpybridge）

- **集成**：把 cpybridge 分支（9c19e77，基于 0.302）合入 0.303。`from python import <module> [as alias]` 把属性调用降级成一次 CPython 子进程 IPC（首次调用懒拉起 python3 worker，带标签 JSON 帧，全进程一个 worker 复用），结果回到 pyadamas 原生值域（unknown）。不嵌入 libpython：无 GIL 互嵌、无 Py_INCREF/DECREF 跨边界所有权、无解释器初始化与主程序 RC/GC 耦合。lexer/parser 零改动（from python import 本就是合法 ImportFrom），语义全在 typecheck/lower 的模块分支。点子模块链调用（cpyos.path.basename）编译期拼成完整模块名仍是一条 IPC。设计文档随分支合入保留：docs/cpybridge-design.md。
- **冲突解法（两边语义都保留）**：文本自动合并干净——ir.cpp 两级索引分析（needs_unknown_index 与 can_skip_index_maint/index_readonly）拉黑 CpyCall/RcTempUnknown；rt_core.c pa_print_any 改用共享探针 pa_unknown_is_heap + CPYFLOAT/LIST/DICT 分派，与 0.303 的 pa_rc_track、rc_index_off 非 static 并存；rt_internal.h（PYADAMAS_RC_CPYFLOAT=8 + pa_cpy_call/pa_print_cpy_value 声明）、ir_ops.h（RtFn::CpyCall + rt_name）、codegen_call_rt.cpp（CpyCall 归 i4 组）、CMakeLists.txt（rt_cpy.c + codegen_index.cpp + lower_cpy.cpp）、typecheck hoist 对 from python import 跳过 bind_alias、lower scan_globals 不占全局槽，均两侧叠加。**唯一语义冲突在运行时交互**：0.303 给 pa_list_append 加了种类维护（kind0 表 append 堆值会升级 kind2），而桥的实参打包是 kind0 借用表（元素引用仍归原主，销毁不 dec）。itertools pack_seqs 传 heap=2 之所以无恙，是那些模块走 index_readonly（探针恒 0、不升级）；桥模块被两级分析强制保留 unknown 索引（CpyCall 拉黑），探针真命中 → 借用表升级 kind2 → 销毁按注册表 dec 借来的实参引用 → 过度释放（208 实测「refcount decrement on a freed object」，C harness 复现 kind0→2 升级后源表 rc 1→0）。修：lower_cpy.cpp 实参 append 的 heap 恒传 0（kind==heap 跳过升级，保持 kind0 借用语义）；marshalling 由描述符驱动（pa_cpy_call 读 args->data[i]+descs），与 elem_kind 无关，故 heap=0 不影响序列化。0.303 运行时种类维护语义不动。
- **延迟（设计文档 §6 实测，Debug 编译器 -O2 产物，python3 3.14.4，N=3000 三轮）**：轻调用 IPC 地板 ~25µs——os.getpid 26.2/25.5/25.5µs（原生 0.59µs ~44×，CPython 进程内 0.36µs ~70×）；math.sqrt(2.0) 26.8/30.0/28.8µs（原生内联 sqrtsd ~10^4×）；json.dumps(100 元素 list) 103/130/122µs（原生 8.1–13.1µs ~10–13×，CPython 进程内 8.3µs）；首次调用含 spawn 36.5/39.7/38.0ms。量化口径 ~25µs/次 ⇒ 单 worker 上限 ~4 万次调用/秒，payload 越大传输占比越高（bytes 走 base64 +33%）。适用边界：粗粒度库调用（一次干 ≥ms 级的活）或批量数据一次过桥（convert a batch once, work natively）；不适合热循环逐元素、把桥当数学库（原生 math 快 10^4×）；短命进程（<100 次调用）摊不平 spawn。
- **限制（先验，设计文档 §8，均有编译期/运行期明确报错）**：kwargs 不支持（协议只带位置参数）；handle 代理（文件句柄/迭代器/生成器/自定义实例/模块/函数）拒绝 TypeError；模块级 from python import name 与裸属性读（math.pi/sys.version）不支持（只支持调用）；结果类型永远 unknown（float 盒已解决打印，unknown 算术/下标/dict 键查找仍受限——下标当 list、bin.add 当整数加）；无超时（worker 卡死挂住调用方）；单 worker 串行（无池/并发，asyncio 需后续）；生成器/async 不能过桥；Windows 报 OSError（CreateProcess 路径后续补）；int64 边界（CPython int 超 int64 → OverflowError，float NaN/Inf 全链路支持）；dict 非 str 键按指针哈希（float 盒/容器键等值查不到）；用了桥的模块强制保留 unknown 索引（放弃 exact-RC 优化）；worker 崩溃当次报 RuntimeError、下次重拉干净 worker（模块状态丢失）；每调用 malloc DOM+帧缓冲不池化。unknown 值域固有歧义：None/bool 进 unknown 与 int 不可分（回程 None→0、bool→0/1，print 出 0/1 而非 None/True，静态类型已知位置不受影响）；tuple 回程打印 [..] 而非 (..)（运行期无 tuple 标记）。异常按 type(e).__mro__ 名表映射到 pyadamas 内置名（JSONDecodeError→ValueError），全不命中→RuntimeError 带 CPython 原名。
- **验证**：207–211（cpy）与 206（store kind）O0/O2 对拍一致（cpy 用例按 expect 段，该段钉自真实 CPython 输出）；差分 **212/0**；普通与 ASan 套件各 **230** ran ok / EXIT=0；ASan detect_leaks=1 抽查 207–211 / 13_gc / 206 全干净；abbench 对 f0a4af4 realistic 绑核交替 15 轮（BURST=5）桥未使用呈中性——dict_wordfreq 268.53→267.09ms（−0.5%）、obj_dispatch −0.1%、queue_sim 159.73→158.12ms（−1.0%）、rec_memo −0.1%、sort_struct −0.4%、str_build −0.1%，合计 **1265.08→1260.79ms（−0.3%）**，六项均在 ±1.0% 噪声带内、无回归（rt_cpy.c 并入 rt.o 未引发可测 DSB 布局回归，无需 perf stat 诊断）；规模检查仍只有原有 5 处超限（codegen_regalloc.cpp 2475 / driver_build.cpp 1432 / ir_ops.h 1253 / lower_impl.h 1236 / typecheck_types.h 1297，无新增超限文件）。
- **既有问题（不在本轮修）**：Debug 构建 118_suspend_expr / 154_nested_await 编译器侧 std::vector<unsigned char> 越界断言崩溃（SIGABRT exit 134），base c467230 独立快照 Debug 构建复现完全相同崩溃，与桥无关，Release 口径不触发（设计文档 §9，建议单独立项）。

## [0.303] - 2026-11-19

### 下标写入维护列表种类元数据（修 unknown 静态类型写入的 SIGSEGV）

- **根因**：静态容器类型是 unknown 时，降级器 store_list_elem 只 retain 不调 pa_list_prep（lk==2 分支），运行期 elem_kind 可能与新值冲突。gen_index_store（O1+ 内联）与 pa_list_set（O0/冷路）写入时都不维护种类/位图：kind1 列表存进标量后种类不升级，销毁 pa_rc_dec_heap_batch 把标量当堆指针 dec → SIGSEGV（/tmp/kind1_repro.py，CPython 正常）；kind2 带位图的混合表（如 [1,"x",3]）覆盖后位图陈旧，销毁按旧位 dec 同样崩（0.271 oracle 即崩，属既有洞）。kind0 存堆值则漏释放/漏 GC 出边。
- **运行时（rt_list.c）**：pa_list_set / pa_list_insert 增加第 4 参 heap（新值静态种类 0/1/2，与 append 同口径），写入时经共享的 pa_list_store_kind 维护：kind 冲突升级 2；kind2 带位图按新值堆性更新第 i 位；kind2 无位图无需动作（销毁走注册表）。**heap==2 才探针**（pa_unknown_is_heap，与 dec_unknown 逐位一致含 exact-RC 回退）——静态已知标量绝不探针：大整数/浮点位模式可能恰好等于存活堆地址，探针会误判（206 用 2^46+12345 在 ASan 下覆盖）。旧值仍按**写入前**的 kind 释放，新值 retain 仍由降级器负责，所有权语义不变。pa_list_prep 的旧 list_probe（只查 rc_index，exact-RC 下恒 0 会漏升级）换成共享探针；探针包装加常规模式内联快路（rc_index_off 改 extern），slow 只服务 exact-RC 回退，prep 热路径与 0.302 同形态。pa_list_append 同族补种类维护（kind3 定型/冲突升级），热路径（kind0+0/kind1+1/kind2）只多一次 kind 装载+两次比较。
- **发射（codegen_index.cpp，新文件）**：gen_index_store / gen_index_store_unchecked 从 codegen_call.cpp 拆出（原文件 1188 行贴近 1200 硬限；codegen_call_rt.cpp 余量也不足，按 §7.1 放新文件）。O0 传 heap 第 4 参；O1+ 内联按新值静态堆性分流：kind0+标量（含 0.298 的 list[float] 快路径，逐字节保留）几条指令跳过；冲突种类/unknown 探针交运行时 pa_list_set；kind2 位图用 bts/btr 内存位操作按静态堆性更新（unknown 新值+kind2 无位图仍纯内联，不必探针）。gen_append_inline 增加「运行期 kind==静态 heap」守卫（heap 是编译期常量，按立即数比较，热路径多一条 cmp+jne；heap==2 恒落回）。RtFn::ListSet/ListInsert 改 4 参（i4 组），datetime.replace 与 list.insert 降级点补 heap 实参。
- **同族审计（切片/extend/乘法/set_len 及重排路径）**：pa_list_slice_assign 已有 kind 合并，但移动/覆盖元素后位图陈旧 → kind2 一律 pa_list_drop_bits 回退注册表（源空表收缩也成立），0→2 升级补 GC 链；insert/del_index/reverse/sort/random.shuffle 同族丢位图（del 另补 kind3→2 防御映射）；copy_range 快路径增加「源运行期种类==静态 heap」守卫（unknown 别名可让 static list[str] 装 kind2 混合表，memcpy+批量 inc 会把标量当堆指针）；extend/reversed/copy 经 prep 已覆盖；set_len 只截断、按 kind 释放，无洞；repeat/slice 新建表种类随源，无洞。运行时内部 append 调用点（dict keys/values/items、itertools、most_common、accumulate、str/bytes/range_to_list、list slice/repeat）把 heap=2 改成准确的运行期种类提示，避免种类维护引入逐元素探针（items 的混合元组因此获得精确位图）。
- **GC link 语义（kind0→2 升级）**：kind0 小元组是 untracked 分配（不进 GC 堆链/索引/新生代）。升级成 kind2 后有了堆出边、可能成环，新增 pa_rc_track（rt_core.c）：清 UNTRACKED 位再 rc_link，与 tracked 分配同形态；young_add 不改 rc、gc_alloc_black 是占位空操作，存活对象计数不受影响；新入链对象不在本轮 gc_set 快照里，增量标记期入链也安全（与新生对象同一论证）。prep/store_kind/append/slice_assign 的 0→2 升级点统一调用。30000 轮「元组经 unknown 升级后自环」ASan detect_leaks=1 干净（环真收集）。
- **实测（对 c467230 绑核交替 15 轮，BURST=5）**：realistic 六项 dict_wordfreq 262.18→266.10ms（+1.5%，p25 +1.0%）、obj_dispatch +0.1%、queue_sim 156.51→159.51ms（+1.9%）、rec_memo 232.65→236.89ms（+1.8%）、sort_struct +0.4%、str_build 141.20→144.00ms（+2.0%，p25 +1.6%），合计 1243.32→1257.97ms（**+1.2%**）；**nbody 39.48→39.42ms（−0.1%）、spectral_norm 60.56→60.58ms（+0.0%）、matmul 19.66→19.26ms（−2.0%）**，三项合计 −0.4%，浮点热路径无回归（kind0+标量守卫与 0.298 快路径逐字节保留）。
- **回归诊断与修复（本轮三次迭代）**：初版 str_build +5.9% / dict_wordfreq +4.7%，逐一定位修掉：① append 内联守卫写了「kind!=2 + kind==heap」两条比较，而 str_build 的 parts.append 推成 list[str]（heap=1），每 append 多 2 指令（8M 次 +1.4% 指令）——融合成单条 cmp $heap（kind2/3 与冲突种类一并排除），与 0.282 守卫同指令数，asm 对比只剩立即数差异；② items/most_common 的混合元组按 own 传 0/1 提示，kind2 元组首元素触发位图分配，每对一次 alloc/free churn——tk==2 改回 heap2（不建位图，注册表销毁），tk 0/1 传准确种类免探针；③ prep 的无条件「升级+track」写法让 gcc 给整函数加 push/spill/setne 序列——track 移进冲突冷分支，kind2 直接跳过探针（种类恒 2，探针本就冗余）；④ 共享探针经 slow 函数多一层 call——头文件包装加常规模式快路（rc_index_has 命中即返回，rc_index_off 改 extern 供判断），exact-RC 才进 slow 回退，判定逐位一致。修后 str_build 指令 +0.1%（逐位级持平）。剩余 +1.2% 是 **DSB 布局**：rec_memo 不执行任何改动代码，perf stat 指令 +0.15%（1803.1M→1805.8M）、cycles +1.9%、idq.dsb_uops −8.9%（1230M→1121M）、idq.mite_uops +19.4%（565M→675M），与 0.301 rec_memo +1.4% / 0.302 sort_struct +2.5% 同族（权衡接受，未放宽守卫）；queue_sim 指令 +0.9%、DSB −2.3%/MITE +7.0% 同族。
- **验证**：新增 206 覆盖 kind1 存标量（原始复现同族）/kind0 存堆值/unknown 探针双向/kind2 位图堆位↔标量位互换（含 63/64/65/128/129 跨字边界）/浮点 kind1 升级与 kind0 float 保持/大整数静态标量不探针/insert/del/pop/reverse/clear/切片增长收缩/extend/append/kind3 定型/sort/shuffle/churn 升级销毁循环/kind0 与 kind1 自环 GC。读回走 unknown 通道（静态元素类型特化是既有语义，不在本轮范围）。O0/O1/O2 对拍 CPython 一致，0.271 oracle 在 206 上 SIGSEGV（rc 139 vs 0，与 201/202/203 同属「oracle 早于用例」的预期 rc 差异）；ASan detect_leaks=1 干净。差分 207/0；0.271 原有语料零差异；普通与 ASan 套件各 **225** ran ok / EXIT=0；泄漏抽查 13_gc / 88_defaultdict / 192_dict_cache / 66_genexpr / 119_gen_except_suspend / 204 / 205 / 206 全干净；Win64 交叉汇编 nbody/spectral_norm/matmul 通过（206 与 13_gc/205 同样受既有扫描位图 .pushsection 段指令限制，未验证 Windows 运行）；规模检查仍只有原有 5 处超限（codegen_call.cpp 1188→1066，新文件 codegen_index.cpp 256 行）。
- **遗留（审计发现，未在本轮修）**：① 静态元素类型特化的读回（list[str] 经 unknown 存标量后按 str 读）仍按旧类型解释，属既有语义；② 混合字面量经切片赋值推断成 list[str] 时 MakeList 按 kind1 建表装标量（0.271 即崩，类型推断洞）；③ deque 的 set/extend 不维护种类（同族，独立容器）。

## [0.302] - 2026-11-19

### unknown dict 的标量-only 销毁跳过（rec_memo −12.8%，realistic 合计 −2.5%）

- **取证**：rec_memo 的模块级 `memo={}` IR 类型是 dict[unknown,unknown]（pa_dict_new 传 own=2,2），虽然实参形参都是 int，运行时所有权标志仍是 unknown —— 0.285 的标量销毁跳过（key_own==0 && val_own==0）从不命中。perf：dict_destroy 13.1%（逐 entry 调 pa_rc_dec_unknown）+ pa_rc_dec_unknown 族 4.7%，每轮 25 个 entry 白走链游 + 探测。
- **发射（运行时-only）**：PyAdamasDict 尾部追加 sticky 标志 saw_heap（既有字段偏移不动；grep 核实 src/ 无 PyAdamasDict 硬编码尺寸/尾部偏移，codegen 只按偏移 0 读 len；sizeof 136→144 由 sizeof 表达式自动跟随，dict 池路由不受影响）。own==2 的 entry 写入路径探测键/值是否堆指针，是则置 1（只置不清；clear/new 归零）。dict_destroy / dict_gc_release / pa_dict_clear 在 (key_own==2 && val_own==2 && !saw_heap) 时跳过链游 —— 从未存过堆值，全部 entry 是标量，无 RC 可做。0.285 的 (0,0) 跳过保留。
- **共享探针（安全核心）**：rt_core.c 暴露 pa_unknown_is_heap_slow + 头文件内联包装 pa_unknown_is_heap，与 pa_rc_dec_unknown 的「该不该 dec」判定**逐位一致**（常规模式 rc_index；exact-RC 模式对齐 + 堆链回退；阈值 <65536 与 inc/dec_unknown 同款 —— 取证描述里的 4096 是 list_probe 的阈值，未采用）。探针说 0 的值销毁时必然空操作，跳过不会漏释放。dec_unknown 改为共用该探针，不复制两份逻辑。内联包装先排除小整数，标量不进函数调用。
- **写入路径全枚举（grep entries 写入点逐一核对，rt_dict.c 外无 entry 写入）**：pa_dict_set 插入/覆盖（函数入口统一探针）；pa_dict_bump 命中直写 val（单独探针 nv）、未命中走 set；update/copy/from_pairs/set_from_list/defaultdict 缺省创建/class 属性表/stdlib/json/re 全部经 pa_dict_set。**update 的 1→2 升级洞**：dst 既有 entry 按旧 own==1 retain（不探针），升级后必须保守置 saw_heap=1（len>0 时），否则跳过链游漏释放；0→2 升级表里只有标量无需置位。copy 保守传播 src->saw_heap。覆盖/删除不减位（sticky 保守）。GC 标记（gc_scan_object）不读 saw_heap，照常逐 entry 扫描。
- **pa_dict_set 的标量快路**：saw_heap 已置位直接返回；标量只做 own 比较 + <65536 比较，**不写内存**（0.283 STLF 教训：不要每 set 必写标志形成循环携带 store→load 依赖）。
- **内联扰动修复（0.285 同款）**：共享探针让 unknown_is_heap 多了第二个调用点，gcc 把它从 dec_unknown 里 outline（objdump 证实多一层 call/ret）——加 always_inline 恢复紧形态（dec_unknown 内 0 处 call unknown_is_heap）。
- **实测（对 1ce2d21 绑核交替 15 轮，BURST=5）**：rec_memo **268.01 → 233.72 ms（−12.8%，p25 −12.8%）**；str_build −1.8%（p25 −2.1%）；dict_wordfreq +0.1%、obj_dispatch −0.0% 中性；queue_sim +0.9%（p25 +0.8%）；sort_struct **+2.5%**（p25 +2.4%）。六项合计 **1282.45 → 1249.84 ms（−2.5%）**。perf 证实机制命中：rec_memo 里 dict_destroy 11.1% + dec_unknown 族 4.7% 整体跌出 profile。
- **回归诊断（DSB 布局，非算法）**：sort_struct 不执行任何 dict 代码，perf stat 指令 455.208M → 455.198M（−0.002%，逐位级持平）、cycles +2.2%、idq.dsb_uops −12.7%（280.5M → 245.0M）、idq.mite_uops +19.3%（171.0M → 204.0M）—— rt_dict.c/rt_core.c 尺寸变化把热区 uop 挤出 DSB 窗口，与 0.282/0.289/0.301 同族（权衡接受，未放宽守卫）。queue_sim 无 dict，指令 +0.5%、cycles +0.24%（perf stat 单次）、DSB +4.8%/MITE −7.0%，噪声级布局代价。
- **验证**：新增 205 覆盖 {} 推 unknown 的标量 dict 销毁（含 fib(25)=75025 ≥65536 的大标量探针路径）、存堆值（str/list/dict 嵌套）后销毁真释放、堆→标量与标量→堆覆盖、update 的 (2,2)/(1,1)→(2,2)/(0,0)→(2,2) 三种升级、copy 传播、clear 归零后再探针、pop/del/setdefault/bump/from_pairs/set/defaultdict(list/int/str)、unknown dict 自环与标量 dict 经列表进 GC 环。O0/O1/O2 对拍 CPython 一致；ASan detect_leaks=1 干净（漏探针即泄漏，LSan 抓）。差分 206/0；0.271 原有语料零差异（201/202/203 为 0.299/0.300 新增，oracle 早于其语法）；普通与 ASan 套件各 **224** ran ok / EXIT=0；泄漏抽查 13_gc / 88_defaultdict / 192_dict_cache / 66_genexpr / 119_gen_except_suspend / 204 / 205 全干净；规模检查仍只有原有 5 处超限。

## [0.301] - 2026-11-19

### 列表缓冲池扩容 + 内联头池扩容（sort_struct −42.2%，realistic 合计 −6.1%）

- **取证**：sort_struct（3900 轮 × 500 元素 list[tuple[int,int,int]] 建表→sort→聚合→丢弃）perf 里 glibc malloc/free 合计约 27%。初判三个来源：list_buf_pool 只覆盖 <=512B（64 元素），500 元素缓冲 4000B 超档，增长链与销毁全走裸 malloc/realloc/free；kind-2 位图每次增长 realloc；排序临时区每轮 malloc((n/2+1)*8)。
- **发射（运行时-only，rt_list.c）**：① list_buf_pool 覆盖扩到 8192B（1024 元素），小档（<=512B）每档仍 64 块、大档每档 8 块防囤内存；请求档为空时按 128 位非空位图 O(1) 向上取第一个非空档（容量只会变大，cap_io 回写真实容量），找不到才 malloc。② list_buf_grow 池内尺寸先试池：get 新块 + memcpy 旧内容 + 旧外部块 put 归还（内联块不动）；>8192B 保留原 malloc/realloc fallback。③ heap_bits 位图入同一池（字数即 8 字节单位 cap）：增长复制旧字 [0,owords)、新字 [owords,bcap) 全清零（比旧实现清零范围只大不小），bits_cap 记池块真实字数；销毁/unknown 降级 put 代替 free。增长后位图扩容改按 l->cap（池可能把 cap 抬高到 > ncap，按 ncap 会漏扩）。④ timsort 临时区入池（n/2+1 元素）。**排序期间不执行用户代码**：plain sort 比较是运行时宏（LESS_STR 只调 pa_str_cmp）；sorted(key=) 的 key 在生成循环预算好（lower_builtin_seq.cpp），ListSortedBy 只收 keys 列表；list.sort(key=) 不支持（lower_method_containers.cpp 报错）。get/put 纯缓冲操作、无 RC/GC 副作用，即使重入也安全。临时区 OOM 改走运行时统一 panic（与 list_buf_grow 一致），不再退化为插入排序。
- **取证修正（关键）**：只做缓冲池化时 abbench sort_struct 仅 −2.0%。perf 复查发现 malloc 大头不在缓冲链而在**元组 RC 头块**：每轮 500 个元组同时存活，list_inline_pool 上限 64，每轮 ~436 个头块走裸 malloc（_int_malloc 9% 全在 rc_alloc_core）/free（_int_free 族 15% 全在 pa_rc_dec_heap_batch）。**内联头池上限 64 -> 512**（rt_core.c，512×176B≈90KB 缓存上限）后第二轮起头块零 malloc，perf 里 malloc/free 跌出前十（_int_malloc 0.15%）。缓冲池化本身确已生效（增长链 malloc 消失），只是边际占比小。
- **不变量保持**：GC 清扫（gc_free_body）不经池、直接 free data/位图（池块是普通 malloc 块，直接 free 合法）；shutdown drain 释放所有档（含非空位图清零）；池块复用不携带旧数据语义（data 只读 [0,len) 先写后读，位图按上文清零/复制）；bits_cap 独立记账语义保持；同池 dtor/release/kind/size 恒定、入池 rc 归零（0.276/0.280 不变量）不受影响。
- **实测（对 85cb1e5 绑核交替 15 轮，BURST=5）**：sort_struct **194.90 → 112.56 ms（−42.2%，p25 −42.4%）**；str_build **−2.7%**（p25 −2.6%）；dict_wordfreq −0.5%；obj_dispatch −0.1%；queue_sim +0.5%（p25 +0.4%，噪声级）；rec_memo **+1.4%**（p25 +1.4%）。六项合计 **1365.60 → 1282.32 ms（−6.1%）**。
- **rec_memo +1.4% 诊断（DSB 布局，非算法）**：perf stat 指令 −0.07%（1943.8M → 1942.5M，逐位级持平）、cycles +1.9%、idq.dsb_uops −2.1%（1253M → 1226M）、idq.mite_uops +4.6%（646M → 676M）——rt_list.c 尺寸变化把热区 uop 挤出 DSB 窗口、传统解码增多，与 0.289 的 rec_memo +1.8% 同族（权衡接受，未放宽任何守卫）。
- **验证**：新增 204 覆盖跨档增长/cap 抬高/位图复用后 kind-2 正确性（list[object] 混合表 + 清空再增长）/排序结果（int/float/str，0/1/2/64/65/500 边界）/sorted(key=) 稳定性/超大表（3000 元素）池外 fallback/extend 与切片赋值增长/GC 环列表（自环混合表 30000 轮）。O0/O1/O2 对拍 CPython 一致（元组排序按机器字序是既有语义，校验和取顺序无关量），ASan/LSan 通过。差分 205/0；0.271 原有语料零差异（201/202/203 是 0.299/0.300 新增用例，oracle 早于其语法，rc 1 vs 0 属预期）；普通与 ASan 套件各 **223** ran ok / EXIT=0；ASan detect_leaks=1 抽查 13_gc / 88_defaultdict / 192_dict_cache / 66_genexpr / 119_gen_except_suspend / 204 全部干净；规模检查仍只有原有 5 处超限。

## [0.300] - 2026-11-18

### 按实际绑定的实参推断类型（不新增语法）

- 调用确实省略形参时，局部可重查的函数或方法才用其字面量默认值推断；显式传参不会被未使用的默认值约束。保持 `None` 哨兵，不把它钉成形参类型；对无法安全共用浮点调用约定的默认值、实参与已知覆盖给出诊断。
- `*` 展开固定长度元组时按元素逐槽绑定，使用每个元素自己的类型，同时核对缺参、重复和多余实参。未知长度的展开不猜固定槽位；同名关键字可由 `**kwargs` 接收，不会占用仅位置形参。
- 验证：新增一项运行回归和六项诊断回归。新运行用例在 O0/O1/O2 与 CPython 输出一致，并通过 ASan/LSan；普通及 ASan 各 222 例通过，差分 204/0，CTest 3/3。0.271 原有语料无输出差异，无新增超限文件。

## [0.299] - 2026-11-18

### 将推断出的返回类型传播到调用方

- 模块内普通函数和方法的返回类型在形参回填后发生变化时，使用最终签名作为提示，在新作用域中按源码顺序重查。更新的不只是调用表达式，还包括外层运算、赋值和下游形参检查。只在可脱离旧作用域的标量或容器返回类型变化时重放；生成器专用路径保持不变。
- 修复方法的仅关键字形参写错位置，以及浮点优先调用绕过混合类型诊断的问题。无标注形参混传浮点和非浮点时两种调用顺序都报错；显式 `float` 标注仍允许整数转换。本轮不新增多态实例化、类型参数语法或跨模块泛型。
- 裸容器返回标注只填类型洞：保留构造器、显式 `Any` 与固定位置上的类型；裸可变容器不因推断出元素类型而被锁定。发现已存储的容器元素与固定浮点标注不一致时给出诊断，不把整数位模式当成浮点数读。
- 验证：新增两项运行回归和五项诊断回归。新运行用例在 O0/O1/O2 对拍 CPython，并通过 ASan/LSan；普通和 ASan 套件各 221 例通过，差分 203/0，CTest 3/3。0.271 原有语料输出无差异，与 0.298 相比 15 项基准的 O2 汇编完全一致，无新增超限文件。

## [0.298] - 2026-11-18

### 纯标量列表的浮点下标写入跳过旧值读取（nbody −1.4% 至 −2.7%）

- **发射**：O1+ 的 checked 浮点 `IndexStore` 保留边界检查和负下标归一化；通过后仅在运行时 `elem_kind == 0` 时直接写入，不读旧值也不进行释放。非零种类走原路径，不用静态 `list[float]` 代替所有权证明。不改变列表长度、位图或 unchecked 发射。
- **取证**：在 `advance` 做独立汇编消融。把长度加载折进比较约少 5% 指令但耗时近乎不变；运行时 kind 0 守卫降约 2.5%。本输入专用的「强行去掉所有权检查和边界检查」仍需 28.76 ms，不能作为生产优化，说明这两项解释不了与 Codon 的全部差距。
- **实测**：对 `572c03b` 的专项绑核 21 轮（每侧每轮 5 次），`nbody` **33.40 → 32.51 ms（−2.7%）**，Codon 0.20.7 为 14.22 ms，输出相同。另测 10 项的 15 轮中，`nbody` **39.91 → 39.36 ms（−1.4%，p25 −1.1%）**，`spectral_norm` +0.3%，合计 +0.2%；其余八项没有生成新快路径。
- **验证**：新增 200 覆盖浮点正负下标覆盖、两个方向的越界及堆列表的覆盖与别名。O0/O1/O2、setjmp、分裂及图着色分配路径对拍 CPython，Win64 的 `nbody` 通过交叉汇编（未执行 Windows 程序）。差分 201/0、0.271 语料无差异、普通与 ASan 各 219 例通过、CTest 3/3、泄漏抽查 13 例通过；规模检查仍只有原有 5 处超限。

## [0.297] - 2026-11-18

### 守卫虚调用直接把整数结果交给累加（obj_dispatch −1.5%）

- **发射**：O2 后端只融合相邻的守卫零参数整数 `VCall → Add → StoreGlobal`，调用和加法结果均须块内单用。命中体和间接回退都将返回值留在 `%rax`，合流后直接加上调用前已求值的左操作数再写全局。回退可能改写全局或抛错，不重新读取左值，也不提前执行外层赋值。
- **实测**：对 `d7e1257` 绑核交替 15 轮，`obj_dispatch` 343.22 → **338.12 ms（−1.5%，p25 −1.6%）**；realistic 六项合计 1368.94 → 1364.61 ms（−0.3%），其余在 ±0.6% 内。另用适配后同一源码配对复测 11 轮：PyAdamas 344.04 → 330.31 ms（−4.0%），Codon 0.20.7 为 110.78 ms，三者输出相同。
- **未采用的实验**：以手改汇编让累加器跨迭代留在 callee-saved 寄存器，在真实慢路前写回；析构和虚调用改写/抛出样例对拍 CPython。独立消融在原负载和适配负载分别慢 2.8% / 7.0%，尽管执行指令少约 7.7%；硬件计数显示传统解码增加。与返回值融合同时采用也更慢，未改循环全局写回策略。
- **验证**：199 覆盖同一调用点的守卫命中、析构重入、回退正常改写和改写后抛错。O0/O1/O2、setjmp、分裂及图着色分配路径对拍 CPython，ASan/LSan 通过；Win64 基准已交叉汇编。差分 200/0、0.271 语料无差异、普通与 ASan 各 218 例通过、CTest 3/3、泄漏抽查 12 例通过；规模检查仍只有原有 5 处超限。

## [0.296] - 2026-11-18

### RC 快路条件转发全局接收者（obj_dispatch −13.6%）

- **发射**：O2 后端匹配同块的「读旧全局、发布新值、`RcDec`、再读同一全局」。不改 IR/活跃性，也不撤掉 0.295 的重入屏障。只有没有运行时调用的减计数快路才复用已发布值；一旦走慢调用，返回后重新读全局。零额外实参的 `VCall` 可直接从 ABI arg0 消费单用结果，中间最多跨过一个不同全局的整数读；相邻 `GenNext` 用原结果栈位置。既有所有权、异常边和慢路浮点保存不变。
- **实测**：对 `040d1d5` 绑核交替 15 轮，`obj_dispatch` **398.20 → 344.07 ms（−13.6%，p25 −13.7%）**；realistic 六项合计 **1429.27 → 1373.42 ms（−3.9%）**，其余在 ±0.8% 内。适配后的同源源码配对复测 11 轮：PyAdamas 389.69 → 343.71 ms，Codon 0.20.7 为 110.54 ms，三者输出一致。
- **验证**：新增 198 用例在同一调用点执行带写入的析构慢路和非归零快路；慢路同时改写全局接收者与累加器。O0/O1/O2、setjmp、分裂和图着色分配路径均对拍 CPython；检查汇编确认慢调用后重新读取全局。差分 199/0、0.271 语料无差异、普通与 ASan 各 217 例通过、CTest 3/3、LeakSanitizer 抽查 11 例通过。Win64 基准守卫交叉汇编通过，完整用例与基线同样受扫描位图段指令限制，未验证 Windows 运行；规模检查仍只有原有 5 处超限。

## [0.295] - 2026-11-18

### 禁止全局转发跨过可重入的 RC 释放，撤回显式虚调用守卫实验

- **正确性**：释放旧生成器可能在 `finally` 中重入并覆写刚发布的全局对象。0.290 的全局转发错误地跨过了 `RcDec`，接下来把已释放的值传给 `next`；新增 197 用例在旧 O2 上触发 ASan heap-use-after-free。现在 `RcDec` / `RcDecUnknown` / `RcCleanup` 清空全局转发状态；边界版本化同样不把会释放引用的调用当作无修改操作。保持 retain → 发布 → release 的顺序。
- **代价**：对 `2726794` 的 15 轮绑核 A/B 中，`obj_dispatch` 因恢复释放后的全局装载，从 339.89 → **399.38 ms（+17.5%，p25 同为 +17.5%）**；六项合计 +4.8%，其余在 −0.3% 到 +1.3% 间。未放宽屏障来掩盖退步。
- **撤回的实验**：另用未修改的 0.294 对照显式 `FieldLoad / ClassAddr / Cmp / Branch`、整数合流槽和原虚调用慢路。15 轮绑核随机交替，`obj_dispatch` **333.05 → 436.75 ms（+31.1%）**，指令约 +16.6%；跨块虚表/类地址往返和布局变化抵销了原快路径。实验未合入，0.294 的后端守卫继续保留。下一步先减少守卫地址搬运，再以可重入慢路为边界证明全局与容器状态，不能靠放行所有 RC 调用来提升循环。
- **验证**：197 在 O0/O1/O2 和 setjmp 路径与 CPython 一致，ASan/LSan 通过。差分 198/0、0.271 语料无差异、普通与 ASan 各 216 例通过、CTest 3/3、泄漏抽查 10 例通过。初次并发 CTest 的码点计时斜率失败；该用例基线和修复版汇编相同，单测和低并发全套复跑通过。规模检查仍只有原有 5 处超限。

## [0.294] - 2026-11-18

### 小型整数虚方法的守卫内联（obj_dispatch −11.7%，realistic 合计 −3.3%）

- **发射**：O2 的无额外实参、返回整数的普通 `VCall`，最多为四个本模块实际虚表生成守卫。只接受整个叶子方法 IR 为整数常量返回、读 `self` 的整数实例字段、或两个此类字段的加减乘。继承同一方法的类分别比较实际虚表地址，共用内联体。命不中、候选过多、方法带副作用或不满足形态时保留原间接调用。静态类型只筛候选，不证明实际对象类型；不改变 IR、RC 或现有调用 ABI/EH。
- **实测**：对 `1ac7acd` 绑核交替 15 轮，`obj_dispatch` 385.65 → **340.44 ms（−11.7%，p25 同为 −11.7%）**；realistic 六项中位数合计 1410.17 → **1363.54 ms（−3.3%）**，其余用例在 ±0.5% 内。另用相同的 Codon 兼容源码配对复测 11 轮：PyAdamas 380.95 → 339.83 ms，Codon 0.20.7 为 110.53 ms，三者输出相同。
- **验证**：新增 196 用例覆盖继承、覆盖、经 `Any` 到达的同槽非子类、带写入和抛错的回退。O0/O1/O2 及 setjmp 路径对拍 CPython，O2 汇编确认守卫和原间接回退并存。差分 197/0、0.271 语料无差异、普通与 ASan 各 215 例通过、CTest 3/3、LeakSanitizer 抽查 9 例通过。Win64 目标基准的守卫汇编通过交叉汇编，未验证 Windows 运行；规模检查仍只有原有 5 处超限。

## [0.293] - 2026-11-18

### obj_dispatch 栈转发实验撤回，保留全局释放顺序回归

- **旧全局值直接传给 `RcDec`**：在相邻的 `LoadGlobal → StoreGlobal → RcDec` 中，把仅供释放的旧值直接读入 ABI 首个整数实参寄存器。反汇编确认少了一次栈写和读回；对 0.292 的 15 轮绑核打乱顺序对照（每侧每轮 5 次），`obj_dispatch` 从 378.42 变为 **403.88 ms（+6.7%，p25 +6.8%）**。单独的硬件计数显示指令 −2.9%、周期 +6.5%，DSB 微操作减少、MITE 微操作增加，与布局改变导致更多传统解码相符。
- **整数虚调用结果留在 `%rax`**：独立试验 `VCall → Add → StoreGlobal`，保留调用前的累加器快照，用 `addq` 直接更新返回寄存器。同一轮对照 **379.40 ms（+0.3%，p25 +0.3%）**，无可辨收益。另试把列表长度加载折进边界比较，短测也变慢。三项均撤回，不改变编译器发射。
- **回归**：新增 195 用例。旧生成器归零执行 `finally` 时必须读到已发布的新全局；如果它重入写入第三个值，外层赋值不能覆盖该结果。还覆盖空旧值、自赋值和两个全局共享同一生成器。O0/O1/O2 和 setjmp 路径对拍 CPython；差分 196/0，0.271 语料无差异，普通与 ASan 各 214 例通过，CTest 3/3，LeakSanitizer 抽查 8 例通过。规模检查仍只有原有 5 处超限。

## [0.292] - 2026-11-18

### 精简普通虚调用发射（obj_dispatch −3.5%，realistic 合计 −0.7%）

- **发射**：普通 `VCall` 先写栈实参，把 `self` 装进 ABI 的首个整数实参寄存器（SysV `%rdi` / Win64 `%rcx`），用它读取 vtable，再直接发射 `call *slot(%r11)`。每次少一次重复的 `self` 装载和一条显式槽加载；仍按实际对象的虚表分派，不改 IR、引用所有权或调用约定，静态和 `super` 调用未改。
- **实测**：对 `1421c8f` 绑核交替 15 轮、每侧每轮 5 次，`obj_dispatch` 399.02 → **385.20 ms（−3.5%，p25 同为 −3.5%）**；六项中位数合计 1418.28 → **1408.31 ms（−0.7%）**。`dict_wordfreq` +1.3%，其余变化在 ±0.3% 内，六项输出与基线一致。
- **验证**：新增 194 用例覆盖非零槽、继承和重写、混合实参（SysV 整型和浮点各两个落栈）、浮点返回和异常捕获，O0/O1/O2 与 setjmp 回退路径均对拍 CPython。差分 195/0、0.271 语料对照无差异、普通与 ASan 各 213 例通过、CTest 3/3、LeakSanitizer 抽查 7 例通过。Win64 混合实参抽查交叉汇编通过；带捕获绑定的完整用例仍触发基线也有的扫描位图段指令问题，未验证 Windows 运行。文件规模检查仍只有原有 5 处超限。

## [0.291] - 2026-11-18

### 缩短引用计数热路径（obj_dispatch −8.9%，realistic 合计 −2.8%）

- **取证**：`obj_dispatch` 的 6400 万轮内层循环每次都 retain 新元素、发布全局循环变量、release 旧元素。采样约 82% 落在生成的主函数；反汇编显示每次增减还要先装入并比较 64 位永生哨兵。优化只缩短发射序列，不改变全局绑定、引用所有权或虚调用。
- **发射**：普通堆值把 `INT64_MIN` 哨兵判断移到冷分支；`RcInc` 在非正计数上用 `negq/jo` 识别哨兵，`RcDec` 用 `subq $1` 同时做有符号比较与减法，只有原计数大于 1 才写回，冷分支借溢出标志识别哨兵。空指针、GC 收集期、计数归零及非法计数仍按原条件走慢路径；两者均使用目标 ABI 的首个整数实参寄存器。
- **测量取舍**：对所有类型统一推迟哨兵判断的实验让 `str_build` 慢 4.7%；最终版对 `str` / `bytes` 保留优先判断永生值。对 `4108e72` 绑核交替 15 轮（每侧每轮 5 次）：`obj_dispatch` 437.94 → **399.09 ms（−8.9%，p25 同为 −8.9%）**，`str_build` +0.4%，`dict_wordfreq` +1.4%，六项中位数合计 1465.20 → **1424.47 ms（−2.8%）**。
- **验证**：新增 193 用例覆盖空的旧全局值、清空列表后仍存活的循环变量、永生字符串与动态字符串交替赋值，O0/O2 对拍 CPython。差分 194/0、0.271 语料对照无差异、普通与 ASan 各 212 例通过，LeakSanitizer 抽查 6 例通过，新用例 Win64 交叉汇编通过。高并发 ASan 跑 `27_runtime.py` 曾有一次取消清理输出缺行；基线/候选各单独复跑 5 次及低并发 ASan 全套均通过。文件规模检查仍只报原有 5 处超限。

## [0.290] - 2026-11-18

### store.global→load.global 转发（obj_dispatch −13.7%，套件 −4.8%）

- **取证**：obj_dispatch 的模块级 `for s in shapes: total += s.area()` —— s 是全局 g8，每轮 store g8 之后又 load g8 当 vcall 的 self（STLF 往返 + 一次全局读，内层 64M 轮）。0.289 的 annotate 里全局读/写合计 ~25%。
- **forward_global_loads**（opt_forward.cpp）：块内 load.global 读到刚被 store.global 写入的全局，且中间只隔纯 IR 指令或 RC 族 CallRt → 直接转发到存入的 vreg，删除 load。要求装载结果块内只用一次（不延长跨块活跃区间）+ 类型一致；GlobalAddr / 用户调用（Call/VCall/…）/ CallExtern / GenYield 使映射失效。
- **安全论证**：用户全局只由生成代码的 StoreGlobal 写入；pyadamas 不支持 `__del__`（全仓库 grep 核实），RC 析构全是运行时内部代码（list_destroy/dict_destroy 等），不碰用户全局；sort 的 key 在生成循环里预算好（ListSortedBy 收 keys 列表），运行时会调用户代码的入口（FuncReduce 族等）不在 RC 白名单里，一律失效映射。
- **实测（15 轮）**：obj_dispatch 508.73 → **439.15 ms（−13.7%**，p25 −13.9%）；rec_memo −0.8%、str_build −0.6%（模块级赋值即读形态同样受益）；其余中性；套件合计 **−4.8%**。
- **验证**：差分 193/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 211 ran ok / EXIT=0；ASan detect_leaks=1 干净（13_gc / 192 / 66_genexpr / 119_gen_except_suspend 抽查）。

## [0.289] - 2026-11-18

### Bin+StoreGlobal 融合（obj_dispatch −5.7%，套件 −1.5%）

- **取证**：obj_dispatch 的 `total += s.area()` 内层 64M 轮，每轮把 add 结果写进 vreg 栈槽再读回来写全局（`mov %rax,0xf8(%rsp)` + `mov 0xf8(%rsp),%rax` 的 STLF 往返，annotate 里合计 ~10%）。
- **can_fuse_global**：整数 Bin（Add/Sub/Mul/位运算/移位）+ StoreGlobal，结果块内只用一次且不跨块 → bin 结果留在 %rax，紧跟一条 `movq %rax, global`，省掉栈槽往返。StoreGlobal 是纯 store（RC 由降低器单独发 inc/dec），全局根本身就是那个符号，无 GC 帧扫描问题（与 StoreSlot 融合的坑不同），任意类型安全。
- **实测（15 轮）**：obj_dispatch 540.29 → **509.55 ms（−5.7%**，p25 −5.6%）；rec_memo +1.8%（perf stat：指令 −0.5% 但 cycles +1.6%，DSB 近平 —— 函数尺寸变化的布局效应，权衡接受）；wordfreq +0.9% 同类；str_build −0.2%；套件合计 **−1.5%**。
- **验证**：差分 193/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 211 ran ok / EXIT=0；ASan detect_leaks=1 干净（13_gc / 192 / 66_genexpr / 119_gen_except_suspend 抽查）。

## [0.288] - 2026-11-18

### str 相等单字节内容快路径（str_build −6.5%，套件 −0.8%）

- **取证**：pa_str_eq 仍占 str_build 8.4% —— 指针相等快路径早已存在，但 `c == "0"` 有 9/10 是**不等的数字字符**（驻留指针不同）→ 每次为一个字节比较走完整 call。
- **gen_cmp 扩展**：指针不等后双方 len==1 直接比 data[0]（str/bytes 同布局：len+8、data+24）；不等直接出结果（Eq→0 / Ne→1），len!=1 才落回 pa_str_eq。顺带删掉 Eq/Ne 分支前重复的实参装填。scratch 用 r10/r11 避开 Win64 实参寄存器。
- **两个汇编坑（记录警示）**：movzbq 目的必须 64 位寄存器 —— %al 报 operand size mismatch，%eax 报 q 后缀不允许 32 位，正确是 %rax 再 cmpb %al 比低字节。首版让所有含 str 相等的用例编译失败，**difftest 因 O0/O2 同样失败而假绿（193/0）**，套件 211→189 才暴露。教训：difftest 全绿还要看套件计数，两侧同样失败不算正确。
- **实测（15 轮）**：str_build 160.47 → **150.07 ms（−6.5%**，p25 −6.2%）；dict_wordfreq −0.9%；其余中性；套件合计 **−0.8%**。
- **验证**：差分 193/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 211 ran ok / EXIT=0；ASan detect_leaks=1 干净。

## [0.287] - 2026-11-18

### str 下标 ASCII 快路径内联（str_build −6.2%，套件 −0.6%）

- **取证**：str_build 逐字符下标走 pa_str_char_at（5.3%）+ pa_char_str（7%）两层 call。ASCII 字符命中 256 项驻留表（零分配、永生），调用开销纯浪费。
- **gen_index_load 的 Str 分支内联快路径**（提取为 gen_str_index_inline，避免 codegen_call.cpp 超限）：`0<=i<pub` 且 `pub==len`（str 不变量：等号当且仅当全 ASCII）且字节 <0x80 → 直接查 pa_char_tab（main 序言已初始化）。负索引/越界/非 ASCII 落回 pa_str_char_at（归一化、IndexError、UTF-8 码点路径语义不变）；高字节守卫是防御性的。
- **ABI**：s/i 走 arg_reg_i，scratch 用 rax/r10（两 ABI 均 volatile 且避开实参寄存器）；快路径不碰 xmm，慢路径 spill/reload。
- **实测（15 轮）**：str_build 171.10 → **160.49 ms（−6.2%**，p25 −6.0%）；queue_sim −0.5%；其余中性；套件合计 **−0.6%**。
- **验证**：差分 193/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 211 ran ok / EXIT=0；ASan detect_leaks=1 干净；unicode（héllo世界）/负索引/越界 IndexError/空串边界对拍 CPython 逐行一致。

## [0.286] - 2026-11-18

### 临时栈 push/take 内联快路径（str_build −5.3%，套件 −0.6%）

- **取证**：str_build 逐字符循环里 pa_rc_temp 占 6.8%。字符串是永生值（256 项驻留表），push 读 rc 比较 INT64_MIN 就返回；take 更要把整条栈白扫一遍（永生值从不入栈，永远找不到）。call/ret + 扫描全是浪费。
- **gen_rc_inline 扩展 RcTemp / RcTempTake**（仿 inc/dec 先例）：永生 push/take 5 条指令（null 检查 + rc 加载 + INT64_MIN 比较）；非永生 push 12 条（stats/diag/扩容守卫落回原函数）；take 内联栈顶命中 + 从后往前扫描 + swap-remove，与运行时逐位一致。运行时 pa_rc_temp_take 同步加永生早退（语义等价：pa_rc_temp 跳过永生值，栈上不可能有）。
- **寄存器安全**：CallRt 所在块本就没有 GPR/FP 临时值（既有门控）；快路径不碰 xmm，慢路径 spill/reload_fp_slots（析构可能跑，遵循 inc/dec 先例）。
- **实测（15 轮）**：str_build 180.36 → **170.79 ms（−5.3%**，p25 −5.4%）；其余中性；套件合计 **−0.6%**。RC stats 模式输出一致（temp=132000 / cleanup=132000 平衡；take=0 因 opt_rc 后 str_build IR 已无 take 指令，push/cleanup 配对）。
- **验证**：差分 193/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 211 ran ok / EXIT=0；ASan detect_leaks=1 干净（13_gc / 66_genexpr / 119_gen_except_suspend / 192_dict_cache 抽查）。

## [0.285] - 2026-11-18

### dict set 单探测 + 标量销毁快路径（wordfreq −1.7%；两条方法论教训）

- **set 单探测**：pa_dict_set 原来先 dict_lookup 找已有 key，未命中再 dict_place_slot 从同一 hash 重探一遍找空槽/墓碑 —— 新键插入走两遍探测链。新增 dict_lookup_place：探测路上顺带记第一个墓碑，命中返回 entry，未命中把可写槽写进 out 参（与 place_slot 逐位一致）。reindex 会重建索引，触发后重算 place。bump 未命中路径也从「lookup+lookup+place」三探降到两探。
- **标量销毁快路径**：dict_destroy / dict_gc_release / pa_dict_clear 在 key_own==0 && val_own==0 时跳过整条 entry 链游（无 RC 可做）。rec_memo 的 dict[int,int] 每轮 25 项链游是循环携带指针追逐。
- **教训一 —— gcc 内联扰动**：新增 dict_lookup_place 后 gcc 的内联预算把 dict_key_matches 从 dict_lookup 里 outline 成公共函数（两处调用共享），wordfreq 每次探测多一层 call/ret，**指令 +6.3%、cycles +4.5%**（perf stat + objdump 证实：old lookup 94 行内联形态 → new 63 行带 call）。修复：dict_key_matches 加 always_inline，恢复紧探测循环。**同一翻译单元里加函数可能改变既有热函数的内联决策，性能回归要先看 objdump 再怀疑算法。**
- **教训二 —— perf 占比 ≠ 边际成本**：销毁链游在 perf 里占 rec_memo 11.6%，跳过后指令 −5.3% 但 **cycles ±0**（abbench 15 轮 p25 +0.0%）—— 链游的 L1 依赖加载全落在其它延迟的阴影里，OoO 把它完全隐藏。perf 的采样占比（含 skid）不能直接当「省掉就赚」的依据，必须 A/B 验证。
- **实测（15 轮）**：dict_wordfreq **−1.7%**（p25 −2.5%；perf stat cycles −6%、指令 −5%）；rec_memo 中性；其余中性；套件合计 −0.2%。
- **验证**：差分 193/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 211 ran ok / EXIT=0；ASan detect_leaks=1 干净（13_gc / 88_defaultdict / 192_dict_cache 抽查）。

## [0.284] - 2026-11-18

### contains+get 双查找 IR 层融合（rec_memo −11.8%，套件 −2.0%）

- **0.283 的运行时末次查找缓存因 STLF 串行化撤回后，这版在 IR 层做融合**：新 pass `opt_dict_fuse` 识别 `%c = DictContains(d,k); branch %c, T, F` + T 的纯前缀后的 `DictGet(d2,k2)`（d/k 同槽），改写成 `DictTryGet(d,k)`（idx=新帧槽 S，found 落 %c 直接进分支寄存器）+ T 里 `load.slot S`。一次哈希+探测替代两次；值经帧槽中转但**同一迭代内一写一读，无跨迭代依赖链**（与失败缓存的本质区别）。
- **守卫**：T 的唯一前驱是 B 且 F≠T（别的路径进 T 会读到未写的槽）；contains→分支、T 入口→get 之间都是纯指令（调用/存储可能改 dict）；get 的值只在 T 内用（SSA 保证）；不匹配 `not in`（条件经 Un）等变形，保守。
- **新运行时入口 pa_dict_try_get**：命中写借用值到 *out 返回 1，缺失返回 0 不写 out、不创建 default（与 contains 守卫下的 get 逐位等价；defaultdict 的缺省创建只发生在真 get，这里缺失走 F 分支，语义不变）。
- **开发中修掉的坑**：值槽没有 StoreSlot 定义 → 寄存器提升的活跃分析视其为入口 live-in，块入口重载逻辑见「所有前驱都持有寄存器」就不重载 —— 运行时写栈后提升寄存器不更新，读陈旧值（rec_memo 输出 390000、difftest 两例红）。修复：DictTryGet 的值槽与 SlotAddr 一样排除提升，一律留在内存（堆类型槽本就不提升，整型槽才受影响）。
- **实测（15 轮）**：rec_memo 297.69 → **262.57 ms（−11.8%**，p25 −12.1%）；profile 里 pa_dict_contains / pa_dict_get 消失，pa_dict_try_get 8.3%，dict_lookup 27% → 22.7%；其余用例中性；套件合计 **−2.0%**。
- **验证**：差分 193/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 211 ran ok / EXIT=0；ASan detect_leaks=1 干净（13_gc / 88_defaultdict / 192_dict_cache / 90_functools_final / 39_match_advanced 重点抽查）。

## [0.283] - 2026-11-18

### dict 末次查找缓存：尝试 → 实测负收益 → 撤回（STLF 串行化诊断）

- **尝试**：rec_memo 的 `if n in memo: return memo[n]` 是 contains+get 双查找（dict_lookup 占运行时 27%）。给 PyAdamasDict 加末次查找缓存（cache_key/hash/entry/valid，+32 字节）：同键连续查找命中直接返回 entry 下标，省 hash+probe；插入/擦除/reindex 失效。正确性通过 20000 步确定性伪随机操作对拍 CPython + 边界用例（del+空闲 entry 复用、扩容、墓碑清理、相等异指针 str 键、±0.0、pop/clear/update）。
- **实测负收益**：rec_memo **+8.8%**（abbench 15 轮，p25 +8.7%），dict_wordfreq 中性，其余噪声。撤回。
- **判别实验诊断**：保留缓存写入、禁用命中检查的变体 cycles=945M，完整缓存 886M，基线约 800M —— 命中确实省了 ~59M cycles，但**每次查找的缓存写入花掉 ~145M（≈7.6 cycles/次）**。根因：缓存字段在 dict payload 第三条 cache line，**相邻两次查找经缓存字段形成循环携带的 store→load 依赖（STLF ~5-7 cycles 串行）**——本来独立的查找被串成链。与 0.278 余数旋转同一族教训（OoO 下内存依赖链比指令数更致命）。
- **结论**：运行时内存驻留的查找缓存是死路；可行的融合路径在 codegen 层（寄存器驻留的 found/value 对，如 DictGetOpt IR  op），留作后续候选。
- **保留**：`tests/run/192_dict_cache.py` 作为字典语义回归（含 expect 段）。首版测试曾因 LCG 乘法溢出 int64 与 CPython 大整数语义分叉而假失败——改 32 位 LCG（乘积 <2^61）后两边序列一致；这不是缓存 bug，是测试 bug，记录以免再犯。
- **验证（撤回后）**：差分 193/0；release 与 ASan 套件各 **211** ran ok / EXIT=0；abbench 对 0.282 全噪声级（运行时与 0.282 逐位一致）。

## [0.282] - 2026-11-18

### list.append 内联快路径 + 循环头 32 字节对齐（str_build −6.6%，套件 −1.2%）

- **append 内联**：`pa_list_append` 在无扩容（len<cap）、无位图（heap_bits==NULL）、非 kind2（kind2 空表运行时会分配位图）时就是 `data[len++]=v` 一条 store，call/ret + 装填 + 运行时检查比活本身还贵（str_build profile 里 pa_list_append 14.6%）。三个守卫任一不满足落回原调用。RC retain 不在这条指令里（生成代码在 append 前单独发 pa_rc_inc），语义不变。
- **重要发现 —— DSB（uop 缓存）布局敏感性**：append 内联初版让 obj_dispatch **+7.2%**。perf stat：指令 −0.5%、分支/branch-misses/L1-icache-misses 全不变、**cycles +9.4%**；`idq.dsb_uops` −228M / `idq.mite_uops` +176M —— 上游代码尺寸变化把热循环的 uop 挤出 DSB 窗口，落回 MITE 传统解码。循环头原来已 16 字节对齐（.p2align 4）但不够：**改 32 字节（.p2align 5，DSB 窗口粒度）后 obj_dispatch 大部分恢复**，且 queue_sim / dict_wordfreq 额外受益。64 字节无进一步收益（DSB 组冲突不由入口对齐单独解决）。**教训：codegen 小改动也可能通过 uop 缓存布局产生 ±10% 的远端效应；评估时看 idq.dsb_uops / idq.mite_uops，循环头对齐用 32B。**
- **实测（15 轮）**：str_build 193.32 → **180.61 ms（−6.6%**，p25 −6.6%）；dict_wordfreq **−5.3%**；queue_sim **−2.7%**；sort_struct −1.2%；rec_memo 中性；obj_dispatch **+2.9%**（残余 DSB 组冲突，perf stat 证实非分支/非 icache，权衡接受）；套件合计 **−1.2%**。
- **验证**：差分 192/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 210 ran ok / EXIT=0；ASan detect_leaks=1 干净。

## [0.281] - 2026-11-18

### dict 缓冲池化 + 去掉双重 mix64（rec_memo −32.4%，套件合计 −8.4%）

- **取证**：rec_memo（每轮新建 memo dict 跑 fib(25)，195000 轮）profile 里 dict_lookup 19.6% + dict_hash_value 17.6% + pa_dict_set 9.1% + dict_destroy 7.3% + dict_reindex 6.7% + malloc 4.7% + free 2.8% ≈ 75% 在字典路径。每个新 dict 三次 malloc（entries/order/index）+ 25 个键触发 3 次 reindex（icap 8→16→32→64，每次 calloc+free+重哈希）+ 销毁三次 free。
- **缓冲池化**：dict_pool 原来只池化 RC 头块，dict_destroy 把三块缓冲 free 掉。改成**小 dict（cap≤64、icap≤128）缓冲随头一起保留**：缓冲指针寄存在池节点（数组栈，256 槽）里 —— payload 会被 rc_alloc_core 的 memset 清零，不能留在 payload；分配命中后 memset 之后写回。pa_dict_new 见 entries 非空即走复用路径：状态重置 + index 清零，**零 malloc、零增长**（缓冲已是上一世高水位，25 键的 memo 不再 reindex）。大 dict 缓冲直接 free 不保留，防池囤内存；GC 清扫路径（gc_free_body）自己 free 缓冲，不经池；shutdown drain 连缓冲一起 free（ASan 泄漏干净）。
- **去双重 mix64**：int/float 键哈希原来 mix64 两次（单值路径一次 + 公共尾部又一次）—— 单次 mix64 已是全雪崩双射，第二次纯浪费。字符串路径提前返回不受影响，与编译期字面量哈希（FNV+单次 mix64）一致；hash() 内置与字典共用同一函数，同值同哈希不变。
- **实测（15 轮）**：rec_memo 439.14 → **296.98 ms（−32.4%**，p25 −32.4%）；dict_wordfreq −2.9%；sort_struct −1.2%；queue_sim +1.2%（rc_alloc_core 多一个 DICT 池命中判断的分支/布局代价，权衡接受）；其余噪声。套件合计 **−8.4%**。profile 里 malloc/free/reindex 全部跌出前十，dict_hash_value 17.6% → 10.6%。
- **验证**：差分 192/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 210 ran ok / EXIT=0；ASan detect_leaks=1 干净（13_gc / 88_defaultdict / 66_genexpr / 119_gen_except_suspend 重点抽查）。

## [0.280] - 2026-11-18

### kind-0 元组分配内联快路径（queue_sim −16.5%）+ 四个潜在 bug 修复

- **内联快路径**：`gen_make_list` 的 kind-0 元组（全非堆元素、n≤4）在池命中且 `gc_need_check==0`、diag 关闭时，直接在生成代码里弹池、初始化（0.276 不变量：池块 dtor/release/kind/size 恒定，只重置 rc/marked/prev/next + payload），省 call/ret + 参数装填 + 运行时检查；任一守卫不满足落回 `pa_tuple_new_filled` 原路径（GC 触发/assist 点语义不变）。perf 里 pa_tuple_new_filled 跌出前八。
- **bug 1（发射，致命）**：内联路径返回 RC 头指针 h 而非 payload h+64 —— 槽里存了头指针，dec 的 p-1 落到前一个对象内部，池链被 interior 指针污染，级联破坏。调试实录：ASan heap-buffer-overflow（读 argv 块尾后 16 字节）→ gdb 池头/数据字段 watchpoint → 抓到 put 参数是 `__file__` 字符串内部 → 反推返回值差 64。修复：返回前 `leaq 64(%rax), %rax`。
- **bug 2（发射，致命）**：守卫用 `cmpq`（8 字节）比较 4 字节 int 全局（gc_need_check / pa_diag_rc_on），把相邻 .bss 变量读进高 4 字节 → 永真 → 快路径从不命中（174 万个元组全走慢路径，计数器 fast=0 暴露）。gdb 单步 + eflags 证实 ZF=0 而 flag=0。修复：`cmpl`。
- **bug 3（运行时，0.274 起潜在）**：`pa_rc_dec_heap_batch` 的元组快释放入池前没把 rc 归零（池不变量 rc==0，dec_slow 的「dec on freed」panic 靠它挡二次释放）——stray dec 会静默二次 free → 池重复块。修复：入池前 `h->rc = 0`。
- **bug 4（运行时，潜在）**：`gc_need_check` 唯一清除点是 gc_incr_finish，小堆同步收集从不走增量 → 标志首次置 1 后永久滞留，每次分配白跑检查块（也挡住内联快路径）。修复：检查块无动作时清标志；记账在每次被跟踪分配接近 7/8 阈值时重新置 1、增量启动也置 1，触发时机不变（gc_runs=85 前后一致）。
- **实测（15 轮，0.279 修复后的 abbench）**：queue_sim 188.59 → **157.55 ms（−16.5%**，p25 −16.6%）；str_build −1.8%；sort_struct **+2.2%**（perf stat：指令 −0.4% 但 cycles +2.6% —— 内联序列在小 key 函数里的分支/布局代价，权衡接受）；其余噪声级。queue_sim 对 Go 40ms：5.1× → **3.9×**。
- **验证**：差分 192/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 210 ran ok / EXIT=0；ASan detect_leaks=1 干净；最小复现（纯元组 churn、元组+列表循环）对拍 CPython 一致。

## [0.279] - 2026-11-18

### 浮点临时寄存器分配与 GPR 门控解耦（spectral_norm −19.9%）+ abbench 测量可靠性修复

- **根因**：`assign_block_temp_regs` 用同一个安全门控管 GPR 池（r8-r11）和浮点池（xmm3-7）：spectral_norm 的 eval_a 内联进内层循环后含整数 `//2`（魔数序列用 GPR scratch）→ **整块被踢出临时分配**，所有浮点中间值被迫走栈往返（divsd/mulsd 读写内存，perf annotate 里 `mulsd 0x1d0(%rsp)` 一片）。
- **修复**：新增 `fp_temp_op_ok` 独立门控（定义在 codegen_ops.cpp）：整数 div/mod/floordiv 只用 GPR scratch（panic 调用不返回）→ 浮点安全；浮点 FloorDiv/Mod/Pow 调 floor/fmod/pow 会正常返回 → 不安全；列表 IndexLoad 越界慢路径不返回 → 安全，字符串 IndexLoad 调 pa_str_char_at 会返回 → 不安全；CallRt（含 RC 慢路径）→ 不安全。xmm3-7 只被会正常返回的调用破坏；浮点算术 scratch 是 xmm0/1/2，与临时池不相交。两池独立门控后，含整数除法的浮点热块也能拿到 xmm 临时寄存器。
- **实测（修复后的 abbench，15 轮）**：spectral_norm 74.79 → **59.93 ms（−19.9%**，p25 −20.0%）；其余 8 个 classic 用例生成二进制**逐字节相同**（cmp 证实），差值在噪声带内。
- **abbench 可靠性修复（重要）**：对照实验（两侧同一编译器）发现短用例（<100ms）锁进机器周期性干扰的固定相位 —— 固定用例顺序 + A 恒先于 B，**同一份二进制量出 ±20% 假差**（fannkuch +19.7%、fasta +21.8%、fib −14.6%）。修复三条：每轮 `sort -R` 打乱用例顺序；奇偶轮交换 A/B 先后；`BURST=5` 连跑取平均（覆盖干扰周期）。修复后对照：全用例 ±3.5% 内、多数 ±1%。**历史短用例数据需用新工具复核**；长用例（queue_sim 等 >200ms）跨多个干扰周期，历史数据仍可信。
- **验证**：差分 192/0；release 与 ASan 套件各 210 ran ok / EXIT=0。

## [0.278] - 2026-11-18

### 修 0.277 嵌套同名提示阴影洞；增量余数强度削减实测负收益，撤回

- **正确性修复（0.277 的洞）**：非负提示原来用 save/restore 包围循环体 —— 嵌套**同名**循环会重写同一个槽，内层退出后恢复的外层标记把内层终值当外层计数器值。反例：`for i in range(3): for i in range(-4, 0): pass; s += i % 3` —— 内层结束后 i=-1，恢复的外层标记让它走无符号路径，结果错。改成单层集合 + **入口擦目标槽旧键、退出擦本层键**：外层提示从内层入口起永久作废（运行期值已是内层的），不同槽嵌套互不影响。回归并入 `tests/run/191_rem_strength.py`。
- **增量余数强度削减（尝试 → 撤回）**：曾把循环体里 `i % 正常量` 替换成旋转余数槽（循环前 rem=start%c，incr 里 rem=rem+1、遇 c 归零，cmp+branch 替代魔数乘法链），含 AST 预扫描、load 识别、写槽作废。**实测负收益**：queue_sim **+4.2%**（p25 +4.6%）、str_build +4.5%、合计 +1.3%。perf stat：指令 −0.2%、分支 +3.9%、branch-misses −67%、**cycles +6.1%**。根因三条：① 旋转是**循环携带的串行依赖**（rem_k → rem_k+1），魔数链各轮独立、乱序窗口里流水重叠 —— 省指令不等于省周期（0.277 教训的延伸）；② 两个长活余数槽加剧 6 寄存器池竞争，mod 序列的 scratch 与余数槽寄存器冲突引发 spill/STLF 重载，rem 链拉长到 ~7-8 周期成为循环瓶颈；③ t%7 只在 1/3 迭代用到却每轮旋转 —— 急切旋转比按需魔数更贵。**结论：mod 延迟不在关键路径上（分支预测 + OoO 已隐藏），此方向关闭。**机制代码全部撤回，测试保留为取模语义回归。
- **重构**：nn / nonneg_of / 提示窗口助手移到 `src/lower_idiom.cpp`（新头 `lower_idiom.h`），`lower_stmt.cpp` 回到 1200 行内（1194）；`lower_impl.h` 只留内联胶水（def/alloc/store 钩子 + 声明）。
- **验证**：差分 192/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 210 ran ok / EXIT=0；190/191 对拍 CPython 逐行一致（负 start、重赋值作废、嵌套负 range 阴影、break/continue、生成器、INT64_MAX 边界、10 万轮压力）；ASan detect_leaks=1 干净。终态 abbench 对 0.277 全噪声级（sort_struct 生成二进制逐字节相同，cmp 证实）。

## [0.277] - 2026-11-18

### 非负被除数的无符号魔数除法/取模（指令数 −5%，queue_sim −0.4% / str_build −1.9% —— OoO 重叠教训）

- **非负打标**：降级期给可证非负的 vreg 打标（`IRFunction::vreg_nonneg`）：非负常量、len、非负槽的 load、非负操作数的 and/shr、除数非负的 mod（结果与除数同号）。**add/mul/sub 不打标**：int64 加法静默回绕，两非负数相加可能溢出成负，无界分析不能声称非负。
- **range 循环结构提示**：start 可证非负且 step==1 时，私有计数器只取 [start, stop] 的值（每次自增前有 ctr<stop 守卫，从 <= INT64_MAX-1 加 1 不回绕），循环变量由它赋值 —— 两个槽进 `nonneg_slots`，load 打标；体内写非非负值即作废（store_slot_raw，与 BoundsHint 同款机制）；体结束恢复外层提示集（嵌套/同名变量安全）。step!=1 不标记（大 stop 下 ctr+step 可能回绕）。提示可组合：内层 start 来自外层打标循环变量时同样生效。
- **无符号魔数路径**：被除数非负且除数为正时，|x|==x、商非负、余数天然在 [0,d) —— 省 abs/符号还原 4 条 + 余数符号修正 3 条；2 的幂商是逻辑右移、mod 退化成一条 AND。魔数本就按无符号域算（udiv_magic），直接复用。优化遍新建 vreg 无标（越界按 false），保守走有符号路径。
- **实测（abbench 交替 15 轮中位数，绑核）**：queue_sim 203.78 → 203.04 ms（−0.4%，p25 −0.4%）；str_build −1.9%（p25 −1.9%）；dict_wordfreq −1.0%、obj_dispatch −0.6%；sort_struct +1.8% 是噪声（生成二进制与基线**逐字节相同**，cmp 证实）；合计 −0.4%。
- **为什么指令 −5% 只换来 −0.4%**：perf stat 实测 queue_sim 指令 1536.5M → 1460.1M（−5.0%），cycles 508M → 506M。被省的符号修正指令本来就在乱序窗口里与魔数乘法链（movabs→mulq→add→shr→imul→sub，~9 周期关键路径）**重叠执行**，不在关键路径上 —— 指令数不等于周期。下一步若要真正缩短 mod 关键路径，候选是循环内增量余数强度削减（维护 r=(r+1)%c 的旋转寄存器，cmp+cmov 2-3 周期替代乘法链）。
- **验证**：新增 `tests/run/190_nonneg_divmod.py`（对拍 CPython 逐行一致：正常循环、负 start、体内写负作废、step2/while/循环外不标记、**INT64_MAX 边界逐项**、负除数/变量除数、嵌套同名、组合提示、and/shr/mod 传播、生成器；ASan detect_leaks=1 干净）。差分 191/0；全语料对 0.271 基线零差异；release 与 ASan 套件各 209 ran ok / EXIT=0。
- **过程注记**：本轮期间并行文档代理截断了 `codegen_regalloc.cpp`（−1420 行，构建破坏），经用户授权回退该代理对 4 个源文件的改动（文档改动保留），0.277 在干净树上融合验证。

## [0.276] - 2026-11-18

### rc_alloc 本体瘦身：池命中不重写不变头字段，untracked 不入 GC 记账（queue_sim −4.6%，sort_struct −4.2%）

- **池命中跳过 dtor/release/kind/size**：这四个头字段按池恒定 —— 池路由按 kind（dec_slow），str 池恒 STR/NULL/NULL（str 分配点全在 rt_core），两个列表池恒 LIST/list_destroy/list_gc_release，dict 池恒 DICT/dict_destroy/dict_gc_release；运行期没有任何代码改写活对象的这四个字段（grep 核实），GC 清扫直接 free 不回池。四个 store 移进 malloc 分支，池命中只重置 rc/marked（prev/next 由 link/untracked 路径写）。**新增池化分配点必须维持「同池同 dtor/release/kind」不变量。**
- **untracked 对象不入 GC 记账**：调用方保证无出边、不成环，生死全由 RC 管理，GC 永远收不到它们 —— 让它们驱动 GC 频率纯属浪费。queue_sim 改前 gc_runs=231 / gc_freed=0（全是元组 churn 触发的空收集），改后 **gc_runs=85（−63%）**，RC inc/dec/free 计数逐位一致。入口的 gc_need_check 检查块不变：untracked 分配点仍是 mark assist / 收集的触发点，只是不再累加阈值计数；GC 频率只由被跟踪对象的分配量驱动（环只可能出现在被跟踪对象之间）。
- **实测（abbench 交替 15 轮中位数，绑核）**：queue_sim 211.84 → **202.20 ms（−4.6%**，p25 −4.7%）；sort_struct 220.53 → **211.32 ms（−4.2%**，p25 −5.5%）；dict_wordfreq / obj_dispatch / rec_memo 持平；str_build +0.8%（p25 +0.2%，中性）；合计 −0.9%。rc_alloc_core profile 12.2% → 9.0%。queue_sim 累计 420 ms（0.269）→ 202 ms，对 Go 约 5.1×。
- **验证**：差分 190/0；全语料对 0.271 基线编译器输出/退出码零差异；release 与 ASan 套件各 208 ran ok / EXIT=0；9 个 GC/异常重用例（13/151/160/161/174/188/189/92/93）单独 ASan detect_leaks=1 干净。

## [0.275] - 2026-11-18

### 净 push 计数消除 mark 脚手架 + 函数入口 32 字节对齐（queue_sim −7.6%，六用例全中性偏好，合计 −2.2%）

- **净 push 计数**：`move_fresh`（槽/容器 take 新值的所有权交接）原来只发 `RcTempTake` 不减 `rc_pushes` —— push 后立刻被 take 的值仍让语句/循环的 mark/cleanup 被判为「有临时值」而保留。现在 take 减一，既有消除机制（stmt / 循环 / 推导式 / with 的 `rc_pushes == pushes_before` → `erase_mark`/`erase_cleanup`）自然生效：queue_sim 的 mark/cleanup **19 → 0**，temp/take 对被 opt_rc 相邻对消除全部删掉，热路径上 RC 水位脚手架整体消失。`fresh_vregs` 的每个插入点都配对 `push_temp`，计数不会下溢；异常路径不受影响 —— 被消的 cleanup 本来就不在展开路径上跑，push 与 take 之间抛异常滞留的临时值由外层水位兜底（try 的 mark_slot landing pad、caller 语句水位、入口 fn_mark），与既有循环/推导式消除同款不变量。
- **布局回归与修复**：单独上消除时 obj_dispatch **+5.0%**（p25 +4.3%）。perf stat 证明是纯布局效应：指令数 / 分支数 / branch-misses 全部不变（被消脚手架在 obj_dispatch 的冷路径），cycles +5%，`frontend_retired.dsb_miss` 1.37M → 10.3M —— 代码缩短后热区跨 uop-cache 窗口，DSB→MITE 切换增多。试循环头 `.p2align 5` 无效；**函数入口 `.p2align 4` → 5**（32 字节，Cascade Lake DSB 窗口宽度）把 obj_dispatch 从 +5% 拉到 −2%，rec_memo（零消除、纯对齐效应）−1.1%。
- **实测（abbench 交替 15 轮中位数，绑核）**：queue_sim 232.14 → **214.50 ms（−7.6%**，p25 −7.8%）；obj_dispatch −2.0%；sort_struct −2.4%；dict_wordfreq −1.6%；rec_memo −1.1%；str_build +0.6%（p25 +0.1%，中性）；**六用例全中性偏好，合计 1981.14 → 1938.53 ms（−2.2%）**。
- **验证**：差分 190/0；全语料对 0.271 基线编译器输出/退出码零差异；release 与 ASan 套件各 208 ran ok / EXIT=0；GC/生成器/async/异常重用例（13/151/174/188/189/24/73/92/93/173/129/123/159）单独 ASan detect_leaks=1 干净。
- **教训**：消除冷路径代码要警惕前端布局效应 —— 指令数不变 cycles 也能摆 ±5%；用 perf stat 的 `idq.dsb_uops` / `frontend_retired.dsb_miss` 诊断，别急着判语义回归。函数入口 32 字节对齐是便宜的稳定器。

## [0.274] - 2026-11-18

### kind-1 列表元素批量 dec：销毁循环内联快路径与元组释放（sort_struct −4.8%，str_build −2.7%，合计 −1.9%）

- **`pa_rc_dec_heap_batch`**：`list_destroy` 的 kind-1（全堆）元素释放原来逐元素 `pa_own_dec` → `pa_rc_dec_fast` 内联判断 + rc==1 时 `pa_rc_dec_slow` 调用。新批量入口把 dec 快路径与 0.273 的 untracked 小元组释放快路径内联进循环，模式开关（统计 / 收集期 / 诊断）提到循环外 —— 任一开启就逐元素走 `pa_rc_dec` 原路径，语义逐位一致（含 rc<=0 panic、永生跳过、NULL 跳过）。queue_sim 的 pending 每步销毁一批 rc==1 的元组，常见情形零调用。
- `list_destroy` 的 kind 1 分派到批量；kind 2（位图/注册表）与 kind 0 不变；`list_gc_release`（GC 试探 unref）不变。顺手：`rc_alloc_core` untracked 分支不再先写 marked=0 再写 UNTRACKED（省一次冗余 store）。
- **实测（abbench 交替 9 轮中位数，绑核）**：sort_struct 236.26 → **225.01 ms（−4.8%**，p25 −4.2%）；str_build 223.43 → **217.41 ms（−2.7%**，p25 −2.4%）；queue_sim 233.20 → 231.20 ms（−0.9%，p25 −1.6%）；dict_wordfreq 中位 −4.8% / p25 −0.7%（噪声偏大）；rec_memo −1.0%；obj_dispatch 持平；六用例合计 −1.9%。
- **profile 变化**：list_destroy 6.8% → 1.0%，工作移进 pa_rc_dec_heap_batch 2.6% + list_inline_pool_put 0.9%；pa_rc_dec_slow 5.8% → 4.4%。queue_sim 剩余大头：生成代码 59%、rc_alloc_core 12%、copy_range 4%、dec_unknown 2.8%。
- **验证**：差分 190/0；全语料对 0.271 基线编译器输出/退出码零差异；release 与 ASan 套件各 208 ran ok / EXIT=0；8 个 GC/列表重用例单独 ASan detect_leaks=1 干净；PYADAMAS_RC_STATS=1 回退路径计数正常（dec=481 万 / free=276 万）。

## [0.273] - 2026-11-18

### 分配/释放快路径：untracked 元组省 link/unlink 往返，列表省冗余清零（queue_sim −16.1%，sort_struct −12.6%）

- **untracked 直接分配**：`rc_alloc_untracked_ex` 旧实现是「按普通路径分配（`rc_link`：堆链插入 + RC 索引 put + 新生代 add）再立刻 `rc_unlink`（链摘除 + 索引 del + 新生代 del）」—— 对象存活期间的观察态本来就是不在链/不在索引，往返纯属浪费，每个元组出生还在索引里留一个墓碑。`rc_alloc` / `rc_alloc_ex` 拆出 `rc_alloc_core(..., untracked)`，untracked 路径直接置 prev/next NULL + marked=UNTRACKED，跳过 link/unlink 往返。GC 分配记账口径不变（元组原本就计入）。queue_sim 的 rc_link+rc_unlink 从 7.2% 降到 2.7%（只剩被跟踪的列表）。
- **LIST 跳过 memset**：三个列表构造点（pa_list_new / pa_list_new_filled / pa_tuple_new_filled）都显式写满全部 6 个字段；data 缓冲只读 [0, len)，append/filled 先写后读，池块里的陈旧内容永远不会被读到。`rc_alloc_core` 对 LIST kind 跳过 memset（其余 kind 不变）。profile 里 memset 2.6% 消失。diag 钩子只读 RC 头，不受影响。
- **untracked 小元组释放快路径**：`pa_rc_dec_slow` 的 rc 归零分支里，kind LIST + UNTRACKED + size 112 + elem_kind 0 + 无位图 + 内联缓冲时 `list_destroy` 是空操作（无元素可 dec、无位图可 free、无外部缓冲可还），整块直接挂回内联池，省一次函数调用 + 三个分支；条件不满足（增长过 / 混合种类）照走 dtor 原路径。
- **实测（abbench 交替 9 轮中位数，绑核）**：queue_sim 279.45 → **234.34 ms（−16.1%**，p25 −16.6%）；sort_struct 270.19 → **236.20 ms（−12.6%**，p25 −12.3%）；dict_wordfreq −2.2%；str_build / obj_dispatch / rec_memo 噪声级；六用例合计 −3.9%。分段测量：跳 link/unlink + 清零单独 queue_sim −14.6%、sort_struct −11.9%；释放快路径再补约 2%。queue_sim 累计 420 ms（0.269）→ 234 ms，对 Go 约 5.9×。
- **新 profile**：生成代码 58%、rc_alloc_core 10.6%、list_destroy 6.8%（kind-1 列表逐元素 dec，必要工作）、pa_rc_dec_slow 5.8%、pa_tuple_new_filled 3.8%、copy_range 3.1%、append 2.9%。下一步候选：分配本体（池查找 + 头初始化 + 记账）与列表元素 dec 循环。
- **验证**：差分 190/0；全语料对 0.271 基线编译器输出/退出码零差异；release 与 ASan 套件各 208 ran ok / EXIT=0；7 个 GC 重用例（13/151/160/161/174/188/189）单独 ASan detect_leaks=1 干净。

## [0.272] - 2026-11-18

### 切片拷贝惯用法识别：range-append 循环批量化的（queue_sim −2.5%，收益低于预期，原因已定位）

- **惯用法**：`for k in range(lo, hi): dst.append(src[k])`（单语句体、步长常量 1、无 else、dst/src 静态类型都是 list、k 不同于 dst/src）在降级器里识别，替换成一次 `pa_list_copy_range(dst, src, lo, hi, heap)` 调用，省掉逐元素 append 调用、容量检查和生成代码循环开销。新增 `src/lower_idiom.cpp`（纯 AST + 静态类型匹配，失败不发射任何 IR）；调用点在 `lower_for_range` 参数求值之前。
- **为什么不直接改写成切片语法**：Python 切片对负索引归一化、越界静默截断；range 循环逐个取值、越界抛 IndexError，语义不同。运行时入口分两条路：快路径（0<=lo<=hi<=src->len）一次扩容 + memcpy —— kind0/heap0 纯值拷贝，kind1/heap1 拷贝后批量 retain，其余种类（未定 3 / 混合 2 / 静态冲突）逐元素 prep+append（定型、位图、保守 retain 全走原路径）；慢路径逐元素 pa_list_get+prep+append，负索引与 IndexError（部分结果保留）和原循环逐位一致。dst==src 别名安全：hi<=len 保证源区间整体在写入起点之下，扩容先于取指针，逐元素分支每轮重读 src->data。
- **语义护栏**：批量版只在正常结束补 k=hi-1（条件分支：lo<hi 才写，零迭代保持原值），循环中途抛异常时 k 的中间进度无法还原 —— 所以只在分叉不可观测时匹配：当前位置不在任何 try/with/finally 里（cleanups 空，异常必然离开本函数），且 k 是普通局部槽（非全局 / 闭包 cell/env / 推导式槽）。
- **注册**：RtFn::ListCopyRange 追加在枚举末尾（不移动既有编号）+ rt_name + gen_rt_i5（5 整型参数）+ ir.cpp container_use。ir_ops.h / lower_impl.h 只加注册行（+2/+1，两文件本已超限，逻辑全在新文件与 rt_list.c）。
- **实测（abbench 交替 9 轮中位数，绑核）**：queue_sim 286.69 → **279.53 ms（−2.5%**，p25 −3.0%）；dict_wordfreq −1.4%、obj_dispatch +0.2%、rec_memo +0.5%、sort_struct −0.2%（噪声）；str_build +1.5% 是噪声 —— 该用例**零匹配**，生成二进制与基线完全相同。六用例合计 −0.2%。quickbench 最好 7 次口径：queue_sim 279 ms、str_build 211 ms。
- **为什么低于交接预期的 20%+**：profile 定位 —— 旧拷贝循环只占 queue_sim 约 6%（生成代码 54.4% 里的一小块），批量版 pa_list_copy_range 现占 4.12%（其中 memcpy + 逐元素 retain 是必要工作），生成代码占比降到 50.8%。真正的大头仍是分配与 RC 生命周期：rc_alloc_ex 10.4%、pa_rc_dec_slow 7%、rc_link+rc_unlink 7.2%、list_destroy 5.7%、pa_tuple_new_filled 4%。下一步候选应瞄准这些（如元组/列表的分配合并、RC 慢路径），而不是继续在拷贝循环上挖。
- **验证**：新增 `tests/run/189_copy_range_idiom.py`（对拍 CPython 逐行一致：跨内联边界、循环变量终值、负索引、越界异常部分结果、别名、非空 dst、裸列表定型、混合种类、求值顺序副作用、不匹配形态退回、生成器、GC churn；ASan detect_leaks=1 干净）。**全语料对照**：tests/run 全部用例分别用 0.271 基线编译器与本编译器编译运行，输出与退出码零差异 —— 惯用法在降级器（-O0/-O2 都生效），difftest 的 O0/O2 对照抓不到这类改动，必须对拍旧编译器或 CPython。差分 190/0，release 与 ASan 套件各 208 ran ok / EXIT=0。

## [0.271] - 2026-11-18

### 小列表 header+buffer 合并分配（sort_struct −20.3%，queue_sim −3.8%，六用例合计 −4.0%）

- **合并分配**：容量 <=8 元素的列表（含 `pa_list_new_filled` / `pa_tuple_new_filled` 定长快路径）把 `data` 缓冲内联在「RC 头 + PyAdamasList」同一个 malloc 块尾部（64+48+64=176 字节），新增 `list_inline_pool`（最多缓存 64 块）。建/销各从两次池操作（`list_pool_get` + `list_buf_get` / `list_buf_put` + `list_pool_put`）合成一次。`rc_alloc` 拆出 `rc_alloc_ex(size, ..., extra)`，`h->size = size + extra`，释放时按 size 路由回对应池；untracked 变体同步（`rc_alloc_untracked_ex`，kind 0 元组用）。GC 记账把内联缓冲计入（原先缓冲字节不入账）。
- **内联缓冲是块内指针，不能 realloc / free**：新增 `list_buf_grow`，append / insert / 切片赋值三处增长统一走它 —— 内联时换 malloc + memcpy（内联区从此闲置，头块销毁时仍整块回内联池，下次复用重新启用），外部缓冲照旧 realloc。`list_destroy` 与 GC 清扫（`gc_free_body`）按 `data == list_inline_at(l)` 跳过块内指针的单独释放；shutdown 清空内联池，ASan 泄漏口径不变。
- **实测（abbench 交替 9 轮中位数，绑核）**：`sort_struct` 335.91 → **267.74 ms（−20.3%**，p25 −21.1%）；`queue_sim` 295.25 → **284.01 ms（−3.8%**，p25 −2.8%）；`str_build` −0.9%、`dict_wordfreq` −1.2%、`obj_dispatch` −0.8%、`rec_memo` +0.6%（噪声）；六用例合计 2171.72 → 2084.95 ms（−4.0%）。quickbench 最好 7 次口径：queue_sim 283 ms、str_build 214 ms。queue_sim 新 profile：`list_buf_get` 从 8% 降到榜外，生成代码占比 47% → 54.4%，GC 仍 0.06%；剩余大头是 `rc_link`/`rc_unlink`（合计 7.1%）与生成代码里的逐元素 append/retain —— 交接文档 §5 的切片惯用法识别是下一步。
- **验证**：新增 `tests/run/188_list_inline_buf.py`（append/insert/切片赋值三条增长路径越过内联边界、堆元素高频建/销复用、列表自环 + GC churn、元组快路径 kind 0/1、混合种类、大列表外部路径；输出与 CPython 逐行一致，单用例 ASan `detect_leaks=1` 干净）。差分测试 189 用例 / 0 不一致，release 套件与 ASan 套件各 207 ran ok / EXIT=0。
- **工具修复**：`abbench.sh` 写死 `taskset -c 0`，在 cpuset 为 24-47,72-95 的容器里 taskset 直接失败，六个用例全量出 ~12.5 ms 的「失败开销」假数字。改成从 `/proc/self/status` 的 `Cpus_allowed_list` 取第一个可用 CPU，测量前自检 taskset 可用性，不可用就报错退出。
- 文档：`docs/runtime.md` 列表节补合并分配布局与增长/销毁/GC 清扫的不变量。

## [0.270] - 2026-11-17

### 字符串/除法/RC 三条热路径内联，单字节 str 接运行期驻留表（queue_sim −32.9%，str_build −55.4%）

- **短串拷贝/比较内联，join 跳过空分隔符，个位数驻留**：新增 `sm_copy_bytes` / `sm_bytes_eq` / `sm_bytes_cmp`，`n <= 8` 时内联成几条 mov、`n > 8` 回落 libc（长串上 libc 的向量化仍最优）；`sm_join_fill` 遇到空分隔符整个跳过 —— 原来 `"".join` 对前 n−1 个元素各做一次 `memcpy(dst, sep, 0)`，空拷贝照样走 PLT 跳板，str_build 累计 **792 万次纯跳板往返**（perf 里 `__memmove` 8.8% 采样的来源）；`pa_str_from_i64` 对 0..9 直接返回 `pa_char_str('0' + v)`，不再每次 malloc + 释放（每轮 60 次 `str(i % 10)`，共 396 万次分配）。
- **常数除数除法用魔数乘法替换 `idivq`**：新增 `AsmGen::gen_div_const` / `gen_div_const_emit`，`/`、`//`、`%` 三个入口都先试它，能化简就完全不生成 `cqto + idivq + div_guard`。perf annotate 指到指令级：`simulate` 里 `t % 3` 与 `t % 7 + 1` 两个 `idiv` 合计占该函数 **47.6%** 采样，`build_line` 里 `i % 10` 占 **71.9%**。算法用 Granlund-Montgomery 的 round-up 魔数按 64 位重推（65 位魔数只存低 64 位的 `m'`），有符号走 `trunc(x/d) = sign(x)·floor(|x|/d)` 在无符号域算；2 的幂单独走移位修正 —— 魔数法在 2 的幂上会退化，第一版没分开就在这里翻车；除数 0 / −1 / INT64_MIN / 超出 int32 范围仍走原通用路径。正确性：**3058 万组 `(x, d)` 对拍 C 的 `/`，零失败**。
- **RC 记账内联**：`RcInc` / `RcDec` / `RcMark` / `RcCleanup` 四个 RtFn 在 `gen_call_rt` 里开专用发射，生成代码不再 `call pa_rc_inc/dec/mark/cleanup`，只有慢路径才调 `pa_rc_*_slow`。根因：四个函数体分别只有 1~6 条指令（`pa_rc_mark` 就是一条 `return temp_len;`），call/ret + 被调用者保存 + PLT 跳板比活本身贵一个数量级，perf 里四个函数合计占 18~20%。慢路径可能触发析构，**必须**补 `spill_fp_slots` / `reload_fp_slots`（xmm 全 caller-saved），快速路径不 clobber 寄存器所以不用落盘。0.240 内联的是运行时内部调用点，这一轮内联的是**生成代码**里的调用。
- **单字节 str 字面量接运行期驻留表**：导出 `PyAdamasStr* pa_char_tab[256]`（`char_tab` 的载荷指针），`pa_static_strs_init()` 在 main 序言调用（依赖 `pa_disable_unknown_index` 的结果，必须排在后面）；`AsmGen::char_tab_literal` 把单字节 ASCII 的 str 字面量生成成 `movq pa_char_tab+N(%rip), %rax`，与 `s[i]` 拿到的是同一个指针；bytes 字面量不参与（`pub` 语义不同）。`gen_cmp` 的 str `==` / `!=` 加指针相等快路径：相等直接出结果，否则才 `call pa_str_eq`。
- **两个 bug（差分测试都没抓到）**：① `str(单个数字)` 返回控制字符 —— `pa_char_str` 的参数是**字符编码**不是数值，0..9 快速路径错写成 `pa_char_str(v)`，`str(7)` 拿到 BEL（0x07）而不是 `'7'`（0x37），`len()` 仍是 1，只按长度看完全正常；② str `!=` 的慢路径忘了取反 —— 第一版只在指针相等快路径处理 `!=`，于是所有「内容不同」的 `!=` 恒为假。两个都是**新加的 `tests/run/187_str_smallint_divconst.py` 抓到的**（`line[i] != "3"` 的累加值从 91 变成 1），说明差分测试有真实盲区：它只比最终输出，187 个用例里没有「`str(单个数字)` 之后再打印/比较」的。
- **实测**：`queue_sim` 420 ms → **282 ms（−32.9%）**，对 Go 10.5× → 7.05×；`str_build` 496 ms → **221 ms（−55.4%）**，4.4× → 1.97×。验证：差分测试 188 个用例 / 0 不一致，`tests/run/*.py` 189 个文件，release 套件与 ASan 套件 EXIT=0。

<!-- 下面 0.244–0.269 是补写：这些版本提交时没有同步更新 CHANGELOG，2026-11-18 从
     commit message 原文回填。数字都来自当时的提交说明，但篇幅比逐条手写的条目短。 -->

## [0.269] - 2026-11-17

### 循环内全局变量寄存器化（微基准 −84%，真实基准无收益）

- 把循环里反复读写的模块级全局变量在循环体内寄存器化，窄版本 `promote_globals_in_loops` 已接入优化管线。
- 微基准 131.1ms → 21.1ms（−84%），但 6 个真实基准合计 −0.5%、优化遍零触发 —— **真实代码上无收益**，这条路线到此为止。

## [0.268] - 2026-11-17

### 字典计数器模式融合（dict_wordfreq −37.7%）

- 识别 `d[k] = d.get(k, 0) + 1` 这类计数器更新，融合成一次哈希查找 + 原地写回，省掉重复查表。
- `dict_wordfreq` −37.7%。

## [0.267] - 2026-11-17

### 短字符串哈希缓存（dict_wordfreq −12.5%，短键场景 −27%）

- 字符串对象新增独立的 `chash` 字段（偏移 32），第一次被字典当键时算好写回；SSO 短串和长串都能缓存。
- 原先短串的哈希每次现算，是字典热路径上的固定开销。`dict_wordfreq` −12.5%，短键场景 −27%。

## [0.266] - 2026-11-17

### 元组/列表构造快路径（sort_struct −42%，queue_sim −16%）

- 新增 `pa_list_new_filled` / `pa_tuple_new_filled`：定点构造 n≤4 的元素时一次调用写完，省掉 N+1 次 `pa_list_append`（每次还要走位图维护和容量检查）。
- `sort_struct` −42%、`queue_sim` −16%。

## [0.265] - 2026-11-17

### 索引访问热路径从 19 条指令降到 9 条（nqueens −8.3%）

- 照 Go 的边界检查结构重排 `index.get` 快路径：无符号比较一次挡掉越界，负索引归一化和浮点元素分派都挪进慢路径。
- `nqueens` −8.3%。

## [0.264] - 2026-11-17

### math.sqrt 内联（照 Go 的 SQRTSD + 冷路径，nbody −2.3%）

- `go tool objdump` 确认 Go 的 `nbody.advance` 里 0 次函数调用，而我们的 `pa_math_sqrt` 每次都 call 并额外溢出 3 个浮点寄存器。
- 改成 `comisd` 判负 → 非负直接 `sqrtsd`、负数才落回调用序列，语义一致。`nbody` −2.3%。

## [0.263] - 2026-11-17

### 图着色分配器（精确活跃集，matmul −8.6% / nbody −3.5%）

- 新增实验性图着色分配器（精确活跃集 + 溢出代价模型），环境变量 `PYADAMAS_RA_GRAPH` 开启，默认关闭。
- `matmul` −8.6%、`nbody` −3.5%。没有照抄 Chaitin-Briggs —— 在 SSA 化的 vreg 上直接建冲突图更简单。

## [0.262] - 2026-11-17

### split 分配器改为增量叠加 + 修两个真 bug（无性能收益）

- 把 split 从「重算整函数位置」改成「在默认分配之上增量叠加」，并修掉两个正确性 bug。
- **性能无收益**：TOTAL +27%。随后 0.269 的池容量实验证明容量本来就不是瓶颈，这条路线放弃。

## [0.261] - 2026-11-17

### 模块缓存键漏掉 rc_split（A/B 测试拿到假数据）

- 模块缓存的键里没有 `rc_split` 标志，于是开关 split 后复用了旧缓存的目标文件 —— A/B 对比测的其实是同一份代码。
- 这类 bug 最危险的地方不是慢，是**让实验结论整个作废**。修完后把 cache key 补上全部影响代码生成的开关。

## [0.260] - 2026-11-17

### live range splitting 基础设施（按块分配槽寄存器）

- WIP：按基本块为槽变量分配寄存器，把「整函数一个位置」放宽成「每块一个位置」。
- 只有基础设施，未做跨块的一致性编排，性能未评估。

## [0.259] - 2026-11-17

### 统一浮点寄存器分配尝试（失败，记录根因）

- 尝试让浮点变量和整型变量共用同一套寄存器分配框架。
- **失败**：浮点槽的提升位置同时被 `spill_fp_slots`/`reload_fp_slots` 的调用点编排依赖，两套机制混在一起会互相覆盖。根因已记录，代码回退。

## [0.258] - 2026-11-17

### 跨块浮点 vreg 提升（修两个真 bug）

- 把浮点栈槽提升到 xmm8–15，按 Load/Store 热度决定提升哪些。
- 修两个真 bug：跨块的浮点 vreg 在块边界没有正确的 spill/reload；提升过的槽和 `spill_fp_slots` 的写出顺序冲突。

## [0.257] - 2026-11-17

### vreg 栈槽改用活跃性分析（修死 vreg 撞槽）

- vreg 的栈槽分配原来只看「变量还活着」，改用活跃区间后不再让两个不重叠的 vreg 抢同一个槽。
- 修掉「死 vreg 撞槽」：一个已经不再使用的临时值覆盖了另一个仍然活跃的值。

## [0.256] - 2026-11-17

### join 不再销毁类型洞

- 控制流汇合点原来会把两侧的类型标注「求交」成洞（unknown），实际上洞一旦产生就再也补不回来。
- 改成保留精确的一侧；只有真正冲突时才落洞。

## [0.255] - 2026-11-17

### 类型洞统一用 fresh_hole，裸容器标注不再比不标注更差

- 所有产生类型洞的地方统一走 `fresh_hole`，避免同一个洞被两处标注复用后互相污染。
- 修掉「显式标注裸容器反而比不标注更慢」这个反直觉现象。

## [0.254] - 2026-11-17

### 裸 list 标注不再退化成 unknown（matmul 3.67x → 2.33x）

- 根因链三个环节都错：`list`（不写元素类型）被当成未知元素类型、元素读写走进保守的 unknown 路径、每次访问都要查 RC 索引确认「这到底是整数还是指针」。
- 修完后 `matmul` 从 3.67x 降到 2.33x。

## [0.253] - 2026-11-17

### 移除异常路径上遗留的调试 getenv

- 异常路径上还留着几处 `getenv` 调试开关，正常执行不会经过，但异常频繁的循环里会被反复调用。
- 改成启动时读一次的静态标志。

## [0.252] - 2026-11-17

### 移除分配热路径上的 getenv 调用（bench_rc_gen 0.87x → 1.13x）

- **这是那一轮性能调查的最终根因**：`rc_alloc` 里每分配一次就调一次 `getenv` 判断诊断模式。
- `getenv` 要遍历环境数组并做字符串比较，65 万次分配就是 65 万次全表扫描 —— 比分配本身贵得多。改成启动时读一次的 `pa_diag_rc_on`。`bench_rc_gen` 0.87x → 1.13x。

## [0.251] - 2026-11-17

### temp_take 栈顶快速路径（扫描步数 −40%）

- `pa_rc_temp_take` 要从栈顶往下找目标值。实测 30 万次 take 共 75 万步、平均 2.5 步。
- 加栈顶直接命中判断：容器构造时「刚 push 就 take」是主流形态，一次比较就结束。扫描步数 −40%。

## [0.250] - 2026-11-17

### str/list 重复运算漏登记 fresh_vregs（inc 减半）

- 同一表达式被重复求值时，IR 里会复用同一个 vreg；但 `fresh_vregs` 没有把复用的那一份登记进去，导致 RC 引用计数被多算一遍。
- 修完后 inc 调用次数减半。

## [0.249] - 2026-11-17

### GC 检查移出分配热路径 + 定位那 10ms 的真实构成

- `rc_alloc` 每次都读 7 个 GC 相关全局变量做判断，合并成一个 `gc_need_check` 标志：为 0 时整块跳过。
- 同时用 rdtsc 逐函数计时定位到那 10ms 的真实构成 —— 不是 GC 本身，是这些判断和诊断钩子。

## [0.248] - 2026-11-17

### 修正 GC 计时口径（增量工作此前完全未统计）

- GC 的计时只在完整收集时累加，增量标记的工作量完全没被统计，于是「GC 只花 0.007ms」这个数字是假的。
- 改成增量步进也计时，得到的才是可以拿来判断优化方向的数。

## [0.247] - 2026-11-17

### RC 冗余消除遍（temp/temp_take 抵消对）

- 降级时产生的 `pa_rc_temp` / `pa_rc_temp_take` 配对里有一部分是纯冗余的（push 完立刻 take，中间没有任何可能抛异常的调用）。
- 新增一个优化遍把它们互相抵消掉，省掉一整对函数调用。

## [0.246] - 2026-11-17

### unknown RC 加小整数短路（bench_heapq 1.71x → 2.00x）

- unknown 值的 dec 要先判断「这到底是整数还是指针」。裸 int64 可能是任意下标/长度，减掉 64 字节头后落在极低地址。
- 加 64KB 下限 + 16 字节对齐双约束先廉价排除小整数，再才查堆链确认。`bench_heapq` 1.71x → 2.00x。

## [0.245] - 2026-11-17

### dict 对象池（bench_rc_literals 1.51x → 1.58x）

- 字符串和列表都已经有按档位划分的 freelist，字典没有 —— 每次 `{}` 都要走一次 malloc。
- 补上字典对象池。`bench_rc_literals` 1.51x → 1.58x。

## [0.244] - 2026-11-17

### 字符串池按档位分层（textwrap 133ms → 113ms）

- 字符串池原来所有长度共用一档，短串被切成大块浪费、长串又命中不到合适档位。
- 改成按长度分档，小档（≤40 字节）放宽到 512 块、其余 64 块。`textwrap` 133ms → 113ms。

## [0.242] - 2026-11-17

### RAII 推广到容器与字符串方法

- 按「多出口 + 需要清理」筛选，转换 6 个函数：`jp_value`（list/dict/tuple 三分支）、`pa_itertools_product`、`pa_dict_items`、`pa_dict_copy`、`pa_str_method`、`pa_bytes_method`。合计 `PA_PIN_SCOPE` 13 处、`PA_FREE_SCOPE` 2 处，手工 `pa_gc_pin` 从 71 降到 62。
- 新增 `PA_FREE_SCOPE`：同样的 RAII 思路但清理动作是 `free`，管理 calloc/malloc 的裸数组。`pa_itertools_product` 原来在末尾手工 `free(idx); free(vals);`，中途加提前返回就会漏。
- 按标准跳过：`buf_append` 的 pin 是静态分析误报（实际是 `realloc`）、`pa_asyncio_create_task` pin 后只有一个 return、其余 62 处都是单出口或极短函数。

## [0.241] - 2026-11-17

### RAII 作用域守卫（GCC cleanup 属性）

- **问题**：运行时「保命对象」要手工配对 `pa_gc_pin`/`pa_gc_unpin`。`jp_value` 有 11 个 return、4 处 pin，每条出口都要人工保证配对 —— 0.233 就漏过一处（tuple 分支），造成 96 字节泄漏。
- **做法**：C 没有析构函数，但 GCC/Clang 有 `__attribute__((cleanup(fn)))` —— 变量离开作用域时自动调用。`PA_PIN_SCOPE(var)` 自动 pin，任意出口自动 unpin；`PA_PIN_RELEASE(var)` 交出所有权时解除守卫。
- **两层保障**：RAII 管正常路径，临时栈管异常路径（longjmp 不触发 cleanup 属性，这是 C 语言限制）。
- **效果**：`jp_value` 手工 `pa_gc_pin` 4 → 0。实验验证：故意漏写 `PA_PIN_RELEASE` 时 cleanup 仍自动兜住，不泄漏。
- **代价实测**：性能零损失（`bench_json` 8ms vs 8ms）；代码 +4%（151 → 157 行）。

## [0.240] - 2026-11-17

### inc/dec 内联（抄 Swift ARC 的编译期内联思路）

- `pa_rc_inc` / `pa_rc_dec` 从跨编译单元函数调用改成头文件 `static inline`：快路径（计数 > 1、不在收集期、非统计模式）内联展开，慢路径（归零释放 / 收集期 / 统计）调 `pa_rc_*_slow`。
- 运行时内部上千处调用点通过宏 `pa_rc_inc(p) → pa_rc_inc_fast(p)` 重定向；`rt_core.c` 用 `PA_RC_NO_INLINE` 提供生成代码调用的外部符号。
- **机制验证**：`rt.o` 里 `pa_rc_inc`/`pa_rc_dec` 未定义符号引用 **239/236 → 0**。
- **性能**：`bench_rc_gen` 76ms → 75ms（约 1.3%）。幅度小，因为 RC 调用几乎全在运行时库内部（IR 里只有 5 个 inc / 12 个 dec，实际执行 30 万 / 95 万次）。
- **试过无效**：方案 D 扩大对象池（64 → 512 → 2048）。class 3/6 溢出清零，但 class 5（size 41-48）仍 98% 溢出。单独跑 `s = 'x' * 16` 时溢出是 0 且快于 CPython（17ms vs 19ms）—— 问题是**释放时机**不是池容量。已回退。
- **评估后放弃**：方案 C 静态消除冗余 inc/dec —— IR 里只有 5 个 `rc_inc` + 12 个 `rc_dec`，编译期能消除的上限是这 17 个操作，优化空间 < 0.001%。

## [0.239] - 2026-11-17

### 增量清扫（照抄 Go 并发 GC 结构）

- 先读 Go 官方 [GC guide](https://go.dev/doc/gc-guide) 与 `src/runtime/mgcmark.go`、`mwbbuf.go` 源码，再动手。照抄三点：① 增量驱动（清扫拆成有界步数，挂在 `rc_alloc` 上，即 Go 的 mark assist 思想）；② allocate-black（标记期间新分配的对象不参与本轮）；③ 写屏障 `pa_gc_write_barrier`。
- **本地化优化**（Go 没有对应物）：① **标记同步、只分片清扫** —— 试过标记也分片，但 RC + 增量标记有硬冲突：标记期间对象可能被 RC 释放、队列还指着它，即使登记 `gc_pend` 延后仍有对象在收尾后才归零（`174_gc_construct_roots` 漏 21KB）。Go 没这问题是因为它根本没有 RC。② 小集合（< 2 万）同步清扫，避免分片打断分配路径缓存局部性。③ 倒计数器，热路径只有三条指令。
- **效果**：最大停顿 5.09ms → **1.68ms（-67%）**；GC 总时间 12.99ms → 0.62ms；吞吐 73ms → 75ms。
- **过程中修的三个真 bug**：① `gc_incr_finish` 里 `old_bytes +=` 写成累加（gc_set 是本轮全部存活对象，必须覆盖）；② 同步清扫分支漏设 `gc_incr_major`，导致 `alloc_since_major` 永远停在阈值之上、**每次分配都触发 major**（`180_codepoint_perf` 触发 28 万次 GC、开销 72ms 占程序 46%）；③ `gc_pend` 处理必须在 `gc_phase` 复位**之后**，否则 `pa_rc_dec` 又登记回去形成自循环。

## [0.238] - 2026-11-17

### minor 阈值改用 Go pacer 公式

- Go 的目标堆公式：`Target heap = Live heap + (Live heap + GC roots) * GOGC / 100`，即阈值应按存活堆比例算而非固定值。原来 `young_threshold` 写死 2MB。
- 每轮收集收尾时重算：`target = live + (live + roots)`（GOGC=100），下限 2MB（对应 Go 的 4MiB 最小堆）。
- **效果**：GC 次数 34 → 7（minor 31 → 4）。总耗时不变 —— 剩下的是 3 次 major，minor 本来只有 0.4ms/次。

## [0.237] - 2026-11-17

### 按需关闭索引维护（`bench_rc_gen` 0.75x → 0.90x）

- 三步定位：① 关掉 GC 跑 73ms（GC 占 11ms）；② 关掉**索引维护**跑 80ms（原本 102ms，索引维护占 22ms）；③ 但直接关掉会让 45 个用例失败。
- **做法**：新增 `ir_module_can_skip_index_maint()`，与 `needs_unknown_index` **正交** —— 后者回答「索引要不要建」，前者回答「建了有没有人查」。运行时加 `pa_set_index_readonly()`，只关维护不动查询语义。
- **判据是试出来的**：只排除生成器/协程 → 45 个失败；+ 排除 stdlib 导入 → 4 个；+ 只放行单函数模块 → 1 个（`86_popen` 字符串被多包一层引号，`IsStr` 失效）；+ 全局变量只允许内置类型 → 0。最后一条关键：`86_popen` 的全局是 `subprocess.Popen`，`m.imports` 记不到内置模块。

## [0.236] - 2026-11-17

### 按位置放宽 exact-RC 判定（exact-RC 用例 41 → 50）

- `ir_module_needs_unknown_index` 原来只要模块里任何位置出现 unknown 就整体退化。实测这个判定过宽：走 exact-RC 能快 33%（83ms → 56ms，追平 CPython）。
- **按位置区分**：裸 unknown 在槽/返回值/参数上时必须保留索引；但 `list[unknown]` 这种「unknown 只在容器元素位置」的情况，0.235 的 `heap_bits` 已在写入点按静态类型记录了每个元素是不是堆指针，销毁时不需反查。
- **试过但不安全（已回退）**：「`maybe_unknown` 的全局变量也可以放宽」—— `unknown_is_heap` 在索引关闭时靠 `marked` 位和遍历堆链判断，对象已被 GC 释放后读的是悬空内存，ASan 实测 `heap-use-after-free`。
- **同时修的真 bug**：`pa_repr_tuple` 每轮拼接用的临时字符串 `s` 用完没 dec。保守模式下有 GC 兜底看不出来，exact-RC 下字符串是 untracked leaf 直接泄漏（`75_tuple_repr` 96 字节）。

## [0.235] - 2026-11-17

### 容器逐元素堆位图（`dec_unknown` 降 96%）

- 给 `PyAdamasList` 加 `heap_bits`：写入时按**写入点的静态类型**记录「这个元素是不是堆指针」，销毁时按位图直接 `pa_rc_dec`/跳过，不必走全局 rc_index 反查。
- 新增 `pa_list_append(l, v, heap)` 第三参数，由 codegen 按静态类型给出：0 非堆 / 1 堆 / 2 unknown。出现 2 时整个列表放弃位图退回旧路径。
- **效果**：`bench_rc_gen` 的 `dec_unknown` **467613 → 17418（-96%）**。**但总耗时没有改善** —— 这否定了「哈希索引查询是瓶颈」的假设。之前测出的「跳过判定省 21ms」来自一个段错误的构建，计时无效，已作废。
- **保留理由**：方案 B 需要这个能力（容器元素在没有全局索引时也要能正确 dec）。
- **过程中修的两个真 bug**：① `gen_make_list` 直接发射 `pa_list_append` 但只设 2 个参数，第 3 个寄存器是垃圾值 → tuple 构造后按错误类型 dec，段错误；② `gc_free_body` 只 `free(l->data)` 漏了 `free(l->heap_bits)`，GC 清扫路径不走 `list_destroy`，15 个用例泄漏。

## [0.234] - 2026-11-17

### GC 零回收退避（容器段 major 从 8211 次降到 22 次）

- `bench_rc_gen` 比 CPython 慢，但单独测三个组成段都不慢 —— 是 GC 的组合效应。开 `PYADAMAS_GC_STATS` 看到容器段 `runs=8211 minor=3 major=8208`，每次 `set=5`、`freed=0`。
- **根因**：阈值算法 `mt = old_bytes * 2` 本该有 8MB 绝对下限，但容器段 `old_bytes` 只有 520 字节，每轮 major 都零回收，`alloc_since_major` 很快又越过阈值 —— 死循环式空扫。
- **修法**：上一轮 major 零回收时阈值提到 `old_bytes * 8`，**同时保住 8MB 绝对下限**（没有下限时 `old_bytes*8` 算出来才 4KB，比一次分配还小，等于没退避）。
- **效果**：容器段 `gc_runs 8211 → 22`，`total_ms 2.484 → 0.024`。

## [0.233] - 2026-11-16

### 混合容器元素在 exact-RC 模式下永远不释放（泄漏 1 → 0）

- **根因**：`json.loads("[1, \"x\"]", (0, ""))` 的描述符 `(tis)` 推断出 `elem_kind=2`（混合），析构走 `pa_rc_dec_unknown`；而编译器判定程序「没有 unknown 值」时会调 `pa_disable_unknown_index()`，`rc_index_has` 恒返回 0 —— dec 被静默跳过，元素永久泄漏。
- **修法**：`rc_index_off` 时用三条硬约束做回退判断，绝不误判整数——① 16 字节对齐（`rc_alloc` 走 malloc）；② `marked == PYADAMAS_RC_UNTRACKED`（exact-RC 下字符串 / range / struct 是 untracked leaf，`rc_link` 置 2 且不挂链）；③ 其余遍历 `heap_head` 确认链表成员。0.221 试过的「对象头自洽性」判据会段错误（整数很容易凑出看起来合法的头），已否决。
- **tuple 异常安全**：`jp_value` 的 tuple 分支只有 `pa_gc_pin` 没有 `pa_rc_temp`（list/dict 分支都有），`jp_expect(']')` / 长度不匹配抛异常时容器泄漏；补齐并在正常返回点 take。
- **定位方法**：分配序号 + 按时间顺序消费 RELEASE 的严格时序配对，不受地址复用干扰。
验证：普通全量 205 ran ok；ASan 全量 205 ran ok；O0/O2 差分 188 用例 0 不一致；**泄漏扫描 0 处 / 188 用例**（从 0.217 的 23 处一路清零）。

## [0.232] - 2026-11-16

### json 解析器异常路径上的容器泄漏

- **根因**：`jp_value` 创建 list/dict 后只做 `pa_gc_pin`（防 GC 清扫），**没有 `pa_rc_temp`**。`pa_gc_pin` 只影响 GC，异常展开时临时栈的 RC 清理看不到它 —— 抛异常跳过正常返回路径后容器永不释放。gdb 分配回溯给出确凿证据：`pa_list_new ← jp_value ← pa_json_loads ← __pa_main (53_json.py:24)`。
- **修法**：容器创建后立刻 `pa_rc_temp(out)`，正常返回前 `pa_rc_temp_take(out)` 把所有权转给调用者。正常路径行为不变，异常路径由展开的 cleanup 释放。覆盖 list/dict 两个分支的 4 个返回点。
- `34_exception_chain` 的 45 字节同类泄漏一并消失。
验证：普通全量 205 ran ok；ASan 全量 205 ran ok；差分 188 用例 0 不一致；剩余泄漏 1 处（`53_json` 96 字节）。

## [0.231] - 2026-11-16

### 诊断工具改用线性扫描（修开放寻址导致的归属错乱）

- **两类错乱**：① 插入时复用「地址不同」的 DEAD 槽，新记录落在 `hash(旧地址)+k`，而 `diag_find(新地址)` 从 `hash(新地址)` 起探测 —— 找不回来，表现为「刚 alloc 完，紧接着的 temp-push 报 UNTRACKED」；② 同地址遇到 LIVE 时原地复用，把还在世的对象挤出表，退出汇总漏报。
- **修法**：活跃对象只有几十个，O(n) 线性扫描完全够，且没有开放寻址的不变量问题（只在诊断开启时走）。`diag_insert` 收紧为：同地址 + DEAD 原地复用；同地址 + LIVE **不复用**、分配新槽；地址不同 + DEAD 链尾兜底复用并计 overflow。
- 顺带修：`temp-push` / `temp-pop` 原用 `what[0] == 'p'` 判断（两者都以 p 开头），改看第二个字符；`UNTRACKED` 现在打印 RC 头（kind/rc/size），用来区分「被 MIN/KIND 过滤掉」与「根本没走 rc_alloc」。
验证：普通全量 205 ran ok；ASan 全量 205 ran ok；差分 188 用例 0 不一致；工具关闭时零 debug 输出。

## [0.230] - 2026-11-16

### 诊断工具补 temp 栈事件 + 两个陷阱的记录

- **临时栈事件**（`temp-push` / `temp-pop`）：对象进栈后由 `pa_rc_cleanup` 统一释放，「进了栈却没人 pop」光看 alloc/free 看不出来，现在能直接看到配对是否完整。
- **未追踪指针报警**：`temp-push` 收到不在表里的指针时明确报 `UNTRACKED`，不再静默跳过。
- **`pa_diag_snapshot(when)`**：任意时刻快照存活对象数，区分「真的没人释放」与「由 shutdown 统一释放」。实测 `53_json`：`gc_shutdown` 前存活 5 个、之后 3 个 —— 2 个由 GC 释放，剩 3 个是 untracked leaf（不在 GC 堆链上），只能靠 RC。
- **两个陷阱写进 docs/development.md**：① 地址复用会让事件归属错乱，怀疑某对象时要把**地址**拿去 grep 全部事件，看是否与其他对象生命周期重叠；② `cmake --build | grep error` 在 ninja 并行下会吃掉编译失败（`-Werror=unused-variable` 让 `rt_json.o` 一直是旧的），应该看 `tail` 或退出码。
验证：普通全量 205 ran ok；ASan 全量 205 ran ok；差分 188 用例 0 不一致；泄漏 1 处（`53_json`）。

## [0.229] - 2026-11-16

### 运行时诊断工具独立成模块（rt_diag.c）

- 0.227 的追踪代码塞在 `rt_exc.c` 里只管异常对象，这一版独立成 `runtime/rt_diag.c`，能力扩到所有 RC 对象。开关：`PYADAMAS_RC_TRACE=1` / `PYADAMAS_RC_TRACE_MIN=N` / `PYADAMAS_RC_TRACE_KIND=K`（str/list/dict/obj/struct）/ `PYADAMAS_EXC_TRACE=1` / `PYADAMAS_DIAG_DEPTH=N`（默认 3）。不设变量时零输出、零可测开销（一次 getenv + 一次自增）。
- 输出直接给出归因：`[rc#20] alloc str 32B rc=1 <- str_alloc_raw <- str_new <- pa_json_loads <- __pa_main (53_json.py:20)`，退出时汇总未释放对象。
- **四个设计决定**：① 外部哈希表，不改结构体布局（加字段会让对象 126 → 142 字节，ASan 数字跟着变、前后对照失效）；② 调用点归因走 `_Unwind_Backtrace`（`__builtin_return_address(n>0)` 要编译期常量且被 `-Werror=frame-address` 拦），按 `.eh_frame` 逐帧还原并用生成代码的 PC 表拿到文件名行号，链接加 `-rdynamic`；③ 表满不静默丢弃，溢出置标志并在退出时提示「结果不完整」；④ 地址复用不算新对象，同地址 DEAD 原地复用（否则误报 14 个），但绝不覆盖 LIVE 槽（否则真实泄漏消失）。
- **踩到的坑：热路径上不能加钩子**。把悬挂指针检测放在 `pa_rc_inc` / `pa_rc_dec` 入口，`88_defaultdict` 的 `f["v"] += 1.5` 输出从 `1.5` 变成 `-1.6882786079646144e+260`。二分锁定到 `pa_diag_check_use`：它只是一个普通函数调用，但在这两个会被内联的极热函数里多一次调用就改变了寄存器分配，而生成代码对 `%xmm0` 的存活假设太脆弱。钩子只能放在非热路径（`rc_alloc` 入口、真正释放的那一支）。
验证：普通全量 205 ran ok；ASan 全量 205 ran ok；差分 188 用例 0 不一致；工具关闭时零 debug 输出；泄漏 1 处（已精确定位到 `53_json.py:20`）。

## [0.228] - 2026-11-16

### 生成器帧槽里的异常对象没人释放（泄漏 2 → 1）

- **诊断先定位再修**：`PYADAMAS_EXC_TRACE` 打出 `119_gen_except_suspend` 的轨迹 `alloc → leave(写进帧槽 s1) → leave(同槽再写) → LEAK`；帧槽快照显示生成器已正常终止（status=1）但 `s1`（exc_cur_slot）里仍存着异常对象。
- **两处机制都漏了它**：① `gen_finish` 的顺序是 `release_slots()` 然后 `exc_leave()`，写回槽的那一刻释放已跑完，而 codegen 的帧销毁开头 `cmpq $1, 8(%rbx); je .Lgengcreldone` 对 status==1 短路跳过整块槽释放（对 RC 槽是对的，对异常槽不是）；② `exc_cur_slot` / `exc_top_slot` 类型被推断成 int（存裸指针），`lower_gen_async.cpp` 的 `if (!ir_is_heap(ty)) continue;` 跳过它们，`slot_heap` 为 0，释放循环也不生成代码。
- **修法**：`exc_cur_slot` 标 `slot_heap=3`、`exc_top_slot` 标 `4`；codegen 把 `slot_heap >= 3` 的释放移到 status 检查**之前**无条件执行；`pa_gen_slot_release` 对 own==3 走 `exc_free`、own==4 走 `pa_exc_free_save_chain`（都不能 `pa_rc_dec`，那会按 RC 头读到 malloc 块前面）；`pa_rc_shutdown` 退出前把临时栈残留值 dec 干净。
- **为什么前几轮失败**：0.226 之后在这个 126 字节上盲试三次（slot_heap 标记、生成器 owner 登记表、调整 gen_finish 顺序），每次都引入新的 heap-buffer-overflow / use-after-free 并回退。差别在于当时不知道对象在哪一步丢掉，只能猜所有权。
验证：普通全量 205 ran ok；ASan 全量 205 ran ok；差分 188 用例 0 不一致；泄漏扫描 **1 处 / 188 用例**（`119_gen_except_suspend` 126 字节 → 0）。

## [0.227] - 2026-11-16

### 异常对象生命周期追踪工具（PYADAMAS_EXC_TRACE）

- **动机**：0.226 后剩两处泄漏，盲试多轮（改 slot_heap 所有权标记、加生成器 owner 登记表、调整 gen_finish 顺序）每次都引入新内存错误并回退。结论：异常对象的所有权在 `pending_exc` / `current_exc` / 生成器帧槽 / 临时栈之间反复转移，光靠读代码和 ASan 的字节数推不出来。
- **工具**：`PYADAMAS_EXC_TRACE=1` 把每个 `PyAdamasException` 的 `alloc` / `free` / `leave`（写回帧槽 = 所有权转移）/ `enter`（取回）逐条打到 stderr，退出汇总 `LEAK`。追踪表是外部表（指针 → 分配序号），不在结构体里加字段；不设变量时只有一次 getenv 和一次自增，实测全量测试与泄漏字节数与加入前完全一致。
- **直接给出的结论**：`119_gen_except_suspend` 的两个泄漏对象轨迹是 `alloc → leave → leave → 再无任何事件`，即写进帧槽后再没被取回也没被释放 —— 问题确定在「生成器销毁时帧槽里的异常对象没人还」，排除了 RC 配平方向。
验证：普通全量 205 ran ok；ASan 全量 205 ran ok；泄漏 2 处（与加入工具前一致）；docs/development.md 新增追踪一节。

## [0.226] - 2026-11-16

### 零开销 raise 路径的收尾责任（泄漏 3 → 2）

- **根因**：0.225 让 `terminated()` 认出不返回的运行时调用后，`lower_try` 不再在 except 体后补 `ExcDone`（补了也是死代码）。这本身是对的，但**收尾责任落到那些 `_Noreturn` 函数身上**，而它们没做 `pa_exc_drop_save()`（还 except 入口压的 `PyAdamasExcSave`）和 `exc_free(current_exc)`（`pa_exc_accept_value` 接管成 `current_exc`，原由 `ExcDone` 负责 free）。运行时计数 `accept=7 done=5` 差 2 个。
- **修法**：三个零开销入口都补收尾——`pa_eh_raise`、`pa_eh_raise_value_from`、`pa_eh_reraise`。（setjmp 路径的 `pa_reraise` 在 0.225 已修。）
- `34_exception_chain` 45 B → 0、`39_match_advanced` 47 B → 0。
- **回退的方案**：`pa_raise_value_from` 里保存/恢复 `pending_exc_args` 对 119 无效（126 字节不变），且 34/39 的修复不依赖它，保持最小改动。
验证：全量 205 ran ok；差分 188 用例 0 不一致；泄漏扫描 188 用例 **2 处**（`119_gen_except_suspend` 126 B、`53_json` 96 B，均需改生成器销毁路径 / GC 交互，量级更大）。

## [0.225] - 2026-11-16

### terminated() 漏认不返回的运行时调用（死代码吞掉清理）

- **根因**：`Block::terminated()` 只认控制流指令，而 `pa_reraise` / `pa_raise*` / `pa_eh_*` 都是 `_Noreturn`。于是 `lower_try` 认为「块还没终止」，在 `pa_reraise()` **后面**又追加 `pa_exc_done()` + `jump` —— 永远执行不到却被当活代码保留，其中的清理动作静默丢失。运行时计数 `accept=6 done=5 reraise=0`，尽管 IR 里明明有 5 处 reraise。
- **修法**：① `terminated()`（ir_types.h）补上 `ExcReraise` / `Raise` / `RaiseValue` / `RaiseValueFrom` / `RaiseInstance` / `Reraise` / `EhRaise` / `EhRaiseValueFrom` / `EhReraise` / `EhRethrow`；② 新增 `pa_exc_drop_save()`，供 setjmp 的 `pa_reraise` 与零开销的 `pa_eh_rethrow` 在跳转前还掉 save 节点。
- **踩过的坑**：`pa_exc_drop_save` 最初实现成「还 save 节点 + `exc_free(current_exc)` + 恢复外层值」，引入 2 个回归（`No active exception to reraise`、抛错对象）。裸 `raise` 重抛的**正是** `current_exc`，不能在它之前 free；改成只还 save 节点。
- `32_exception_value` 16 B → 0。剩余 3 处经 `git stash` 验证在 HEAD 上同样存在，非本次引入。
验证：普通全量 205 ran ok；差分 188 用例 0 不一致。

## [0.224] - 2026-11-16

### os.path.split / raise 收尾 / 异常上下文链（泄漏 5 → 2）

- **① `pa_os_path_split` 多 inc 一次**（rt_os.c）：与 0.220 pathlib 同类，`str_new` 返回的串本来就带 rc=1（产生者持有），`pa_list_append` 只存指针不加引用，多 inc 的那份没人释放（`81_os_extra` 泄漏 120 字节，head/tail 各一条）。
- **② `pa_exc_reraise` 不清理 save 节点**（rt_exc.c）：裸 `raise` 直接 `exc_longjmp`，跳过 except 块的 `pa_exc_done`，而 `pa_exc_accept_value` 进 except 体时压的 `PyAdamasExcSave` 必须在这里还掉，否则每执行一次 `except ...: raise` 泄漏 16 字节。
- **③ 异常上下文链被覆盖丢失**：`pa_exc_enter` 把 `exc_save_top` 摘下存进 `n->top`，恢复时直接覆盖，当前那条非空链永久丢失；加 `free_save_chain` 在覆盖前还掉。
- **走错又退回的一步**：给 `lower_raise` 加 `emit_cleanup_exits` 引入 11 个用例回归——有值 raise 的清理把要抛的异常值释放了（`refcount increment on a freed object`），裸 raise 的 ExcDone 把 `current_exc` free 了（`No active exception to reraise`）。结论：**raise 的收尾不能靠通用清理机制**，要抛的对象必须在清理之后仍然有效，正确做法是运行时在跳转前自己收拾。
验证：普通全量 205 ran ok；ASan 全量 204 ok / 1 偶发（`180_codepoint_perf` 性能阈值在 ASan 下的时间抖动，连跑 5 次全过）；差分 188 用例 0 不一致；`81_os_extra` 216 B → 0、`119_gen_except_suspend` 207 B → 0。

## [0.223] - 2026-11-16

### 异常子系统 shutdown 审计（挂起 handler 记录丢失）

- **审计方法**：盘点运行时所有 `static` 堆指针逐项检查清理点。`young_buf` / `gc_work_buf` / `gc_set_buf` / `gc_tab_buf` / `gc_pend` / `list_pool` 已有；`exc_regs` / `handler_freelist` / `suspended_head/pool` / `exc_node_top` 四块**无清理点**。
- **修法 1**：新增 `pa_exc_shutdown`，统一还掉挂起表 → 空闲节点池 → handler freelist → 异常继承登记表 → 上下文嵌套节点，在 `pa_gc_shutdown` 末尾调用。
- **修法 2（关键）**：`pa_handler_suspend` 原来只在 `gen_owner` 非空时登记，但很多 suspend 发生在 owner 为 NULL 的上下文（生成器被 `pa_gen_next_opt` 驱动时 owner 已在上一轮 `gen_run` 结束时清掉）。那些记录摘链后既不在 `handler_top` 也不在挂起表 —— 永久丢失（快照 `handler_top=0 suspended=0 freelist=3` 就是证据）。改成**无条件登记**，owner 为空时挂到「孤儿」owner 上由 shutdown 兜底，生成器销毁按自己的 owner 找，不会误删孤儿。
- `162_pep479` 240 B → 0，总泄漏 5 处 → 4 处。
验证：普通全量 205 ran ok；ASan 全量 205 ran ok；差分 188 用例 0 不一致。

## [0.222] - 2026-11-16

### 生成器 handler 清理统一到 unwind_handlers_to

- 0.219 只在 `gen_run` 的异常路径加了清理循环，但生成器有**三个**退出点会留下 resume 内部压过的记录：`gen_run` 的 `PA_SETJMP != 0` 分支、`pa_gen_close` 的异常分支（finally 里再抛）、`pa_gen_close` / `pa_gen_finalize` 的正常路径。后两个只做 `handler_top = h.prev`，沿途记录既不在全局链上也没人归还。
- 抽成 `unwind_handlers_to(stop)` 三处共用；`pa_handler_discard_record` 内部会摘挂起表 + 检查 `heap_alloc` 再归还 freelist，栈记录与已复用记录都安全。
- **诚实说明**：这个重构让三条路径一致（此前只有一条对），但**没有消除剩下的生成器泄漏**——`162_pep479` / `119_gen_except_suspend` / `32_exception_value` 字节数不变。它们的泄漏栈都指向 `gen_run → pa_gen_next_opt`，说明生成器还在迭代中（`list(g_inner())` 消费到一半），记录挂在挂起表里、对象还没销毁程序就退出了，需要在 shutdown 里清空挂起表（0.223 做）。
验证：普通全量 205 ran ok；ASan 全量 205 ran ok；差分 188 用例 0 不一致。

## [0.221] - 2026-11-16

### json 类型描述符漏认 tuple（混合类型容器元素泄漏）

- **根因**：`desc_heap` 实现是 `return *d == 's' || *d == '(';`，而描述符首字符编码里 **`t`（tuple）漏了** —— tuple 也是堆对象。于是 `(tis)` 的 `all_heap` 变 0、`any_heap` 变 1，`pa_list_new` 拿到 `elem_kind=2`，析构走依赖 `rc_index` 的 `pa_rc_dec_unknown`；这类程序被判定「无 unknown 值」→ `pa_disable_unknown_index` → dec 静默跳过 → 元素永久泄漏。补 `'t'` 后 `elem_kind=1`，析构走 `pa_rc_dec` 正常释放。
- **为什么不去改 `pa_rc_dec_unknown`**：让它在 `rc_index_off` 时靠对象头自洽性（kind/rc 范围 + marked 保留位）判断指针，试过**会段错误**（`53_json` / `89_bytes_final` 整个程序无输出）—— 整数很容易凑出看起来合法的对象头。要正确做需让混合容器在 exact-RC 下也维护元素类型信息（位图或标签），是容器布局级改动。补 `desc_heap` 是在**产生 elem_kind=2 的源头**上修。（0.233 最终用三条硬约束解决了这个分支。）
- `79_datetime_pathlib` 120 B → 0，`141_stdlib_pathlib_extra` 同类问题一并消失，总泄漏 8 处 → 5 处。
验证：全量 205 ran ok；差分 188 用例 0 不一致。

## [0.220] - 2026-11-16

### pathlib 返回值引用泄漏（str_alloc_raw 的产生者引用没人放）

- **根因**：`pa_path_new` 的语义是「接管一个**已有**的字符串引用，内部 `pa_rc_inc` 补一份」，但 6 处调用传的是 `str_alloc_raw` / `str_seal` 的**刚产生**结果 —— 那个串本来就带 rc=1，再 inc 变成 2，产生者那份没人释放（`147_pathlib_extra` / `141_stdlib_pathlib_extra` 各泄漏 128 字节）。
- **修法**：新增 `pa_path_adopt()` 接管刚产生的串、不额外 inc；6 处 fresh 串改用它，2 处传已有串的保持 `pa_path_new`。pin/unpin 两边一致，防止 `rc_alloc` 触发主收集时参数串被清扫。
验证：两处 pathlib 泄漏 → 0；全量 205 ran ok。

## [0.219] - 2026-11-16

### 生成器 handler 记录的生命周期（泄漏 67200B → 0）

- **根因**：handler 记录的完整生命周期是 `push → suspend（yield 时摘链）→ rearm（resume 时压回）→ leave/discard`，但生成器可能**再也不会 resume**（被 close 后丢弃、finally 里抛异常跳出），suspend 过的记录既不在全局链上也没人归还。更糟的是 `pa_handler_free` 是裸 `free(p)`，不进 freelist 也不摘登记表，留下悬空指针。
- **四件事**：① `PyAdamasHandler` 加 `suspend_owner`，suspend 时记下所属生成器，销毁时按 owner 找回（记录在挂起期间保持有效，因为 rearm 用的就是同一地址）；② 全局挂起表，suspend 登记、rearm/leave/discard/free 都 `unsuspend()`；③ `pa_handler_free` 改为「摘挂起表 + 归还 freelist」，这是 123/119 的 use-after-free 根源；④ `gen_run` 异常路径先把 `h.prev` 到 `handler_top` 之间的记录逐个归还。
- **容易踩的坑：栈记录 vs 堆记录**。`gen_run` / `pa_gen_close` / `pa_gen_finalize` / `rt_async` / `rt_agen` 里的 `PyAdamasHandler h` 是栈变量，进 freelist 会造成 stack-use-after-return。加 `heap_alloc` 标记（push 置 1、栈记录置 0，归还前检查），**且必须在所有栈记录处显式初始化** —— 漏一个就读到垃圾值，普通构建直接段错误，而 ASan 构建因内存清零反而看不出来（这个差异一度让 5 个用例只在普通模式失败）。
- `93_gen_exc_rc` 67200 B → 0（300 个生成器 × 224 B）、`123_finally_yield` 224 → 0 并修掉一个 UAF、`162_pep479` 448 → 240，总泄漏 10 处 → 8 处。
验证：普通全量 205 ran ok；ASan 全量 205 ran ok；差分 188 用例 0 不一致。

## [0.218] - 2026-11-16

### ASan 覆盖运行时内部 + 修 json.loads 浮点返回值错位

- **运行时插桩**：之前 `--asan` 只给生成代码插桩，`rt.o` 是预编译零插桩，RC/GC/列表/字典这些**运行时内部**的指针错误完全看不到（0.216 能抓到纯属运气）。新增 CMake 目标 `rt_asan.o`：同样源文件用 `-O1 -g -fsanitize=address` 重编（79 个 ASan 符号），`--asan` 时链接它。代价：纯分配负载 16ms → 27ms（1.68×），测试套件不受影响。
- **立刻抓到一个真 bug**：`PYADAMAS_TEST_ASAN=1` 全量扫描发现 `53_json.py` 输出 `-1.6882786079646144e+260` 而不是 `2.5`。根因在 `gen_call_rt` 的 `raw_ret` 只列了 `DictKeyAt`，而 `pa_json_loads` 的契约是**返回 i64 位模式**（浮点值也塞在 `%rax`），没在列表里于是 codegen 按 double 去读 `%xmm0` 的残留值。普通构建下被寄存器残留掩盖（恰好凑出正确结果），ASan 改变内存布局后稳定暴露。
- **修法**：`raw_ret` 加上 `JsonLoads`；float 类型时先 `movq %rax, %xmm0` 搬位模式（`movsd` 不能直接从 GPR 取源）再按浮点落位。
验证：普通全量 205 ran ok；**ASan 全量 205 ran ok**；差分 188 用例 0 不一致；7 个 GC 复现（gc01/gc02/x01/z01/g04/g05/d04）全部 ok。

## [0.217] - 2026-11-16

### ASan 成为一等公民（CLI + 测试框架 + 修掉 23 处假泄漏）

- **CLI**：`--asan` / `--ubsan` 让生成可执行文件带检查器，编译和链接同时生效，底层仍是 `PYADAMAS_CC_FLAGS`。`ASAN_OPTIONS=detect_leaks=0` 是有意的默认——GC 程序里「故意留到进程退出」的对象很多，泄漏报告会淹没真正的 use-after-free。
- **测试框架**：`PYADAMAS_TEST_ASAN=1 PYADAMAS_TEST_JOBS=4 bash tests/run_tests.sh`，188 个用例全跑 ASan，慢 3-5 倍，改动触及 RC/GC/lowering 时用。首次全量扫描 **0 处内存错误**。
- **修掉 23 处假泄漏**：开 `detect_leaks=1` 扫 108 个用例报 23 处 Direct leak（各 120-464 字节），全是进程退出时还挂在分配器缓存里的块。两个来源：`str_pool` / `list_pool` / `list_buf_pool` 回收时不立刻 free；`sys.argv` 挂在全局根上永不 dec，而 exact-RC 下字符串**不进 GC 堆链**（`rc_link` 对叶子直接 return），`pa_gc_shutdown` 的 `while (heap_head)` 扫不到。修法：shutdown 清空三个池 + 新增 `pa_sys_argv_shutdown` 在释放堆之前交还 argv 引用。**23 处 → 5 处**。
- **文档重写**：docs/development.md 的「内存 / UB 检查」整节删掉手工编 rt.o / cc 包装脚本 / 改 PATH 的老流程，改成三条命令，并把 0.216 的定位过程写进去当例子。
验证：全量 205 ran ok；差分 188 用例 0 不一致。

## [0.216] - 2026-11-16

### unknown 槽被提升导致 GC 根缺失（use-after-free 完整修复）

- **问题**：`def f(): u = ident([...]); return u[0]` 在 major GC 后堆损坏 / SEGV，`PYADAMAS_GC_DISABLE=1` 完全正常，`-O0` 也崩。
- **根因（ASan 抓到第一现场）**：ASan 报 `SEGV on address 0xffffffffffffffe6`（= -26）在 `pa_rc_temp`。`u` 是 unknown 槽，被寄存器分配提升到 `%r12`，而帧记录登记的是栈槽 `48(%rsp)` —— 栈槽自序言清零后再没被写过，GC 读到 0 不标记，`u` 被回收，之后 `index.get` 从已释放列表取元素得到 -26。`PYADAMAS_GCSLOT_DEBUG` 实证：`s1 ty=unknown scan=1 reg=%r12`（该留内存却拿了寄存器）且与 `s3 ty=int` 撞同一个寄存器。
- **三处修复**：① `build_slot_liveness` 候选表排除 unknown 槽（原来只排除堆槽 / 取过地址 / handler hazard）—— unknown 可能是堆指针，GC 要靠栈槽地址找根；② `gc_scan_slot_` 改在寄存器分配**之前**按静态类型算好，消除「先分配再登记」的循环依赖（槽一旦被提升就既不在栈槽也不在扫描清单里）；③ `can_fuse` 禁止融合被 GC 扫描的槽（融合会让栈槽从此不再更新）。
- **顺带**：`driver_build.cpp` 的 `cc` 调用支持 `PYADAMAS_CC_FLAGS`，编译和链接共用。这个 bug 靠它才定位到 —— IR 层面完全看不出问题。
验证：全量 205 ran ok；差分 188 用例 0 不一致；`/home/lenovo/wb_repro/` 的 7 个 GC 复现全部通过。

## [0.215] - 2026-11-16

### unknown 返回值的引用所有权缺失（use-after-free 根因）

- **问题**：`def f(): return a[0]` 里 `a[0]` 静态类型是 unknown 时返回**不加引用**，调用方拿到借来的指针；被调函数返回后那份引用被临时栈清理放掉，对象被回收。`PYADAMAS_GC_DISABLE=1` 下完全正常（对象不在池里被复用，问题被掩盖）。
- **两处同一类不对称**：① `emit_return`（lower_impl_slots.cpp）的 `ir_is_heap(unknown)` 是 false，unknown 返回值什么都不做 → 改走 `RcIncUnknown`（先查堆索引确认是堆指针再 inc，整数安全）；② `retain_for_store`（lower_impl_util.cpp）在 dst 与 src **都是 unknown** 时什么都不做 → 补 `rc_inc_unknown`。`u = ident([...])` 正是「无标注函数返回未知类型、存进未知槽」。
- **效果**：6 个 GC 复现里 4 个修复（gc01_min_uaf、z01_global_root、g04/g05_unknown_root_hold），2 个从「堆元数据损坏 / 静默算错」变成可检测的 panic（gc02_uaf_junk、x01_only_u_root）。
- **诚实说明剩余缺口**：`release_slots` 只释放 `ir_is_heap(ty)` 的槽、unknown 槽整个跳过（`release_globals` 是对称的）。直接补齐会让 12 个用例 over-release —— unknown 槽里的值未必被 retain 过（装饰器回填、borrow_slots、帧槽别名都会让它持有借来的值）。正确做法是槽级「是否 owned」标记，不在这一版做。
- **顺带修测试框架撑爆磁盘**：`tests/run_tests.sh` 输出无大小限制（某次 `78_enumerate_zip.py` 死循环写了 **19G** 日志）+ `testlogs.$$` 从不清理，`build/dev` 涨到 **32G**。改为 `head -c 1048576` 截断（截断后 diff 必然不等，照样报 OUTPUT DIFF）+ `trap cleanup_logs EXIT INT TERM`。
验证：全量 205 ran ok。

## [0.214] - 2026-11-16

### join(T, None) 返回 Optional[T] 而不是 unknown（4.7×）

- **根因**：`join_into` 在 Optional 分支之后、兜底之前有一条 `if (a->kind == None || b->kind == None) return t_unknown();`，于是三元表达式、if-else 合并、列表字面量里**只要掺了 None 就全退化成 unknown**（`v = 1 if flag else None` 应为 `int | None`，`[1, None]` 应为 `list[int | None]`）。
- **为什么是缺口不是权衡**：`Optional[T]` 本身完整支持——标注路径生成的 IR 正确、`is not None` 收窄也正确，只是推断的 join 没走它。改为 `mk_optional(b)` / `mk_optional(a)`。
- **实测**：300 万次热循环（`v = i if i % 2 == 0 else None` + `is not None` 收窄）34-36ms → **6-7ms，约 4.7×**。asm 层面 `pa_rc_inc_unknown` / `pa_rc_dec_unknown` 从 16 处降到 0 —— unknown 走保守 RC 路径，每个操作都要额外调用查注册表。
- **来源与后续**：team 的 type-auditor 审查 351 处 `t_unknown()` 时定位（B2）。同批的 B1（调用点缓存重查前的返回类型，3.7×）两种修法都失败回退：重跑模块体会让类定义处理两次生成两个 Type 对象（`cannot assign Point to Point`，47 个用例失败）；就地更新符号类型会破坏 12 个合法重绑定用例。B1 需要完整的「赋值点 → 类型版本」跟踪，是类型系统级改动；B3（hoist 的 unknown 占位污染分支合并）同理。
验证：全量 205 ran ok；差分 188 用例 0 不一致。

## [0.213] - 2026-11-16

### 切片赋值缺少元素类型检查（导致垃圾指针读出）

- **Bug**：`l: list[int] = [1, 2]; l[0:1] = ['hello']` 编译期不报错，`print(l[0])` 输出 `96042627193968` 这类垃圾且每次运行不同（CPython 输出 `hello`）。字符串指针被写进声明为 `list[int]` 的列表，后续按 int 读出就是未初始化内存。
- **根因**：`typecheck_stmt.cpp` 的切片赋值分支只检查「对象是否可变」，注释写着「切片赋值在运行时才展开，类型层面只确认可变」——这个假设是错的；而紧随其后的单元素赋值分支有完整的 `merge_elem` 检查，切片赋值漏了。
- **修法**：`list[T]` 的切片赋值在右值也是 `list[U]` 时比较 T 与 U，复用单元素赋值的 `merge_elem`，报 `cannot assign 'list[str]' to slice of 'list[int]'`。
- **来源**：team 的 runtime-auditor 扫运行时内存管理时发现，独立复现后定位到类型检查器。
验证：错误用例编译期报错；正确用例 `l[0:1] = [9, 8]` 输出 `[9, 8, 2, 3]` 与 CPython 一致；全量 205 ran ok；差分 188 用例 0 不一致。

## [0.212] - 2026-11-16

### 清理 R3 死代码 + 修正误导性的寄存器分配调试输出

- **死代码（净删 35 行）**：`spill_int_caller_slots()` / `reload_int_caller_slots()` 依赖的 `int_caller_slots_` **只被 clear、从未 push_back**，两个函数永远空操作却挂在每个调用点的 spill/reload 路径上；`build_int_caller_mask()` 只有声明没有实现。一并删掉函数、调用点、成员变量、死声明。
- **误导性调试输出（这才是真正浪费时间的）**：`assign_slot_regs_linear()` 末尾统计 `slot_reg_` 里全部非空项，但**浮点槽是在调用方随后才分配的**，此刻浮点项还是空的。于是长期输出 `advance_1: 7/14 slots promoted`，看起来「只提升了一半、还有大量空间」，实际是整型 7/8、浮点 6/6，**已接近饱和**。这个错误数字直接导致「寄存器分配还有很大优化空间」的误判，进而在 vreg 分配上连续多轮投入失败。修正后输出 `[ra] advance_1: int 7/8 promoted, 6 regs`。
验证：全量 205 ran ok；差分 188 用例 0 不一致；删除的是死代码，行为完全不变。

## [0.211] - 2026-11-16

### 两处真实 bug：堆槽 LICM 外提 + RcMark 寄存器化

在尝试重写 RC 临时栈 / 扩展 vreg 寄存器分配的过程中暴露出两个**既有**正确性 bug，平时不发作是因为依赖的栈槽恰好没被覆盖，分配策略一变就变成段错误或静默算错。

- **Bug 1：LICM 把堆槽的 load.slot 提到循环外**。`hoist_loop` 的 `hoistable` 对 `LoadSlot` 只检查「槽本身没被 StoreSlot 写过」，但堆槽内容可以被循环里的 `IndexStore` 间接改写。实测 nbody 的 `energy()` 把 `load.slot s0`（列表 b）提到 `licm.pre`，栈槽被别的 vreg 复用后读到垃圾指针，段错误在 `b[i*7]`。修法：`LoadSlot` 对堆槽一律不外提。
- **Bug 2：RcMark 的返回值被放进寄存器**。`pa_rc_mark()` 返回 RC 临时栈水位，入口 mark、出口 cleanup，中间隔着任意多次调用；它是普通 int vreg，会被按活跃区间复用栈槽或分到 callee 寄存器，一旦离开栈槽某次 call 就冲掉它，`pa_rc_cleanup(mark)` 拿到错误水位并 panic `invalid temporary stack mark`。修法：`vreg_is_rc_mark()` 识别这类 vreg，在 `assign_vreg_slots` 里强制独占栈槽（区间拉满整个函数）。
- **本轮寄存器分配改动全部回退（有数据）**：块内 vreg 进 callee 池让 nbody 差 306、binary_trees 输出 25（应 8191）。根因是 `vreg_off()` 仍指向那个栈槽，任何读栈槽的路径（spill/reload、RC 调用）都拿到过期值 —— 要安全地做必须先重写 RC 临时栈与 vreg 栈槽的耦合，是另一个量级的改动。
验证：全量 205 ran ok；差分 188 用例 0 不一致；z1_rt 116、z2_rt 134、z2_codegen 209、eh 33、sem 42/42 全过。

## [0.210] - 2026-11-16

### 参数 >6 且含浮点的间接调用丢实参（+ 差分测试工具）

- **Bug**：10 个 float 形参的函数经间接调用时第 9、10 个实参丢失。`fargs(1.1..10.1)` 在 -O0/-O1 输出 8.8（只加到第 8 个），-O2 输出 59.6（内联掩盖了 bug），CPython 59.6。
- **两层根因**：① `closure_needs_shim` 对 >6 参数直接 return false，注释写「暂不 shim（退回原行为）」，但原行为并不成立——统一约定是第 i 个实参进第 i 个**整型**寄存器（只有 6 个），真实 SysV ABI 是整型/浮点各自计数、浮点有 8 个寄存器，两者在「>6 参数且含浮点」时不一致，没有 shim 就没人做重排；② shim 读溢出区少算 8 字节——`call` 压入返回地址后调用者的溢出区整体后移 8，漏掉这个偏移就把返回地址当第 1 个溢出实参，最后一个实参永远读不到（正好是 59.6-49.5=10.1）。
- **修法**：去掉 `params.size() > 6` 限制（所有非生成器函数都生成 shim，统一约定才自洽）；`emit_closure_shim` 支持栈上实参（临时区扩成 [0,8n) 存全部实参 + [8n,8n+8*sp) 放真实 ABI 栈传参区，重排后调整 rsp 让栈传参区紧贴返回地址）；溢出区读取补 `+8`。
- **顺带：`tools/difftest.sh`** —— 同一份源码在 -O0 与 -O2 下输出必须一致。这个 bug 就是它抓到的（188 个用例里唯一一个不一致）。优化遍引入静默错误时常规测试未必发现，O0/O2 差分能。
验证：差分 **188 用例 0 不一致**（修复前 1 个）；全量 205 ran ok；五套夹具 z1_rt 116、z2_rt 134、z2_codegen 209、eh 33、sem 42/42 全过。

## [0.209] - 2026-11-16

### IR 重构决策记录（稳定 ID / tag 操作数 / 暂不上 SSA）

纯文档，无代码改动。把 0.205–0.208 四步重构的**理由**写进 docs/ir.md，特别是「为什么不上 SSA」这个容易被反复提起的问题。

- 新增「那为什么不干脆上 SSA」章节：SSA 的三个主要好处（优化遍不用重建数据流、值单一定义、跨块引用不失效）分别由 `uses_of`/`def_of` 索引（0.205）、`Operand` 带 tag（0.206）、`Inst::id`/`Block::id`（0.205/0.208）拿到大部分，不用动前端降级和 phi 构造。真正的分界线是「要不要做需要精确数据流的优化」——现有的遍（折叠、DCE、CSE、LICM、内联、边界版本化）都是局部或结构性的，索引表够用。
- 补充稳定标识章节（含 0.203 边界版本化栽在块号过期上的教训）、索引操作数带 tag 章节（`PYADAMAS_IR_CHECK=1` 校验、隐式转换是刻意的迁移期手段），数据结构表补 `id` 字段。
- 同期另有一次 docs 提交修正 codegen.md 的寄存器分配与 SSA 过时描述，对齐 0.203–0.209。
验证：前四步已各自提交并验证（全量 205 ran ok，五套夹具全过）。

## [0.208] - 2026-11-16

### 块稳定 ID，EhRegion 不再存块号（重构第 4 步）

- **为什么必须做**：`IRFunction::EhRegion` 原存 `int begin_block / end_block / lp_block`（vector 下标），而字段自己的注释就写着「内联等 pass 会重建块表，这里的块号会过期，仅作记录/诊断用」——「明知会过期还存下来」是技术债。0.203 的边界版本化被同一类问题打垮：`version_bounds_loop` 每次插入 3+ 个新块，块号整体偏移，`h=9` 那层循环（含全部列表读写）被永久跳过，99 处边界检查一个没消。
- **改动**：`Block::id`（uint32_t，0 = 未分配，分配器 `next_block_id` 只增不减）；`block_by_id`（id → 当前下标），`block_index(id)` 查不到返回 -1 表示块已删；`EhRegion` 三字段改存块 id，赋值处用 `ensure_block_id()`；`rebuild_use_def()` 顺带重建 `block_by_id`；`new_block()` 从聚合初始化改显式赋值（加字段后聚合初始化会错位）。
验证：全量 205 ran ok；z1_rt 116、z2_rt 134、z2_codegen 209、eh 33、sem 42/42 全过。

## [0.207] - 2026-11-16

### DCE 改用 use-def 索引（重构第 3 步）

- `fold_all()` 的死代码消除原来每轮手工扫全函数统计每个 vreg 的使用次数，现在直接用 `IRFunction::uses_of`（`uses_of[v].size()` 就是 v 被读的次数），语义等价。
- **第 3 步的意义**：前两步（稳定 ID、tag 操作数）是铺路本身不产生收益，这一步才开始消费那套基础设施。后续 `opt_forward` 的 use_count、`opt_inline` 的 uses 计数都能同样接过来 —— 它们现在各自扫一遍，同一个函数在一条 pass 链上被扫十几次。
验证：全量 205 ran ok；编译耗时 matmul 4ms / nbody 6ms / spectral_norm 4ms / nqueens 3ms；**生成代码 md5 与改造前一致**（索引只换统计方式，不应改变任何优化决策）。

## [0.206] - 2026-11-16

### IR 索引操作数带 tag（重构第 2 步）

- `Inst::idx` 从裸 `int32_t` 换成带 tag 的 `Operand`（Kind: None/Slot/Global/Str/Func/Class/Extern/Block/Field）。这是那套 IR 最容易踩的坑：idx 的含义随 op 变（槽号/全局号/字符串号/函数号/类号/外部符号号/字段下标），填错编译器不管，只能靠测试和 dump 发现。
- **隐式转换是刻意的过渡手段**：195 处使用点一次性改完风险太高，保留 `operator int32_t()` 与 `operator=(int32_t)` 让存量代码继续编译，同时 kind 能被记录和校验；等所有消费点改成显式取 kind 后删掉隐式转换，即可让「漏写 kind」变成编译错误。
- **校验**：`idx_kind_ok(op, kind)` 在 `Inst::text()` 里做一致性检查，由 `PYADAMAS_IR_CHECK=1` 开启，迁移期 `kind == None` 放行。本次迁移 19 处降级侧赋值点改显式构造，覆盖 CallDirect / StoreExtern / LoadGlobal / GlobalAddr / SlotAddr / LoadSlot / StoreSlot / CallExtern / GenSetSlot；全量扫描 **0 处不匹配**。
验证：全量 205 ran ok；z1_rt 116、z2_rt 134、z2_codegen 209、eh 33、sem 42/42 全过。

## [0.205] - 2026-11-16

### IR 稳定指令 ID + use-def 索引（重构第 1 步）

- 给 IR 加「稳定标识 + 反向索引」基础设施。优化遍要在指令之间建立引用（可用表达式、死代码判定、循环版本化候选表），而**块内下标和块号都会被 `vector::insert/erase` 与新增块改变** —— 0.203 的边界版本化就是栽在这上面。
- **新增**：`Inst::id`（uint32_t，0 = 未分配，函数内唯一、不随重排而变，分配器 `next_inst_id` 只增不减、删指令不回收，因此任何时刻拿到的 id 都能查到「是否已删」）；`IRFunction` 上三张表 `inst_by_id`（id → (block, index)）、`def_of`（vreg → 定义指令 id）、`uses_of`（vreg → 使用指令 id 列表）；`rebuild_use_def()` 一次 O(指令数) 扫描重建，在结构改动的 pass 之后调用。
- **刻意取舍**：不做 SSA 判定（vreg 本来就可多次定义，`def_of` 只留最后一次定义，索引服务「快速找到使用点」不承担正确性判定）；`inst_by_id` 的位置值会过期，是快速定位而非权威来源，真正的稳定引用是 id 本身；id 只在 emit/def 分配，pass 新建指令走 `ensure_inst_id()`。
验证：全量 205 ran ok（ID 分配不影响语义）；编译耗时 matmul 5ms / nbody 8ms，无可见开销。纯增量，不改变任何生成的代码。

## [0.204] - 2026-11-16

### R4 浮点线性扫描 + matmul 基准改函数形式

- **R4**：抽出通用 `linear_scan_pool()`（整型/浮点共用），浮点不再「按频次挑前 8 个整函数常驻」；`slot_reg_cand_` 同时收两类候选，`slot_cand_is_fp_` 标记，GPR / xmm 两套池分别扫描。修掉一个自引入的严重 bug：区间计算用全局候选下标而传入的是子集，导致 nbody 输出错；改为按 `cands` 建「全局→局部」映射。
- **matmul 基准**：热循环从模块顶层（循环变量是全局变量，分配器管不到）包进 `run()` 变成局部槽，可常驻寄存器。
验证：全量 205 ran ok；四套异常夹具无回退。

## [0.203] - 2026-11-16

### 寄存器分配 R1–R3（活跃性 + 线性扫描 + caller 池）

基于活跃区间给槽位做分配，替代按访问频次挑前 N 个常驻。**正确性收益明确，性能收益为零**：`__pa_main` 有 106 个 call / 42 个块，活跃区间几乎都跨调用点，caller 池条件命中不了，matmul / nbody / spectral_norm / fib / nqueens 的栈访问数一个没变。根因是模块顶层没有函数边界——要提速得先解决这个，而不是继续投入寄存器分配。
- R1 `build_slot_liveness`：块级 live-in/out 位图（`PYADAMAS_RA_DEBUG=1` 可 dump）；R2 `assign_slot_regs_linear`：线性扫描 + 溢出「终点最远」区间；R3 caller 池：块内临时池缩到 r8/r9，r10/r11 只给不跨调用的槽，调用点 spill/reload。
- 途中修两个真实 bug：区间用「首次/末次出现」在 CFG 顺序 + 循环回边下错误（3 挂起 + 4 段错误），改为扩展到 live-in/out 覆盖的整块；SysV 下 r8/r9 是参数寄存器、r10/r11 是闭包/环境指针，全当 caller 池导致 matmul 算错（43198080 → 420883）。
验证：全量 205 ran ok；`PYADAMAS_NO_RA=1` 可回退旧分配器。设计见 [regalloc.md](docs/regalloc.md)。

## [0.202] - 2026-11-16

### Z5：表驱动展开成为默认路径

`--zero-cost-exc` 从默认关闭变为默认路径，另留 `--no-zero-cost-exc` 回退。可挂起函数（生成器/协程）永久保留 setjmp（5 处 C 级兜底抓不到 `_Unwind`，设计上的降级）。收益：全量用例 `_setjmp` 670 → 361（-46%）。沿途修 6 个「两套机制混用」的真实 bug：handler 体里的 setjmp、try 体以 raise/return 结尾、EhEnd 块被当死块裁掉、LP 缺 `pa_eh_leave`、可挂起函数进出未屏蔽 `pa_eh_depth`、早退时 finally 不执行。验证：全量 205 ran ok；z1_rt 116 / z2_rt 134 / z2_codegen 209 / tests/eh 33 / sem_baseline 42/42 全过。

## [0.201] - 2026-11-16

### 基准用例全部补上类型标注

9 个基准用例全部标注为 `list[float]` 等正确形式，输出与 CPython 逐字节一致（裸 `list` 会让元素退化成 unknown，是 nbody `math domain error`、spectral_norm 输出 0 的真正原因）。最终 release 基准见 [bench/compare/RESULTS-release.md](../bench/compare/RESULTS-release.md)：binary_trees 9.20×、fasta 13.14×、mandelbrot 24.16×、spectral_norm 26.51× 对 CPython。

## [0.200] - 2026-11-16

### 恢复四语言经典基准（CPython / PyAdamas / Go / C++）并重跑

从旧提交恢复 `bench/compare/` 的 9 个用例与 `run_compare.py`，装 Go 1.27.4 并加自动探测。定位：与 Go 大致同档，binary_trees / fasta 略快于 Go；matmul / nbody / spectral_norm 落后 3–6×（当时根因归为无寄存器分配，0.203 证实迁移后收益为零，真正瓶颈是模块顶层函数边界）。

## [0.199] - 2026-11-16

### 标准库新增 fnmatch / copy

- **fnmatch**：shell 通配翻译成正则，`*` / `?` / `[seq]` / `[!seq]` 全支持，四入口，8 项输出与 CPython 一致。
- **copy**：静态类型下 `isinstance` 不能收窄成具体容器，提供按类型分开的显式入口 `copy_list` / `copy_dict` / `copy_set` / `deepcopy_list` / `deepcopy_dict`；deepcopy 用「内层长度 + `is` 身份」验证。未实现 `__copy__` / `__deepcopy__` 钩子、类实例字段递归、环检测。全量 205 ran ok；模块表 37 个条目。

## [0.198] - 2026-11-16

### Z5 第一步：模块顶层的 try 也走零开销路径

`__pa_main` / `__pa_init` 是合成函数，模块级语句绕过 `collect_zero_cost_tries`。修复后在模块入口对顶层语句也跑形状判定。186 用例 `--zero-cost-exc` 下 `_setjmp` 582 → 429、仍有 setjmp 的用例 42 → 24；剩余里 22 个含 yield/async，非生成器只剩 with-in-try 与多参 raise 两个已知排除。全量 203 ran ok。

## [0.197] - 2026-11-16

### Z4 完成：try-else 接入；with 在 try 内明确排除

try-else 新增保护区外 else 块，四路与老路径、CPython 一致。with 在 try 内明确排除（证明做不了）：LP 里 `__exit__` 虽生成了正确汇编但运行期没被调用——with 用 setjmp 保护 `__exit__`，与外层 `_Unwind` 混用状态不一致；实测 203 用例中该结构 0 次。z2_codegen 209 项全过。Z4 至此完成：finally、嵌套 try、早退、try-else 全部接入。

## [0.196] - 2026-11-16

### Z4：嵌套 try 接入零开销异常路径

`zero_cost_try`（单指针）改成 `zero_cost_tries`（集合），逐个 try 独立判定；codegen `EhPlanRec` 从一条保护区改成一组 range，同函数多条保护区合成一份 LSDA。with 从「函数体里一个都不能有」改成「只拒绝含 with 的那个 try」。z2_codegen 176 → 198 项全过，全量 203 ran ok。

## [0.195] - 2026-11-16

### Z4：早退（return/break/continue）穿过零开销保护区

`FuncCtx::Cleanup` 加 `ZeroCostEh`（带临时值水位槽），`emit_cleanup_exits` 遇到它发 `pa_eh_leave` + `pa_rc_cleanup`，否则 `pa_eh_depth` 不归零会导致后续异常误走 `_Unwind`。新增 `16_early_exit.py`，z2_codegen 165 → 176 项全过。

## [0.194] - 2026-11-16

### Z4：finally 接入零开销异常路径

实测 112 处 try 里 finally 占 42（最大头）。两个关键 bug：带 finally 的 try 在 `lower_try` 里直接走 `lower_finally` 到不了新路径；finally 不能内联两份到同一落点。改成独立 fin 块，异常路径用 `pa_eh_rethrow` 重启 phase 1，外层仍接得住。z2_codegen 143 → 165 项全过。

## [0.193] - 2026-11-16

### itertools 补 6 个算子（stdlib 自举）

`takewhile` / `dropwhile` / `filterfalse` / `compress` / `pairwise` / `groupby`；`kModuleTable` 里 itertools 从 `Native` 改成 `NativePython`。统一接收 `list`、返回 `list`（非惰性迭代器）。未提供 `starmap` / `count` / `cycle` / `tee`。顺带确认 `collections.namedtuple` 做不了（运行期生成类与编译期布局冲突）。全量 203 ran ok。

## [0.192] - 2026-11-16

### Z3：多 handler 分派链 + typed catch 接入零开销路径

此前零开销只接「单层 try + 一个 catch-all」，185 用例里 0 个能走新路径。去掉 handler 数量/类型限制，except 改成链式分派，全不匹配走 `pa_eh_rethrow`（重启 phase 1，内层不匹配时外层仍接得住）。新增 3 用例，z2_codegen 110 → 143 项全过，全量 202 ran ok。

## [0.191] - 2026-11-16

### Z2-b3：C 运行时抛出的异常也能被零开销 try 抓到

所有抛出点收口到 `rt_exc.c` 的 `exc_longjmp`，新增 `pa_eh_depth` + `pa_eh_enter` / `pa_eh_leave`，按深度二选一走 `pa_eh_throw` 或 `longjmp`。三条 C 运行时异常三方输出逐字一致，z2_codegen 110/110 PASS，全量 202 ran ok。

## [0.190] - 2026-11-16

### 签名无浮点的 shim 退化成单条 jmp

签名无浮点时 shim 只发一条 `jmp .LfN`。间接调用 2000 万次 53ms → 46ms，`22_exception` 的 .text 从 +384 字节 → +64 字节。全量 202 ran ok。

## [0.189] - 2026-11-16

### 形参类型混用（float/非 float）改成编译期报错

`bind_param` 回填分支在浮点/非浮点混用时报错要求标注，消除静默算错。全量 202 ran ok。

## [0.188] - 2026-11-16

### 间接调用统一 ABI 约定

闭包 +0 指向 shim（非函数本体），所有间接调用走统一约定（实参进整型寄存器、返回值 `%rax`），修浮点经 unknown 函数值传递的静默算错。全量 202 ran ok；四套异常夹具无回退。

## [0.186] - 2026-11-16

### 字符串码点语义 S1+S2

[str-codepoints.md](docs/str-codepoints.md) 的 S1+S2：`PyAdamasStr` 24 → 32 字节，`pub`（str 码点数 / bytes 字节数）放偏移 0，`len` 与真值判断指令一行未改；字面量编译期算好 pub；下标/切片/迭代/方法按码点，bytes 保持字节；顺序迭代游标 + 全 ASCII 快路径；新增 `len_bytes(s)`；cimport 取 data 偏移 16 → 24。性能对比 0.184 在 0.87–1.05×，迭代线性。全量 201 ran ok。S3 未做。

## [0.187] - 2026-11-16

### 修 `functools.reduce` 传浮点回调时的静默算错

现象：`reduce(fadd, [1.5, 2.5])` 输出 `4612811918334230528`（= `1.5` 的 IEEE-754 位模式）。

**根因不是跳板缺失**（`pa_call2_f` 早就存在），而是**编译器选错了跳板**：
`lower_functools` 只看结果类型来决定用整型版还是浮点版，而 `reduce` 的结果类型来自
`functools_call` 的 `return elem`；当 `xs` 是裸 `list` 形参或没有标注返回时 `elem` 就是
`unknown`，于是选了整型版 `pa_functools_reduce`，浮点元素的位模式被当整数塞进 `%rdi`。

修复：
- `typecheck_call_modules.cpp`：`reduce` 的结果类型改为按**回调形参类型**推导 ——
  两个形参都是 `float` 且元素类型为 `float` / `unknown` / `int` / `bool` 时，结果类型为 `float`；
- `lower_import_misc.cpp`：`use_fp = 结果类型为 float || 回调两个形参都是 float`；
  元素类型为非数值时给出明确错误，而不是静默算错。

验证：原始例子从位模式变成 `4.0`（带初值 `4.5`），与 CPython 一致；
新增 `tests/run/182_dynamic_call_float.py`；全量 **202 ran ok / 0 FAILED / 0 OUTPUT DIFF**；
四套异常夹具无回退。

影响面已逐个核对（`pa_call1` / `pa_call2` 全部 4 个调用点）：只有 `reduce` 的整型版会传浮点，
其余（Future 回调、`re.sub` 替换回调）形参类型固定，类型检查会拒绝 `float` 标注，无需改。

**仍未修的架构性限制**（不在本任务范围，已在文档登记）：
- `unknown` 形参的函数（`def f(a, b): return a+b`）本身按整数 ABI 编译，被浮点实参调用时仍算错；
- 元素类型为 `unknown` 的裸 list 被按 double 读时，int 元素会读错（`fold([1,2])` 得到 `1.5e-323`）。
  这两个都需要「元素域上的运行期信息」才能修，与 0.187 的选择问题不同源。
## [0.185] - 2026-11-16

### 修用户类一元运算符的 SIGSEGV（`__neg__` 等）

现象：用户类定义 `__neg__` 后取负直接 **SIGSEGV(139)**。

**根因不是 dunder 查表缺失，而是「一元运算整条链都没做类实例分派」**：

1. `typecheck_expr.cpp` 的 UnaryOp 对 MINUS/PLUS/TILDE 一律返回操作数自身类型（Class），
   TILDE 还直接判死 `expected int`；
2. `lower_expr_ops.cpp` 的 `lower_unary` 没有任何类分支，直接落 `IrOp::Un`（`negq`/`notq`
   机器整数指令），**把对象指针取负**；
3. 结果 vreg 的静态类型仍是 Class（`ir_is_heap` 为真），降级层对野指针 `pa_rc_inc` -> 崩溃。

二元运算有 `lower_dunder_binop` / `dunder_binop_ret`，一元完全没有对应物。

修复（9 个文件，+137/-12）：
- 新增 `TypeChecker::dunder_unary_ret`：按 MRO 查单参 dunder 的声明返回类型；
- MINUS/PLUS/TILDE 对 Class 走 `__neg__` / `__pos__` / `__invert__`，无实现且非开放类则报错
  （不再静默）；`lower_unary` 加类分支走 `dunder_call`；
- `abs()` 走 `__abs__`（原来把对象指针当 i64 取绝对值）；`operator.neg/pos/abs/invert` 同样走 dunder；
- `AnnAssign` 到 `self.attr` 时补登记接收者类型（顺带修掉一个 `unsupported assignment target`）；
- `dunder_len` 容忍 bool 返回、`dunder_bool` 容忍 int 返回（否则静默退回指针判空，结果相反）。

`__pos__` / `__invert__` / `__abs__` 与 `__neg__` **完全同根因**，一并修；
`__bool__` / `__len__` 根因不同（本来就有 dunder_call，但只认精确返回类型），也已修。

验证：字面复现从 rc=139 变成 rc=0 且输出 `-3` 与 CPython 一致；新增
`tests/run/176_unary_dunder.py`；全量 **195 ran ok / 0 FAILED / 0 OUTPUT DIFF**；
z1_rt 116、z2_rt 134、z2_codegen 99、tests/eh 33、sem_baseline 42 全部无回退。
## [0.184] - 2026-11-16

### 零开销异常 Z2-b2：codegen 接线打通（`--zero-cost-exc`，默认关闭）

最小闭环完成。同一份 `.py` 用两条路径各编一次，核心证据：

| | 老路径 | 新路径 |
|---|---|---|
| `call _setjmp` | 有 | **0 处** |
| `call pa_handler_push` | 有 | **0 处** |
| `.cfi_personality` / `.cfi_lsda` / `.gcc_except_table` | 无 | 齐全（GCC 同款 catch-all 形态） |
| stdout | 基准 | **逐字节一致** |

- 开关 `--zero-cost-exc` 默认关；关闭时 **187/187 个文件的 asm md5 与 HEAD 完全一致**
  （隔离树 HEAD + 仅本补丁验证）。
- 形状门禁（`lower_exc.cpp: find_zero_cost_try`）：非可挂起 + 恰好一条 try + 零 with +
  单 catch-all handler + 无 else/finally + try/handler 体内无 return/break/continue。
- IR 新增 `EhBegin/EhEnd/EhLandingPad` 标记与 5 个 `RtFn`；LP = `pa_eh_land` -> `RcCleanup` ->
  **现有 except 分发块一行不改**；不匹配走 `pa_eh_rethrow` 重启 phase 1。
- 新增 `tests/exc/z2_codegen/run.sh`：9 用例 × 11 断言 = 99 项全过。
- 压力：10 万次跨两个被跳过帧的 throw/catch，新路径 0 setjmp / 2 LSDA，maxrss 11324KB
  （老路径 11348KB）——方案 G 的帧清理接线正确、无泄漏。
- 注意：**现有 177 个用例里没有任何函数满足第一刀门禁**，所以开关打开对现有套件零影响；
  新路径目前只由专用夹具覆盖。

### 模块注册表：从 3 处硬编码名单变成单一数据源

- `src/driver_build.cpp` 新增 `kModuleTable`（36 行，`ModuleKind` = Native / Python / NativePython），
  删除原来散在三处的重复名单（`program_module`、`check_builtin_name`，以及 `visit_unit` 里的
  import 跳过名单）。
- `NativePython` 语义：导入时把 `stdlib/<source>` 编成模块，对外接口 = 原生接口 ∪ 扩展源新增名字，
  **原生名字优先**（扩展不允许覆盖原生，会破坏现有语义）。
- **登记门**：`stdlib/` 里的 `.py` 未登记则直接报错，保证表是唯一来源；
  Debug 构建每次 load 跑 `validate_module_table()` 自检源文件与依赖。
- 证明：改动前的二进制跑新用例报 6 个错（`module 'functools' has no attribute 'cache'`…），
  改动后输出与 CPython 一致。
- 现在加一个新模块 = 表里 **1 行** + 一个 `.py`；给原生模块补 API = **0 行 C++**。

验证：全量 194 ran ok / 0 fail；z1_rt 116、z2_rt 134、tests/eh 33 项全过。
## [0.183] - 2026-11-16

### GC：修复 54 个函数的「构造期对象不在 GC 根上」窗口（一整类 UAF）

0.181 修掉异常路径的 GC 根缺失后，沿着同一类问题做了系统性审计。

**方法**：脚本递归统计「能传递触发 `rc_alloc`」的函数共 243 个 → 取「≥2 次分配调用」的候选 80 个
→ 逐个人工核对「第二次分配发生时，第一次的结果是否只存在于 C 局部变量里」。

**结果**：修复 **54 个函数、70 个 pin 点**，另修 **2 处全局根的登记顺序**（改成「先登记根、再分配」）。
新增统一助手 `pa_gc_pin` / `pa_gc_unpin`：不转移所有权（临时 +1 引用并挂临时值栈，unpin 撤销），
所以既能 pin 自己刚分配的对象，也能 pin 借来的参数；**异常安全**——pin 的引用落在临时值栈上，
`longjmp` 时由 handler 的清理水位统一释放。

**修复前必崩的实际路径**（都实测到 139）：`split` / `split(sep)` / `splitlines`、`dict.items`、
`re.findall`、`Path.parts`、`itertools.permutations(9)`、`file.readlines`、`json.loads` 深嵌套、
`asyncio` 的 `wait_for`/`gather`/`create_task`、`subprocess`、`sha256_hex`、`re.sub` 的用户回调等。
这些都是「平时跑得好、GC 压力一大就炸」的形态。

**根因证据**：新增 `tests/run/174_gc_construct_roots.py` 在**修复前**的运行时上 3/3 稳定 SIGSEGV；
修复后连跑 5 次 rc=0，`MALLOC_CHECK_=3 MALLOC_PERTURB_=170` 下同样稳定，
`PYADAMAS_GC_CONSERVATIVE=1` 下输出一致。

**没有采用「保守扫整段 C 栈」**——那会把 0.175 的根扫描收窄对冲掉；本任务坚持定点 pin 加固。

验证：全量 194 个运行用例 ran ok、59 个反例按预期 rejected，0 失败。
## [0.182] - 2026-11-16

### 零开销异常 Z2-b1：运行时胶水 + 方案 G 的帧清理（personality 侧，尚未接 codegen）

- 新增 `runtime/rt_eh.c`：`pa_eh_throw`（把异常包进 `_Unwind_Exception`，16 字节对齐、
  `private` 清零；`_URC_END_OF_STACK` 时返回 `PA_EH_UNCAUGHT` 交上层打印并退出）、
  `pa_eh_landing`（取回 `PyAdamasException*`，装好 `pending_exc` / `current_exc_inst` /
  `pending_exc_args`）、`pa_eh_done`。
- `PyAdamasFrame` 加 `cfa` 字段（帧记录 5 字 -> 6 字），codegen 序言在压帧**之前**把 CFA 写进去；
  `pa_frame_unwind_to_cfa` 逐行对齐 `pa_frame_unwind` 的 bit0/bit1 处理，并加重入 guard 与
  不变量 panic；personality 在 phase 2 的 `_UA_HANDLER_FRAME` 分支、`_Unwind_SetIP` **之前**
  经钩子调用它（不链 `rt_eh.c` 的 Z1 夹具里钩子为 NULL，行为不变）。
- 新增 `tests/exc/z2_rt/`：手写三层帧夹具（owned / bit0 / 只扫 bit1 三种槽）+ driver +
  `run.sh`（PIE 与 -no-pie、GC 开/关、两组反证）。
- 本轮**生成代码仍走 setjmp**，raise / landing pad 的接线是 Z2-b2。
- 非抛出路径的新成本：有帧记录的函数 +8 字节栈与 2 条指令。

### 两处实测订正（已写回 `docs/exc-frame-cleanup.md`）

1. **`_Unwind_GetCFA` 的语义和设计稿假设不同**：在 personality 里它返回的不是当前帧自己的
   DWARF CFA，而是「当前帧直接调用出去的那一帧」的 CFA（约等于该帧 call site 的 rsp）。
   实测 handler 帧真 CFA = rec+112，拿到的是 rec+0；Z1 那种 8 字节帧两者只差 16，所以此前
   看起来「就是自己的 CFA」。因此停止条件必须是 `cfa <= target`，用 `<` 会漏掉最深一帧。
2. **「异常路径每次泄漏 0.63 KB」被证伪**：0.181 查明那次规模相关的崩溃是 GC 根缺失，
   修好后 1 万 / 10 万 / 100 万次异常的峰值 RSS 都是 1.9–2.3 MB，没有泄漏。
   「用 RSS 不随规模增长做 Z2 验收」这个指标仍然有效。

验证：`tests/exc/z2_rt/run.sh` 134 项断言全过（含两组反证：不压记录时释放计数 FAIL、
不释放时 RSS 涨 29.8 MB 被抓）；`tests/exc/z1_rt/run.sh` 116 项仍全过；
全量 `tests/run_tests.sh` 204 用例 0 失败。
## [0.181] - 2026-11-16

### 修复异常路径的 GC 根缺失（10 万次异常循环必崩的 UAF）

现象：`tests/run/92_exc_inline_rc.py` 把循环从 2000 改成 100000 后**必然 SIGSEGV(139)**，
20000 以内正常。gdb 断在 `pa_exc_accept_value -> pa_rc_inc`，即对已释放对象做 `rc_inc`。

**根因不是泄漏，是 GC 根缺失**（`runtime/rt_exc.c`）：

1. `pending_exc_args` / `current_exc_inst` 是 RC owned 的普通 C 全局，**从未登记为 GC 根**。
   raise 与 accept 之间只要有一次分配触发主收集，它们就会被清扫掉；
   实测 gdb 抓到 args 表内存已被 `PyAdamasExcValue` 复用（`->len` 读到 `EXCVAL_MAGIC`）。
2. 异常值构造期间（`pa_list_new` / `str_new` / cause 递归）新对象只存在于 C 局部变量里，
   主收集同样看不见 —— 只修第 1 条后，20 万次仍会崩在 `exc_new(type=NULL)`。

修法：
- 首次 raise 前把两个全局槽地址 `pa_gc_add_roots` 登记为根；
- `exc_value_alloc` / `pa_exc_value_from_exc` 构造期间用临时值栈（`pa_rc_temp` /
  `pa_rc_temp_take`，本来就是 GC 根）挂住半成品，交还调用方时再取回所有权。

归因证据：修前 `PYADAMAS_GC_DISABLE=1` 下 rc=0（确证是 GC 误回收）；
`PYADAMAS_GC_CONSERVATIVE=1` 下仍 139（排除 0.175 的位图收窄）。

验证（`tests/run/173_exc_loop_100k.py` 新增）：10 万次连跑 5 次 rc=0，输出与 CPython 一致；
派生用例 10 万 / 50 万 / **100 万** 次均 rc=0；`MALLOC_CHECK_=3 MALLOC_PERTURB_=170` 下同样稳定；
`-O0/-O1/-O2` 与 runtime `-O0` 全过。
**峰值 RSS 不随规模增长**（1 万 / 10 万 / 100 万次都是约 1.9–2.3 MB）——
之前「每次异常泄漏 0.63KB」的判断是错的，修好根之后没有任何泄漏。

遗留：其它 runtime 模块存在同类「构造期只在 C 局部、不在根上」的窗口（如 `loop_ensure`
连发 4 个 list、`pa_sys_set_args`），本次只覆盖异常路径，建议后续抽统一 pin 助手再全量审计。
## [0.180] - 2026-11-16

### 标准库：新增 7 个模块 + 3 处补全，并给出覆盖缺口矩阵

用 51 个常用 CPython 模块做了实测（`import` + 2~3 个常用 API），结论是
**27 个原有可用 / 24 个完全缺失 / 0 个「能 import 但运行失败」**。矩阵与补齐成本见
[`docs/stdlib-gaps.md`](docs/stdlib-gaps.md)。

新增（全部是 `stdlib/*.py` 自举实现，不动 `src/` 与 `runtime/`）：
`typing` / `enum` / `abc` / `collections.abc` / `glob` / `uuid` / `fractions`；
补全 `textwrap`（`TextWrapper`）、`statistics`（`quantiles`）、`csv`（`DictReader`/`DictWriter`）。

其中 `typing` 桩模块的收益被低估了：注解解析器（`src/typecheck.cpp`）本来就按名字特判 typing
名字，所以一个纯桩模块就能解锁大量真实脚本的注解。

新增 10 个用例（`tests/run/163`~`172`）。验证：191 项全过（基线 181 + 新增 10）。

### 已知阻塞（需决策，未做）

- **内建模块名单是硬编码的**（`src/driver_build.cpp:609-618` 与 `:890-897`），所以
  `stdlib/functools.py` 这类会被挡住 —— 补 functools / itertools / collections 必须先把名单
  改成数据表。
- `dataclasses`：codegen 不合成 `__init__`，shim 会让 `P(1,2)` 静默变成 `0,0`，不能做。
- `contextlib`：unknown 函数值的 `*args` 转发被编译器拒绝。

### 顺带发现的既有编译器缺陷（非本次引入，已记入 stdlib-gaps.md）

- 用户类定义 `__neg__` 后取负 **SIGSEGV（exit 139）**；
- `list(obj)` 不能消费只有 `__iter__` 的用户类。
## [0.179] - 2026-11-16

### 汇编发射换成结构化 Machine 层（行为逐字节不变）

codegen 过去把 AT&T 文本直接拼进 `std::string`（约 250 处 `std::format` + 250 处 `out_ +=`）。
现在 `AsmGen::out_` 是 `MachFunc`：标签 / 指示符 / 指令 / 注释 / 原始文本的序列，
指令再拆成操作码 `MOp` 与操作数 `MOperand`，由 `render()` 拼回文本。

- 新增 `src/mach_instr.h`（数据模型）与 `src/mach_emit.cpp`（一行文本 -> MItem 的解析 + 渲染）。
- 迁移策略是**先换容器、再逐点结构化**：历史发射点通过 `operator+=`（等价 `append_raw`）与
  `line()`（解析成结构化 item，认不出的文法退化 Raw）零改动接进来，所以这一轮生成的汇编
  **与改动前逐字节一致**。
- 验收门槛：176 个文件（`tests/run/*.py` + `tests/*.py`）逐个 `pyadamas asm` 的 md5 全部与
  改动前相同 —— 这是「行为不变」的硬证据，比「测试通过」强。
- 后续工作：219 个 `out_ +=` 直写点逐步换成 `instr()` / `directive()` / `label()` 直接构造；
  之后寄存器分配才能在指令层改写操作数。

**已知坑**：`MachFunc::push_line` 的契约是「已含前导 tab 的一整行」，而旧的 `line()` 自己会补 tab。
两者契约不一致时，所有走 `line()` 的指令都会丢掉前导 tab —— 编译通过、汇编也能过、
`readelf` 一切正常，只有字节变了。用复用缓冲区补 tab 修掉（`line_buf_`，避免每行一次分配）。
## [0.177] - 2026-11-16

### 零开销异常 Z2-a：LSDA 发射器（已就绪，尚未接入 codegen）

新增 `src/eh_emit.h` / `src/eh_emit.cpp`：纯函数式的 LSDA / CFI **文本发射器**，
输入一个函数的保护区计划，输出 `.cfi_personality` / `.cfi_lsda` / `.gcc_except_table`。

- 两种形态：handler（`ttype=0x9b` + TType 表一个 null 项 + `filter=1`）、
  全 cleanup（`ttype=0xff`、12 字节，TType 偏移字段整段消失）。
- 输入支持数值偏移与标签差（`.uleb128 .LEHE0-.LFB0`）两种模式，产物字节相同。
- 接口拆成三段（CFI 指令必须夹在 `.cfi_startproc`/`.cfi_endproc` 之间、LSDA 在
  `.gcc_except_table`、DW.ref 每个 TU 一次），这是位置约束决定的，不是设计选择。

**订正（实测）**：call-site 的 action 字段是 **action 表内的 1 起字节偏移**，不是记录序号
（多 handler 应为 1, 3, ...）；按序号实现会被独立解码器判为结构非法。另外单 handler
且没有 LP 覆盖 range 时是 16 字节 —— 20 字节基准必须**含第二条 `lp=0` 的 range**。

验证：`bash tests/eh/run.sh` 33 项断言全过；`catch_gcc20` 与 `cleanup_gcc12` 两组产物与
gcc 14.2 实测基准**逐字节一致**；另用 `tools/lsda_ref/decode_eh_lsda.py` 独立解码 6 份 `.o`，
结构校验 0 处「结构非法」。

尚未接入 codegen（Z2-b），也未处理 LP 覆盖 range 与 typed catch。
## [0.176] - 2026-11-16

### 零开销异常 Z1.5：handler 判据对齐 ABI + 真 gcc personality 交叉验证

交叉核对（`tools/lsda_ref/CROSSCHECK_M2.md`）指出 Z1 的 personality 用「call-site 的 action
下标 != 0 即 handler」是**私有语义**：ABI 里 `filter == 0` 就是 cleanup，catch-all 必须
`filter > 0` **且** TType 表里有一个 null 项。这一版把它对齐。

- `pa_personality` 解析 action 记录链（两个 SLEB128 的 filter / next）：action 下标 == 0 或
  `filter == 0` -> cleanup；`filter > 0` -> 读 TType 表（反向索引 `ttype_end - filter*size`，
  并实现 libgcc 的 `result == 0` 特判 —— 这正是 null 项为「4 字节 0 + 无重定位」的原因）；
  `filter < 0`（exception spec）-> `_URC_CONTINUE_UNWIND` + 诊断计数。
- 夹具升级为 GCC 同款形态：handler 20 字节（`ttype=0x9b`、偏移指向表末尾、null 项、`filter=1`）、
  cleanup 12 字节、无 pad 8 字节；personality 指针用 `0x9b` 指向 driver 里的运行期槽，
  于是**同一份 .S 的同一套字节可以被两种 personality 解释**。
- cleanup / handler 的 landing pad 区段补了覆盖 LP 的 call-site 记录 —— 缺它时标准
  personality 在 `_Unwind_Resume` 阶段会 `found_terminate` -> terminate。
- 负向用例重做：`0x9b` -> `0xfe` 断言「编码非法」，另加 udata8 编码的夹具覆盖「表越界」分支。
- 新增**真 gcc personality 交叉验证**（`tests/exc/z1_rt/crosscheck_gxx.cpp`）：同一份 `.S`
  换填 `__gxx_personality_v0`，真 C++ `throw 7` + 外层 `catch(int)`。实测
  `caught=0 / lp_calls=1 / fn_ret=7 / z1_lp_seen=1` —— 夹具帧被标准 personality 正确当成
  catch-all（异常被吃掉、landing pad 只跑一次）。PIE 与 -no-pie 两套都过。

`filter < 0` 的处理选择：继续展开 + 诊断计数，**不 terminate**。理由是我们没有 spec 表与
`__cxa_call_unexpected` 协议，做不了 ABI 的正确行为；贸然 abort 会直接杀用户进程，
保守继续 + 可观测计数更可控，且不会错误吞异常。Z4 做 spec 时在该分支补实现即可。

验证：`bash tests/exc/z1_rt/run.sh` 退出码 0，两套链接共 **116 项断言**（43 -> 58 / 套，
含 2 次交叉验证）；`tests/run_tests.sh` 179 项全过。
## [0.175] - 2026-11-16

### GC 根扫描：整段保守扫描 -> 位图收窄（技术债 #1 / #2）

`docs/roadmap.md` 把技术债「按什么时候会疼」排过序，第 1 条是「GC 的根只扫登记区、
临时区仍整段保守扫描、没有栈映射」，第 2 条是「unknown 引用计数不精确、靠保守扫栈兜底」。
两者共同的代价是**误报保留**：临时区里一个恰好长得像指针的整数，会把一整片垃圾对象钉在堆上。

- 临时区保守扫描按 vreg 静态类型收窄：编译期为每个帧发射一张位图（`scan_mask`，放 `.rodata`），
  GC 只扫置位的字；**纯标量帧直接 `scan_words = 0`，整段跳过**。
- 兜底方向只有「宁多勿少」：`unknown` / 无类型 / `Optional[unknown]` / `BoundMethod` /
  非 C `StructPtr` 一律置 1；vreg 槽复用按字取并集；vreg 槽号与表对不上、或收窄不到一半
  就不发位图、退回整段扫；生成器/协程帧不参与；`unknown` 名字槽仍按精确地址登记，不受位图影响。
- 新增 `PYADAMAS_GC_STATS=1`：每轮打集合/释放/耗时/存活对象与字节，根扫描按
  「帧数 / 登记槽 / 临时区实扫÷整段 / 寄存器字数 / 位图是否生效」分解，并给出
  `cons_kept`（先精确根扫排空、再扫保守根时新标活的对象数与字节，按临时区 / unknown 槽 /
  寄存器分项）——这是「误报保留」的直接度量。默认关闭时**零额外开销**（启动读一次环境变量）。
- 新增 `PYADAMAS_GC_CONSERVATIVE=1`：忽略位图，完全退回整段保守扫描。

实测（全量 163 个 `tests/run`、143 轮收集）：

| 指标 | 收窄前 | 收窄后 |
|---|---|---|
| 临时区实扫字数 | 2901 | **403（13.9%）** |
| 保守保留对象 / 字节 | 92 个 / 8144 B | **0 / 0** |

单例：`13_gc` 35→3 字、`151_gc_generations` 33→1 字、`92_exc_inline_rc` 42→1 字。

验证：179 个用例在收窄与回退两种模式下各 **179 pass / 0 fail**；`MALLOC_PERTURB_=170` +
`MALLOC_CHECK_=3` 下再跑一遍同样全过；16MB `RLIMIT_AS` 下拼接/列表/对象环/dict 自环/闭包/
字段/切片压力程序 `rc=0`，峰值 RSS **7808KB（收窄）/ 8020KB（回退）**。
新增 `tests/run/160_gc_scan_narrow.py`（用 `id()` 造「恰好等于堆地址的整数」，收窄前
保守保留 86 个 / 7568 字节，收窄后归零）与 `161_gc_conservative_equiv.py`（回退开关语义等价）。

范围与前提：位图按**字**收，不按**时刻**收（不活跃的 unknown 字仍会扫）；仍没有增量/并发 GC
和完整栈映射；收窄依赖「类型化堆引用要么 owned、要么持有者从根可达」这一既有前提。
## [0.174] - 2026-11-16

### 零开销异常：Z1 地基打通（不改默认代码路径）

目标是把 setjmp/longjmp 换成 Itanium C++ ABI 的表驱动展开。这一步**不碰编译器**，
只验证 personality 契约、LSDA 编码、landing pad 跳转这三件最容易出错的事。

- 新增 `runtime/rt_personality.c`：通用解码 uleb128/sleb128/encoded pointer 与
  LPStart/TType/call-site 表头，实现两阶段语义（search 只认 action != 0 的 handler 帧；
  cleanup 阶段对纯清理帧同样安装上下文，由 landing pad 自己 `_Unwind_Resume`），
  `_UA_FORCE_UNWIND` 下只穿过、不安装。已编进 `rt.o`，默认代码路径不引用。
- 新增 `tests/exc/z1_rt/`：四份手写 `.S` 夹具（无 LSDA 的 plumbing / 真 LSDA /
  两阶段 + 无 pad 中间帧 / 只改 1 bit 的坏表负向）+ driver，`-no-pie` 与 PIE
  两套链接各 43 项断言共 86 项全过；`bash tests/exc/z1_rt/run.sh` 可复跑。
- 新增 `tools/lsda_ref/`：用 gcc 14.2 / binutils 2.44 实测标定 LSDA 字节布局
  （100+ 条硬编码字节断言、11 条链接矩阵断言，`check.sh` 可复跑）。手写汇编产出的
  `.gcc_except_table`（20 B）与 `.eh_frame`（64 B）和 `g++ -O1` 的 catch-all 产物
  **逐字节完全一致**。
- 新增 `docs/exc-semantics.md`（现行异常语义 60+ 行 file:line 对照表、10 条已知不一致、
  三条难点的具体结论、Z2 发射清单）与 `tests/exc/sem_baseline.sh`（42 个异常用例基线，
  `PASS 42 / 42`）；重写 `docs/zero-cost-exceptions.md`。

### 订正与范围收紧（实测推翻了若干早期结论）

- `0x9b` / `0x1b` 的字节对没错，但语义是 `sdata4` 不是 `udata4`（`udata4` 是 0x03）。
- 与 gcc 同构的形态下 **catch-all 的类型表不能省**：gcc 用 `ttype=0x9b` + 一个 null 项
  表示 `catch(...)`；省掉会让 catch 退化成 cleanup，永远抓不到异常。
- action 表不是「条数 + 计数」，而是每条记录两个 SLEB128 的 `(filter, next)`。
- 判断 catch / cleanup 的正确层次是 **call-site 的第 4 个字段（action 下标）是否为 0**。
- Z2 范围收紧为「非可挂起函数 + catch-all 单层 try」：生成器/协程/异步生成器里的 5 处
  C 级 setjmp 兜底抓不到 `_Unwind` 抛出的异常，**永久保留 setjmp**；`with` 可能抑制异常，
  不是纯 cleanup，不一起迁。
- Z3 的类型过滤必须放进 personality 的 phase 1：展开器在 phase 1 只搜索一次，
  内层 except 不匹配后 `_Unwind_Resume` **不会**重新选外层 handler。

### 已知坑（都复现过，详见 `tests/exc/z1_rt/README.md`）

- landing pad 入口的 `%rsp` = 抛出点那条 `call` 之前的 rsp（不是 CFA、不是入口 rsp）。
- landing pad 必须落在 `.cfi_endproc` 的 FDE 范围内，否则 LP 里 `_Unwind_Resume` 时
  libgcc 找不到 FDE -> `SIGABRT(134)`，且**没有任何错误输出**。
- `.cfi_personality 0x9b, <函数名>`（省掉 `DW.ref` 指针槽）汇编和链接都不报错，
  运行期 SIGSEGV。
## [0.173] - 2026-11-16

新增 **x86-64 Windows(win64) 目标**：`--target=win64` 产出可在 Windows 上原生运行的
PE 可执行文件（已在 Windows 宿主机实测运行，非 wine）。运行时 28 个编译单元全部
可交叉编译到 Windows。

### 新增

- `src/target_abi.h` / `target_abi.cpp`：TargetABI 描述一个目标的 ABI 事实（实参
  寄存器、callee-saved 池、浮点池、scratch、栈对齐、影子空间、setjmp 符号名、
  块内临时池等）。两个实例：`kX86_64SysV`（默认）与 `kX86_64Win64`。
- `--target=` 开关（x86_64-sysv / x86_64-win64，也接受 sysv/win64/linux/windows）。
- `emit_arg(pos, is_fp, cls_idx, v)`：同时支持 SysV 的「按类型分计数」与 Win64 的
  「按位置分配」，超出寄存器个数时按 ABI 落栈。
- 运行时平台层（`runtime/rt_internal.h`）：poll → WSAPoll（含非 socket 句柄桥接）、
  clock_gettime → QueryPerformanceCounter、nanosleep → Sleep、mkdir/realpath/
  sysconf/timegm 的 Win32 等价、iconv 恒等桩。
- `runtime/rt_subprocess.c` 的 Win32 分支：CreateProcess + 匿名管道，POSIX 路径零改动。
- `runtime/rt_exc.c` 的 `pa_win_setjmp`/`pa_win_longjmp`：只保存非易失寄存器、
  rsp、返回地址，不依赖展开信息（msvcrt 的 longjmp 走 SEH，而生成的汇编函数没有
  `.pdata`/`.xdata`，展开会失败）。
- 汇编指示符按目标开关：`.cfi_*`/`.type`/`.size`/`.note.GNU-stack` 在 COFF 下关闭，
  `.data.rel.ro` → `.rdata`。
- `verify_win.sh`：一条命令跑完「构建 / POSIX 全量 / SysV 汇编 md5 / 标准库基线 /
  Windows 覆盖」五项。

### 性能

- `try` 进入开销 **122 ns → 6 ns**：glibc 的 `setjmp` 每次都做 `rt_sigprocmask`
  保存信号掩码（实测 117.2 ns vs `_setjmp` 2.5 ns）。pyadamas 从不改信号掩码，
  改用 `_setjmp`/`_longjmp`；handler 记录再改空闲链表复用（13 ns → 6 ns）。

### 修复

- **SSE 不支持内存到内存的 `movsd`**：`move_fp` 在源和目的都在栈槽时直接发
  `movsd mem, mem`，汇编器报错、整个文件编译失败。此前零测试覆盖（跑 160 个
  用例的汇编逐字节对比，唯一差异是另一处）。现经 `%xmm0` 中转，新增
  `tests/run/102_float_slot_move.py` 锁死。
- **Win64 第 5+ 个栈实参的 mem-to-mem `movq`**：同理，改为经 `%r10` 中转。
- **`gc_collect` 晋升阶段读取已释放对象**：Windows 堆在 free 后会 decommit，
  读它直接 `0xC0000005`。POSIX 下靠运气不崩。
- **生成器 `GenSetSlot` 写死 `%rcx`**：Win64 下 `%rcx` 是第 1 个实参、此刻已失效，
  于是往生成器帧里写垃圾值——`for` 循环上界变成随机数，**不崩不报错、只是永不
  结束**。这是整批 Windows 问题里唯一致命形式不是崩溃的一类。
- 手写汇编跳板（`.Ldtor`/`.Lgcrel`/`.Lgengcrel`/`pa_call1`/`pa_call2`）的实参
  寄存器与影子空间按 ABI 取。

### 验证

| 项 | 结果 |
|---|---|
| Windows 一致用例 | **138 / 146**（派工前 69 / 139） |
| 挂起 | 0 |
| POSIX 全量 | 177 通过 / 0 失败 |
| 标准库前端基线 | 502 / 596 |
| SysV 汇编 | 160 用例逐字节不变（`88f2bb28…`）|

**未完成**：结构体按值传参的 Win64 分类规则（3 例）、可变参数（1 例）、
Windows 专有 API（2 例）、COFF 下缺 `.pdata` 导致 traceback（1 例）、
`86_popen` 用例里写死 `/bin/echo` 不可移植（1 例）。

## [0.171] - 2026-11-15

继续收口散簇：标准库前端基线 435 → 501 / 596（84.1%）。

### 变更

- **模块级 undefined name 保守放过**：只在函数体内确定性报错；f09 改成函数体用例保留诊断。
- **None 接收者全放行**：方法/属性、有序比较、成员测试、迭代、下标赋值。
- **类型对象**：`type(self).method()`、3 参 `type(name,bases,dict)`、`__qualname__`/`__mro__` 等、类实例无 `__call__` 当构造器。
- **super()**：类上下文丢失 / 协议 dunder 找不到 / `super(Cls,self)` 当前类。
- **多继承**：附加基类字段布局冲突降为 unsupported。
- **可重绑定**：推断形参、`*args`/`**kwargs`。
- **杂项**：`async with`、`print(file=)`、`open(**kwargs)`、独立 `slice()`、`str.maketrans`/`bytes.partition`、file 方法名当值、切片赋值 Class/None、Exception/Class/Dict 可解包。

### 验证

- 前端基线：435 → **501 / 596（84.1%）**，去掉 `test/` 449 / 532。
- `ctest` 3/3；run 套件 173 ok / 0 fail；`check_size.sh` 通过。

### 说明

剩余 95 个多数是「正确的检查结果」而非误杀（如 tempfile 裸 `os.` 而只 `import os as _os`，CPython 运行也会 NameError；enum/timedelta/Decimal 算术语义；跨模块成员可见性）。要全通需放宽真阳性或实现完整 import 图解析。


## [0.170] - 2026-11-14

一键扫荡剩余散簇：标准库前端基线 411 → 435 / 596（73.0%）。

### 变更

- **2 参 super**：`super(Cls, self)` 当 Cls 是当前类时按零参 super 处理。
- **no-`__init__` 带参实例化**：常量类的标记值实例化不再判死。
- **unpack 放宽**：generator/range/str/Optional 目标可解包（元素类型逐个赋）。
- **type 对象元数据属性**：`__qualname__`/`__mro__`/`__doc__`/`__bases__`/`__module__` 等放行。
- **异常值方法**：`with_traceback`/`add_note`；异常/type 对象属性赋值、方法名 monkey-patch（仅函数值）改为 unsupported。
- **推断形参可重绑定**：inferred 参数不锁定类型，`cannot assign` 簇 18→4。

### 验证

- 前端基线：411 → **435 / 596（73.0%）**，去掉 `test/` 392 / 532。
- `ctest` 3/3；run 套件 173 ok / 0 fail；`check_size.sh` 通过。


## [0.169] - 2026-11-14

getattr 非字面量 + 内置异常多参：标准库前端基线 374 → 411 / 596（68.9%）。

### 变更

- **getattr/setattr/hasattr 属性名非字面量**：`getattr(obj, name)` 对非类/unknown/模块/函数接收者从「报错」改为 check 一律放过（返回 unknown），类实例仍走运行期动态属性表。
- **内置异常构造多参 + 关键字**：`ValueError(a, b, c)` 任意位置参、`Error(code=1)` 关键字不再拒绝。lower/运行时建 args 元组、消息取元组 repr；`raise→except` 间经 `pending_exc_args` 保留 args，`e.args`/str 与 CPython 一致。

### 验证

- 前端基线：374 → **411 / 596（68.9%）**，去掉 `test/` 370 / 532。
- `ctest` 3/3；run 套件 173 ok / 0 fail；`check_size.sh` 通过。
- 新增 `tests/run/159_exc_multi.py`（多参异常与 CPython 逐字一致）。


## [0.168] - 2026-11-14

isinstance / bytes：标准库前端基线 348 → 374 / 596（62.8%）。

### 变更

- **isinstance 第二参数拓宽**：接受的内置类型扩展到 `bytes`/`bytearray`/`memoryview`/`type`/`slice`/`frozenset`/`property`/`classmethod`/`staticmethod`，还有 `(A, B, ...)` 元组/列表、unknown 与可迭代目标。
- **isinstance 运行期**：
  - 元组/列表目标短路 OR：逐个元素检查，任一命中为真，对象只求值一次。
  - known 类型静态判定（`isinstance(b'x', bytes)`→True 等）。
  - unknown 值带运行期标签的内置（str/list/tuple/dict/complex）按标签查；无标签的（int/float/bool 等）保守返回 false。
- **bytes**：`bytes()` 构造接受 2 参（对象+编码，编码忽略）与 list/tuple/生成器/range 的 int 元素；`bytes.decode` 放宽 0..2 参；`bytes * int` 重复、`bytes.translate`；内置类型名当值（`int_.from_bytes`）走类型级方法。

### 验证

- 前端基线：标准库本体 348 → **374 / 596（62.8%）**；去掉 `test/` 为 338 / 532。
- `ctest --preset dev` 3/3（运行 156+）；run 套件 172 ok / 0 fail；`tests/check_size.sh` 通过。
- 新增 `tests/run/158_isinstance_bytes.py`（输出与 CPython 逐字一致）。


## [0.167] - 2026-11-14\n\n自定义异常类当普通类：富异常收字段/方法/岛\u5efa模。\n\n### 变更\n\n- **富异常类当普通类**：自定义异常类有字段/方法/岚`__init__`/`__str__`时，实例是真正的 vtable 对象。`raise Rich(args)`通过类构造建实例并从岚`pa_raise_instance`抛出。`except Rich as e` 通过岚`pa_exc_accept_instance`取回省实例，`e.code`/`e.describe()` 走普通类字段/方法访问。\n- 实例是普通 vtable 对象：字段初始化、vtable 方法分派、`super()` 均正常；`多模块/内置接受` （请参check 阶段放过。\n- **内置异常属性保守**：`OSError.errno`/`UnicodeError.code` 这类内置异常的大多属性静态看不见，check 打 unsupported 放过。\n\n### 验证\n\n- 前端基线：标准库本体 335 → **348 / 596（58.4%）**。\n- `ctest --preset dev` 3/3（运行 156）；差分 840 个程序 0 失败；`tests/check_size.sh` 通过。\n- 新增 `tests/run/157_rich_exc.py`（富异常字段/方法、raise/except 取回实例『pass-only 仍 e.args）。\n\n## [0.166] - 2026-11-14

跨模块 super(): 基类来自别的模块时，check 不再拒绝。

### 修复

- **super() 上的跨模块基类**: 定义类的 MRO 里有导入基类时，它的方法在 check 阶段看不见，以前直接把“找不到方法”当成错误拍死了所有“class X(base.Base): super().__init__()”。现在只在纯本地继承链都找不到时才报错；有导入基类（或 open 类）时返回 unknown，build 沿运行期 MRO 分派。

### 验证

- 前端基线: 标准库本体 320 → **335 / 596（56.3%）**。
- `ctest --preset dev` 3/3（模块 13）; 差分 840 个程序 0 失败; `tests/check_size.sh` 通过。
- 新增 `tests/modules/cross_super/`（跨模块基类的 super()初始化与方法分派）。

## [0.165] - 2026-11-14

动态属性 + 反射放宽：标准库前端基线 294 → 320 / 596。

### 变更

- **实例动态属性**：类实例末尾可挂一个惰性分配的 `dict[str, unknown]`，偏移存进 vtable 元数据第 6 个字（0 = 没有，不用的类零开销）。`setattr`/`getattr`/`hasattr` 对静态没有的字段落到运行期查表；属性名非字面量也走查表。只有真的反射过的类才开槽，普通字段读取仍在编译期判死活（`f28` 拼错字段名保持报错）。
- **非类接收者反射，check 放过**：`hasattr(module, ...)` / `getattr(func, ...)` / unknown 接收者只打 `unsupported` 标记，`check` 不再判死，`build` 用模块当值才拒绝。

### 验证

- 前端基线（Debian CPython 3.13.5）：标准库本体 294 → **320 / 596（53.7%）**，去掉 `test/` 为 **288 / 532（54.1%）**。
- `ctest --preset dev` 3/3（运行 155、反例 53）；差分 840 个程序 0 失败；`tests/check_size.sh` 通过。
- 新增 `tests/run/156_dynamic_attr.py` 与 `tests/11_reflect.py`。

## [0.164] - 2026-11-14

前端基线：放开两处误杀，标准库基线 281 → 294 / 596。

### 修复

- **dunder 返回类型改到函数体推理之后再判**：未标注的 `__str__`/`__repr__`/
  `__len__`/`__hash__`/`__bool__`/`__eq__` 等协议 dunder，定义处还看不到
  return 的类型，以前按 unknown ≠ str 直接判死。现在统一在
  `check_dunder_returns()` 里按推理结果校验；仍是 unknown 就放过——证明
  不了错就不报错。
- **`getattr(obj, name, default)`**：支持第三个参数。静态布局里属性集合
  是固定的，属性缺失时结果就是缺省值；类实例按字段/属性查，其它已知类型
  按成员表判。

### 验证

- 前端基线（Debian CPython 3.13.5）：语法通过 596，类型通过 281 → **294 /
  596（49.3%）**；去掉 `test/` 是 253 → **266 / 532（50.0%）**。
- `ctest --preset dev` 3/3（运行用例 154、反例 53）；差分 840 个程序 0 失败；
  `tests/check_size.sh` 通过。
- 新增 `tests/run/155_dunder_ret.py`（未标注 dunder + getattr 缺省值）与
  `tests/fail/f55_dunder_ret.py`（`__len__` 返回 str 必须报错）。
- 剩余失败已不是误判，而是四项没建模的能力：动态属性（hasattr/getattr
  48+20）、跨模块基类（super() 41）、isinstance（32）、自定义异常类（25+11）。
  明细记在 compatibility.md 的基线一节。

## [0.163] - 2026-11-14

栈回溯换成标准展开器，生成代码去掉帧指针；顺手修掉跨多个挂起点的溢出 bug。

### 修复

- **跨多个挂起点的值没有落槽**：`spill_across_suspends` 算活跃性时把
  `GenYield`/`GenAwait` 当成没有后继，只在续跑块里用到的 vreg 既不存槽、
  也不装回。`await add(await add(1, 2), await add(3, 4))` 这类链式 await 会
  读到上一轮留下的垃圾（RC 报 `refcount increment on a freed object`，或者
  直接段错误）。现在把续跑块算作挂起块的后继；新增
  `tests/run/154_nested_await.py`（await 链 + 生成器连续 `send`）。

### 变更

- **traceback 走 `_Unwind_Backtrace`**：`pa_print_traceback` 不再手走 rbp 链，
  改用 libgcc 的标准展开器逐帧取返回地址，仍只打印调试表里登记过的帧。
  运行时因此可以去掉 `-fno-omit-frame-pointer`（`rt_core.c` 早就不带）。
- **生成代码去掉帧指针**：槽位、帧记录、栈上实参改相对 `rsp` 寻址；出参区
  按整函数最大用量一次留够（调用点不再 `subq`/`addq`，`rsp` 全程不动），
  `%rbp` 并入变量寄存器池（最多 6 个）。槽号小的排低地址，偏移落在 disp8。
- **对齐**：函数入口 `.p2align 4`，循环头（回边目标）同样 16 字节对齐。
  去掉帧指针后 nqueens 一度慢 10%，对齐后回到原水平。
- CFI 跟着改：不再 `push %rbp`，CFA 全程由 rsp 定义，尾声逐条
  `cfi_restore` 并更新 `def_cfa_offset`；`link_program` 显式加
  `-Wl,--eh-frame-hdr`。

### 验证

- `ctest --preset dev` 3/3（运行用例 153）；差分 840 个程序 0 失败；
  `tests/check_size.sh` 通过。
- 未捕获异常回溯、gdb `bt`（含 `set backtrace past-main on`）都还能穿过生成帧；
  帧里已经没有 rbp，能展开只可能是 CFI 起效。
- 同机 A/B（8 轮取最小，旧 = 0.162，各 8 次交错跑）：fib 10.67 → 9.22 ms
  （-13.6%，少 push/pop rbp 与一次 mov）、mandelbrot -2.5%、binary_trees -0.9%；
  nqueens +3.6%、matmul +2.8%、spectral_norm +2.5%，在逐轮 ±4% 的噪声内。
  多出来的 `%rbp` 变量寄存器让 fib 这类调用密集的代码受益最明显。

## [0.162] - 2026-11-14

调试信息收尾：生成代码补上 CFI（栈展开规则）。

### 变更

- **CFI**：每个生成函数发 `.cfi_startproc` / `.cfi_endproc`，prologue 用
  `.cfi_def_cfa_offset` / `.cfi_offset` / `.cfi_def_cfa_register` 描述 CFA 与
  保存下来的 callee-saved（生成器是 `%r15`）；尾声用 `.cfi_remember_state` +
  `.cfi_def_cfa %rsp, 8` + `.cfi_restore_state`，一个函数里的多条返回各描述
  各的。GNU as 据此生成 `.eh_frame`。
- 函数符号补 `.type ..., @function` 与 `.size`；手写汇编（类析构器、GC
  release、生成器帧释放、C 回调跳板、模块 init 桩、`main`）同样带 CFI。
- 新增 `tests/cfi/`（cdecl + `_Unwind_Backtrace` 探针），在
  `tests/run_tests.sh` 里以 `cfi:unwind` 输出：CFI 断掉时深度从 7 掉回 2。

### 验证

- `gdb` 断在 `pushq %rbp` 之后（rbp 链尚未建立）仍能回溯，栈帧里列出
  rbp 的保存位置；`set backtrace past-main on` 可一路走到 `_start`。
- libgcc `_Unwind_Backtrace` 数出的帧依次是 `probe_depth` → `__pa_main` →
  `main` → `__libc_start_call_main` → `__libc_start_main` → `_start`，与 gdb 一致；
  生成器 resume 帧与协程帧（`%r15` 那一套）同样能穿过：探针在生成器体内
  和 `asyncio.run` 的协程体内都数出 10 层。
- `ctest --preset dev` 3/3；运行用例 152 个；差分 840 个程序 0 失败；
  `tests/check_size.sh` 通过。
- 生成代码只多了汇编指示（`.cfi_*` / `.type` / `.size`），指令序列不变；同机 A/B
  （同一二进制用 objcopy 去掉 `.eh_frame`）20 次取最小：mandelbrot
  6.91 / 6.81 ms、spectral_norm 18.19 / 18.35 ms，差值在测量噪声内。

## [0.161] - 2026-11-14

可靠性 + async/generator traceback 补全。

### 修复

- **模块缓存写入原子化**：`assemble_object` 不再直接写 `mod.s`/`mod.o`；
  `mod.pya` 与 `manifest` 也改为临时文件 + `rename`。多个进程同时编译
  同一模块（并行测试、ctest 与 diff 并发）不会再读到截断的汇编/对象文件。
- 新增 `pyadamas_cache_race` ctest：两进程并发 `--rebuild` 编译
  hashlib/hmac 5 轮，专门守这个回归。

### 变更

- **异常携带生成器/协程逻辑帧**：`PyAdamasException` 新增 trace 链表；
  协程 `coro_step`、生成器 `gen_run`、异步生成器驱动在异常逃出时把
  resume 函数帧挂到异常上。未捕获异常先打印普通 rbp 回溯，再追加
  `main` → `outer` → `inner` 这样的逻辑帧（当前只有函数名，没有挂起行号）。
- trace 节点随 `exc_clone` 深拷贝、随 `exc_free` 释放；153 测试扩展到
  async/generator 子进程。

### 验证

- `ctest --preset dev` **3/3**（新增 cache_race）；运行用例 152 个；
  差分 840 个程序 0 失败；16MB 内存验收、`tests/check_size.sh` 通过。
- `pyadamas_cache_race` 连续 5 轮并发 hashlib/hmac 编译通过；
  并行 stress（10 轮）无截断、无残留 `.tmp.*`。

## [0.160] - 2026-11-14
运行时 traceback：未捕获异常打印函数级回溯。

### 变更

- **PC → (函数, 行号) 表**：codegen 在行变化处发 `.LdbgN` 标签并收集表项，
  模块入口调用 `pa_debug_register` 注册；函数结束有哨兵项。
- **rbp 链回溯**：运行时 `rt_debug.c` 沿 rbp 链取返回地址，命中调试表后
  打印 `Traceback (most recent call last)` + `File "...", line N, in func`；
  运行时除 RC 热路径 `rt_core.c` 外统一 `-fno-omit-frame-pointer` 编译。
  core 不抛用户异常，排除它避免 RC 热路径回退（对比 0.158，容器类基准
  恢复到同一水平）。
- **显示名**：函数名去掉生成后缀（`inner_1` → `inner`）。
- **测试**：新增 `tests/run/153_traceback.py`，父进程以子进程重跑并检查
  stderr 含 traceback、函数名、异常消息与退出码。

### 验证

- `ctest --preset dev` 2/2（152 个运行用例）；差分 840 个程序 0 失败；
  16MB 内存验收和 `tests/check_size.sh` 通过。
- release 基准复跑（同机）：mandelbrot 26.5×、spectral_norm 30.3×、fib 15.3×、
  nqueens 8.7× vs CPython；容器类与 0.158 持平。
- 示例：
  ```
  Traceback (most recent call last):
    File "x.py", line 7, in __pa_main
    File "x.py", line 4, in rec
    ...
  pyadamas: uncaught ValueError: tb
  ```

### 限制

- 生成器/协程的堆帧不在 rbp 链上，回溯可能只显示调用它们的普通函数；
  优化内联后内联体归到调用者。
- 没有 CFI；`panic_c` 内部不变量错误仍直接退出；异常值对象仍不携带
  traceback（只有未捕获时打印）。

## [0.159] - 2026-11-14

调试信息：生成代码带 DWARF 行号与可读函数符号。

### 变更

- **IR 指令带源码行**：`Inst` 新增 `line`；降级期按当前语句/表达式行打标，
  优化遍复制/内联指令时保留行号。
- **DWARF 行号表**：codegen 输出 `.file 1 "<源路径>"` 与按行变化的
  `.loc 1 <line>`，GNU as 生成 `.debug_line`；每个函数发出可读符号
  `pyadamas_fn_<N>_<name>`（内部跳转仍是 `.LfN`）。
- **可定位**：`addr2line -e prog -f <addr>` 返回函数名与 `源文件:行`；
  `readelf --debug-dump=decodedline prog`、`nm prog | grep pyadamas_fn`
  可直接查看。

### 说明与验证

- 没有 `.debug_info` / CFI：`gdb` 能定位源码行，但栈回溯不可靠；未捕获异常
  仍只有 `pyadamas: uncaught <type>: <message>`，运行时 traceback 留待后续。
- `ctest --preset dev` 2/2；差分 840 个程序 0 失败；16MB 内存验收和
  `tests/check_size.sh` 通过；生成代码行为与 0.158 一致。

## [0.158] - 2026-11-14
正确性修复、asyncio 收尾与前端基线复测。

### 修复

- **NaN 的 `!=` 在比较+分支融合路径判错**：`if a != b` 生成
  `ucomisd + jne`，NaN 时 ZF=1 落到假分支；改为先把无序（PF=1）跳到真
  分支，再按 ZF 分派。`tests/run/48_branch_mnemonics.py` 补六种比较的
  NaN 回归。
- **生成器/协程 resume 复用上一个函数的浮点提升状态**：`assign_slot_regs`
  对生成器提前返回时没有清 `fp_promoted_slots_` / `slot_fpidx_`，包含
  `list[float]` 打印或 float gather 的协程会生成 `movsd , off(%rbp)` 这种
  空操作数汇编。生成器分支现在显式清空这两张表。

### 变更

- **Task 支持多个并发等待者**：完成时把结果/异常分发给 `waiters` 列表中的
  所有协程；取消等待对象型 Task 时同样通知全部等待者；等待者被取消时从
  列表摘除。`pa_asyncio_create_task` 新增 `result_kind` 参数，Task 结果的
  所有权按静态 `task[T]` 元素类型带入运行时。
- **gather 结果元素所有权按静态类型带入运行时**：`pa_asyncio_gather` 多一个
  `result_kind` 参数（0 裸值 / 1 堆引用 / 2 unknown），避免 float 位模式被
  当成堆指针 dec，也让字符串/对象结果按精确 RC 释放。
- **`from asyncio import ...` 保留特化类型**：`gather` / `run` / `wait_for` /
  `create_task` 等别名仍走 asyncio 的调用点规则，不再退回泛型
  `list[unknown]` / unknown。

### 验证

- `ctest --preset dev` 2/2；151 个运行用例（含新增 NaN、float gather、
  Task 多等待者/多等待者取消覆盖）。
- 差分 840 个程序，0 失败。
- 16MB `RLIMIT_AS` 下 13 个 RC/GC/异步用例全部通过；`tests/check_size.sh` 通过。
- 标准库前端基线（Debian CPython 3.13.5）：598 个文件，语法通过 596，类型通过
  281 / 596（47.1%）；去掉 `test/` 为 533 个文件、语法 532、类型 253 / 532
  （47.6%）。0.133 同口径为 277 / 596（46.5%），本版 +4。
- release 基准（本机 WSL，g++ 14.2，5 次取最小）：mandelbrot 25.8×、
  spectral_norm 29.5×、fib 14.9×、nqueens 8.5× vs CPython，9 个用例输出与
  CPython/C++ 一致；快照见 `bench/compare/RESULTS-0p158-wsl.md`。

## [0.157] - 2026-11-14

优化：浮点比较分支去掉 NaN 专用跳转。

### 变更

- `ucomisd` 对无序（NaN）把 CF/ZF/PF 全置 1。四种有序比较（`< <= > >=`）
  通过交换操作数顺序、改用 `ja`/`jae` 判断，让 NaN 自然落到「不满足」的
  一侧，于是不再需要先发一条 `jp`。内层循环里的浮点条件从
  `ucomisd + jp + jbe` 缩成 `ucomisd + jae`。
- 相等/不等比较仍保留原形式：`==` 在无序时 ZF=1，无法和相等区分。

### 验证

- dev 测试 164 个通过；`ctest` 2/2；差分 840 程序 0 失败；内存与规模检查通过。
- release：mandelbrot 12.1→12.0 ms，其余持平。

## [0.156] - 2026-11-14

优化：拷贝合并与跨块公共子表达式消除。

### 变更

- 拷贝合并：线性扫描分配临时寄存器时带 `v = a op b` 提示——`a` 的最后
  一个使用就是这条指令时，把 `a` 的寄存器直接分给 `v`，指令生成器看到
  目标就是左操作数便原地算（`subsd b, a`），省掉一条 `movsd`/`movq`。
  提示只接受本块临时池里的寄存器，跨块值和变量槽的寄存器不会被覆盖。
- 寄存器之间的浮点搬运用 `movapd` 做整 128 位拷贝。`movsd` 只写低 64 位，
  硬件要把它和目的寄存器的旧值合并，凭空多出一条依赖链；紧挨着复用同
  一个临时寄存器时这一条依赖能吃掉三成性能（实测 mandelbrot）。
- 新增 `opt_cse.cpp`：跨块公共子表达式消除。用前向「可用表达式」数据流
  合并条件块与循环体里重复的纯表达式，键把局部槽的当前值编码成
  `Slot(s)`、常量按值编码、交换律算子规范化；`store.slot` 杀掉依赖该槽
  的表达式，块入口取所有前驱出口的交集。`while i < n and zr*zr + zi*zi
  <= 4.0:` 的两次乘法因此从体块里消失。只删「所有使用都在本块内」的
  重复指令，取过槽地址的函数整函数跳过。比较指令不参与：它常和紧跟的
  分支融合成一条 `cmp+jcc`，融合后不会真的写出布尔值，跨块复用那个
  vreg 会读到没写过的寄存器。
- 上一版用来中转浮点中间结果的 `%xmm1` 融合在发生拷贝合并时让位，避免
  多一条 `movsd`。

### 工程

- 同机 A/B（release，三次取中）：mandelbrot 26.4ms → 12.0ms（2.2×，
  相对 CPython 23.3×，已接近 g++ -O2 的 9.4ms）；spectral_norm
  34.1ms → 30.0ms（1.14×）；nbody 17.2ms、matmul 12.3ms、fib 16.7ms、
  nqueens 77.5ms 持平；标准库 17 项基准无可见回退（statistics 5.4→4.6ms）。
- 164 运行用例、差分 840 程序 0 失败、ctest、memcheck、文件规模检查通过。

## [0.155] - 2026-11-14

优化：浮点数值循环的代码生成。

### 变更

- 块内浮点临时寄存器：确定是 float 的块内纯计算值分配到 `xmm3–xmm7`
  （`xmm0/1/2` 留给指令生成器），mandelbrot 内层循环不再为每个中间
  结果写/读栈槽。
- 浮点比较 + 分支融合：`ucomisd` 直接吃标志位，有序比较用 `jp` 排除
  NaN，省掉 `setcc`/`movzbq`/`cmpq $0`；整数比较路径不变。
- 浮点除法所在块不再被排除在块内临时寄存器之外（浮点除法只用 xmm0，
  不碰借出的 GPR）。
- 新增 `opt_forward.cpp`：把 `and`/`or` 短路布尔值经临时槽往返的形态
  改成块内转发 + 分支线程化 + 不可达块清理 + 死槽写入删除，循环条件
  直接走 cmp+branch。只转发布尔槽——转发浮点/堆槽会延长活跃区间，在
  nbody 这类含调用的热循环里反而变慢。
- 跨块浮点 vreg 可以使用 `xmm8–15` 中剩余的空闲寄存器（原上限 2 个）。
- 跨块 vreg 提升按「使用次数 × 循环深度」排序（支配集 + 回边求自然
  循环嵌套深度），内层循环每轮都读的值优先拿到寄存器，不再被循环外
  只执行一次的常量按 vreg 编号抢走；同时去掉调试用的 `PYADAMAS_NO_FPX`
  开关。

### 工程

- 同机 A/B：mandelbrot 47.2ms → 25.2ms（1.87×），相对 CPython 从 6.1×
  提到 11.6×；nbody 17.6ms 持平；其余语言基准无可见回退。
- 151 运行用例、52 反例、840 差分、ctest、memcheck、标准库 bench 全部通过。
- 槽位转发只做「块内、类型一致、所有使用同块」的值，并对转发链做传递
  解析，避免替换后引用到已删除的 vreg。

## [0.154] - 2026-11-14

工程治理：拆完全部 allowlist 大文件，所有源文件回到 1200 行以内。

### 变更

- `typecheck_call.cpp`（2106 行）按调用类别拆成入口/模块规则/方法表三块，
  新增 `typecheck_call_modules.cpp`、`typecheck_call_method.cpp`。
- `lower_expr.cpp`（2276 行）拆成容器提升、赋值与 `expr` 主分派、运算符与
  比较、容器构造、dunder 协议与转换五个文件。
- `lower_import.cpp`（2130 行）拆成模块别名/import 语句、原生调用、
  operator/collections、functools/datetime/pathlib/json 四个文件。
- `lower_stmt.cpp`（1968 行）拆成控制流/for、with/异常清理、async with。
- `lower_gen.cpp`（1389 行）拆成 yield 与生成器 for、async for 与可挂起
  函数编译、跨挂起溢出三块。
- `lower.cpp`（1267 行）把类相关成员搬到 `lower_class.cpp`，驱动留在原文件。
- `cdecl.cpp`（1247 行）拆成词法/驱动与解析器两部分，新增内部头
  `cdecl_internal.h`；`cdecl_parse.cpp` 保留类型解析与 `Cdecl`。
- `tests/check_size.sh` 的 `ALLOW_FILES` 清空：文件行数检查对所有文件生效。

### 工程

- 纯搬移，接口、报错文本与生成代码不变；151 运行用例、52 反例、840 差分、
  ctest、memcheck、release bench 全部通过。
- 函数级警告仍按约定只提示不阻塞（如 `gen_rt_args` 543 行、
  `builtin_module` 736 行）。

## [0.153] - 2026-11-14

工程治理：拆分剩余的上帝函数与大文件，并加入规模检查。

### 变更

- `gen_call_rt`：505 个运行期调用 case 按实参形态（全整数/全浮点/混合）
  抽成 17 个 `gen_rt_*` 装填函数，移到 `codegen_call_rt.cpp`；
  `codegen_call.cpp` 从 1701 行降到 954 行。
- `builtin_module`（736 行模块签名数据表）整段搬到 `builtin_sigs.cpp`。
- `lower_builtin_call`（1655 行）按家族拆成 `lower_builtin_reflect/seq/
  container/text/misc.cpp` 五个文件，入口只保留 print 特判、关键字拦截
  和按序分派。
- `TypeChecker::call` 按被调表达式形态拆成 `call_name` / `call_attr` /
  `call_value`；`TypeChecker::stmt` 的 15 类语句体搬到
  `typecheck_stmt_forms.cpp`，`typecheck_stmt.cpp` 从 1364 行降到 864 行。
- 头文件：`ir.h` 拆出 `ir_ops.h`（算子与运行期函数编号）和
  `ir_types.h`（数据结构与机器类映射）；`typecheck.h` 的类型模型搬到
  `typecheck_types.h`；`lower_impl.h` 里 25 个较大的内联成员体移到
  `lower_impl_util.cpp` / `lower_impl_slots.cpp`。
- 新增 `tests/check_size.sh`：源文件超过 1200 行直接失败（剩余待拆文件
  登记在允许清单），单个函数超过 300 行只警告；规则写进
  `docs/development.md`。

### 工程

- 纯机械搬移，报错文本、检查顺序和生成代码不变；151 运行用例、52 反例、
  840 差分、全量 bench 全部与拆分前一致。
- 拆分后仍超过 1200 行的文件：`lower_expr.cpp`（2276）、
  `lower_import.cpp`（2130）、`typecheck_call.cpp`（2106）、
  `lower_stmt.cpp`（1968）、`lower_gen.cpp`（1389）、`lower.cpp`（1267）、
  `cdecl.cpp`（1247）。

## [0.152] - 2026-11-14

工程治理：拆分 `lower_method.cpp` 的上帝函数。

### 变更

- `lower_method_call` 从 2131 行降到约 600 行，只保留模块/类接收者分派
  和用户类方法调用；其余按接收者类型抽成 15 个成员函数：
  `lower_method_native` / `generator` / `async_generator` / `task` /
  `future` / `dict` / `set` / `seq` / `str` / `bytes` / `random` /
  `regex` / `match` / `file` / `int`。
- 新增 `lower_method_native.cpp`（异步/原生类型）、
  `lower_method_containers.cpp`（dict/set/list/tuple）、
  `lower_method_str.cpp`（str/bytes）、`lower_method_misc.cpp`
  （random/re/file/int）；`lower_method.cpp` 从 2612 行降到 865 行。
- 纯机械拆分，不改变降级逻辑与生成代码；151 运行用例、52 反例、840
  差分、16MB memcheck、全量 bench 全部与拆分前一致。

### 工程

- 拆分前扫描：`lower_method_call` 2131 行（203 处 `at.attr` 分派）、
  `lower_builtin_call` 1655 行、`gen_call_rt` 761 行（505 个 case）、
  `builtin_module` 736 行（模块签名数据表）、`typecheck_stmt::stmt` 587 行。
  这批先处理最大的 `lower_method`，其余按风险从低到高后续处理。

## [0.151] - 2026-11-13

修复 `str(unknown)` 不做运行期类型分派。

### 变更

- 新增 `pa_str_any`：unknown 值先查 RC 索引，是堆字符串就原样返回（补
  一份引用），否则按整数格式化。`str()` 的 unknown 分支改走它，不再把
  字符串指针当整数格式化成地址数字。
- 新增运行用例 152（unknown 字符串/整数/负数的 `str()` 与 `len`）。

`repr(unknown)` 仍在降级期报「不支持」，不在这次修复范围内。

## [0.150] - 2026-11-13

GC 保守根收窄：不再扫描整段真实栈。

### 变更

- 帧记录 `PyAdamasFrame` 扩展成「影子根表」：新增临时区起点/字数；槽
  地址低两位区分「可能非堆」（bit0，异常展开按注册表 release）和「只给
  GC 扫描」（bit1，借来的 unknown 引用，异常展开跳过）。
- 含 unknown 局部/形参的槽登记到帧记录，GC 只扫这些地址；含 unknown
  vreg 的帧登记整段 vreg 槽区，纯类型化帧一个字节都不扫。
- `pa_frame_unwind` 跳过 scan-only 条目，异常路径不会释放借来的引用。
- exact-RC 模式保持完全不扫栈；含 unknown 的程序也不再扫整段栈。

### 性能

`bench_rc_gen` 的 GC 总耗时在噪声内（约 18.5ms），`bench_heapq` 20.7ms
不变；收益是根集合从「整段栈」缩到「登记过的槽/临时区」，栈上整数被
误认为根的面大幅减小。

## [0.149] - 2026-11-13

分代循环收集：minor 用 trial-deletion，major 保留标记-清扫。

### 变更

- 堆对象按「是否在当前收集集合」分代：上次收集后分配、还没晋升的对象
  进新生代 `young_buf`；minor 只对新生代做 trial-deletion，老对象的引用
  按真实 rc 计入外部引用、天然是根，不需要写屏障。
- minor 的根 = 试探减计数后 rc > 0 的对象 + 保守扫栈（unknown 局部槽这
  类借来的引用不计入 rc）；exact-RC 模式（整程序无 unknown）下所有引用
  都归槽/临时值/容器所有，直接跳过扫栈。
- major 仍走经典标记-清扫（集合 = 整堆），频率降到「老生代翻倍或 8MB」，
  比原来每 `live` 字节全扫一次少一半。
- 收集期间的 `pa_rc_dec` 按阶段分流：trial/restore 只调集合内计数；清扫
  时集合外的边先攒进 pending，回合结束再按正常 RC 释放，避免清扫中重入
  释放。
- 生成器帧的槽释放改走 `pa_gen_slot_release`：只有清扫阶段才清零槽，
  trial/restore 不会破坏生成器状态。
- 修复 `rc_index_has` 把墓碑哨兵 `(PyAdamasRc*)-1` 误判为命中的老 bug：
  unknown 值 `63` 减去 64 字节头正好等于哨兵，会解引用非法地址。
- 新增运行用例 151（新生代环、晋升后的老生代环、生成器帧与 unknown 局部
  槽里的环）与 `bench_rc_gen` 基准。

### 性能

`bench_rc_gen`（10MB 存活堆 + 大量短命容器）GC 总耗时 25.5ms → 17.5ms
（-31%），墙钟中位数 77.8ms → 69.1ms（-11%）；其余基准在噪声范围内
（`bench_heapq` 20.8ms 不变）。

## [0.148] - 2026-11-13

引用计数：裸 `list` / 空字面量的元素种类延迟到第一次写入时定型。

### 变更

- 列表新增元素类别 3「未定」：`list` / `[]` / `list()` 这类看不出元素
  种类的**空**容器先记 3，第一次 `append` / `insert` / 下标赋值 /
  `extend` 时由新增的 `pa_list_prep` 按写入值定型（堆→1，非堆→0，
  冲突降级为 2）。
- 写入路径统一走 `store_list_elem`：容器未定型、静态种类冲突或值是
  unknown 时调用 `pa_list_prep`，其余情况仍按静态种类直接 retain，
  省掉一次调用。
- `extend` / 切片赋值在目标未定型时采用源的种类，冲突时降级为 2；
  `set_from_list` / `Counter` 把源的 3 按 2 处理。
- 修复裸 `list` 上 `pop()` 的 use-after-free：删除元素前按列表种类补
  一份引用，返回值不再指向已释放对象。
- `PYADAMAS_RC_STATS` 增加 `inc_unknown` 字段。
- 新增运行用例 150（裸 list 定型、冲突降级、切片赋值、推导式、pop）。

### 性能

`bench_heapq` 的 `dec_unknown` 从 143 万降到约 10 万，墙钟 30.5ms →
20.9ms（CPython 40.8ms，1.95x）；其余标准库/RC 基准在噪声范围内无变化。

## [0.147] - 2026-11-13

引用计数：容器字面量与推导式的所有权转移。

### 变更

- `list` / `tuple` 字面量：新元素不再「先 retain、后 cleanup dec」，而是在
  容器构造成功后用 `pa_rc_temp_take` 把临时栈上的引用直接交给容器；中途
  元素求值抛异常时引用仍在临时栈上，由 cleanup 正常释放。
- `dict` / `set` 字面量、list / dict / set 推导式、下标赋值对 fresh 键、
  值、元素同样跳过 retain。
- 新增 `bench_rc_literals` 基准；运行用例 149 扩展到 dict / set / tuple
  字面量与推导式。

### 性能

`bench_rc_literals` 的 `rc_inc` 从 10.0 万降到 1.0 万，墙钟约 5.7ms（不转移
引用时约 5.9ms）；主要成本仍在字符串构造与格式化，所有权转移本身只省掉
一对 inc/dec。

## [0.146] - 2026-11-13

引用计数所有权转移与 RC/GC 统计。

### 变更

- 新增 `pa_rc_temp_take`：新产生的堆值写入局部槽 / 全局 / 字段 / 列表元素 /
  字典键值、`append` / `insert` 或作为返回值时，直接把临时栈上的引用转给
  目标，省掉「retain + cleanup dec」这一对；被别名后不再移动。
- `PYADAMAS_RC_STATS=1`：退出时打印 `rc_inc` / `rc_dec` / `rc_dec_unknown` /
  `temp` / `cleanup` / `free` 与 GC 次数、回收数、存活字节。
- 新增 5 个 RC/GC 基准：短命字符串、短命列表、参数/返回传递、对象环、
  大存活堆 + 短命临时值。
- 新增运行用例 149（所有权转移下的别名与旧值释放）。

### 性能

新值移动后 RC 调用数明显下降：`bench_rc_calls` 的 `rc_inc` 从 6.0 万降到 4，
`bench_rc_strings` 从 4.0 万降到 2，`bench_base64_hashlib` 从 16.0 万降到
8.0 万，`bench_string_textwrap` 从 89.8 万降到 53.4 万；对应墙钟提升约
3–12%（小用例受启动和噪声影响更大）。

## [0.145] - 2026-11-13

序列重复与 `base64` / `hashlib` 的 C 内建，修掉标准库基准里唯一慢于 CPython 的一项。

### 变更

- `str` / `list` / `tuple` 与 `int` 的 `*` 改为序列重复（两侧顺序均可），
  走运行时 `pa_str_repeat` / `pa_list_repeat`：一次分配 + `memcpy`，负次数得到
  空序列；`operator.mul` 对序列同样按此特化。此前这类表达式直接报
  「not supported by the code generator yet」。
- `hashlib.sha256` 的压缩函数改为 C 实现（`runtime/rt_hash.c`）：`SHA256`
  只累积 `bytes`，`digest()` / `hexdigest()` 调 `__pa_sha256` /
  `__pa_sha256_hex`，不再逐字节跑 PyAdamas 循环；md5 / sha1 保持纯 PyAdamas。
- `base64` 的 b64 / urlsafe 编解码改为 C 内建 `__pa_b64encode` /
  `__pa_b64decode`；b16 / b32 仍为纯 PyAdamas。
- 新增运行用例 148（序列重复）。

### 性能

`tests/bench` 的 `bench_base64_hashlib`（dev 构建，repeat 3）从 0.27x 提升到
3.05x（14.3ms vs CPython 43.7ms）；拆分测量中 sha256 从 125.8ms 降到 9.6ms，
b64 从 38.9ms 降到 5.3ms。release 下该项为 2.99x（14.7ms vs 43.9ms）。

## [0.144] - 2026-11-13

collections / datetime / pathlib 收尾，并固化标准库基准。

### 变更

- `collections.deque` 补齐 `remove`/`count`/`index`/`insert`/`reverse`/
  `copy` 与 `.maxlen`；`deque(...)` 保留元素类型（`pop`/下标/迭代不再退化成
  `int`），`append`/`insert` 按元素所有权补 retain。
- `datetime.timedelta` 增加 `//`（int 与 timedelta）和 `/`（int 与 timedelta）；
  `date`/`datetime` 增加 `replace()`；日期校验按实际月份天数（含闰年）。
- `pathlib.Path` 增加 `read_bytes`/`write_bytes`、`is_absolute`、`joinpath`、
  `relative_to`、`resolve`、`absolute`、`touch`。
- 新增 `tests/bench/`：同一份用例分别用 pyadamas 与 CPython 运行并取最优
  耗时（`python3 tests/bench/run_bench.py`），覆盖 itertools、collections、
  heapq、bisect、statistics、math、datetime、string/textwrap、json、
  base64/hashlib。
- 新增运行用例 145（deque）、146（timedelta 除法与 replace）、
  147（pathlib 扩展）。

## [0.143] - 2026-11-13

`operator` 改为内置模块并补齐包装函数。

### 变更

- `operator` 从纯 PyAdamas 子集改为内置模块：`add`/`sub`/`mul`/`truediv`/
  `floordiv`/`mod`/`pow`/`and_`/`or_`/`xor`/`lshift`/`rshift`/`concat`、
  比较 `eq`/`ne`/`lt`/`le`/`gt`/`ge`、`is_`/`is_not`、
  `neg`/`pos`/`abs`/`invert`、`truth`/`not_`、`contains`、`getitem`。
  结果类型与运算在调用点特化：数值提升、字符串拼接、容器相等、
  `datetime`/`timedelta` 比较都复用与运算符相同的 lowering。
- `itemgetter` 改为编译期合成闭包：支持单键与多键（多键返回元组），
  键目前限 `int`；`attrgetter` 需要运行期按名字取属性，调用点给出
  明确的不支持错误。
- 运行用例 144 扩展为完整的 operator 覆盖。

## [0.142] - 2026-11-13

标准库基础静态包（第二批）：itertools 组合函数、datetime.timedelta、operator 子集。

### 变更

- `itertools` 增加 `product`、`permutations`、`combinations`、
  `combinations_with_replacement`、`zip_longest`、`accumulate`。结果仍是
  立即求值的列表；`zip_longest` 的每一列按 `Optional[T]` 补齐 `None`。
- 新增 `datetime.timedelta`：新类型 `TimeDelta`。构造支持
  `days`/`seconds`/`microseconds`/`milliseconds`/`minutes`/`hours`/`weeks`
  （位置或关键字）；提供 `.days`/`.seconds`/`.microseconds`、
  `total_seconds()`、加减、乘整数、取负、比较，打印文本与 CPython 的
  `str(timedelta)` 对齐。
- `datetime`/`date` 与 `timedelta` 的加减、`datetime - datetime` 得到
  `timedelta`；`datetime`/`date` 之间的比较改为按值（原来按指针）。
- 新增纯 PyAdamas `operator` 子集：`itemgetter`（列表/元组下标）、
  `truth`、`not_`。`attrgetter` 需要运行期按名字取属性、算术与比较包装
  需要按调用点特化，留待后续。
- 新增运行用例 142（itertools 组合函数）、143（timedelta）、
  144（operator）。

## [0.141] - 2026-11-13

标准库基础静态包（第一批）。

### 变更

- 新增纯 PyAdamas `heapq`（push/pop/heapify/heapreplace/nlargest/nsmallest）、
  `bisect`（左右查找与 insort，当前限有序 `list[int]`）、`binascii`
  （hexlify/unhexlify/b2a_hex/a2b_hex）。
- `base64` 增加 `standard_b64encode/decode`、`b16encode/decode`、
  `b32encode/decode`、`encodebytes/decodebytes`。
- `statistics` 增加 `geometric_mean`；`textwrap` 增加 `shorten` 与
  `initial_indent`/`subsequent_indent` 参数。
- `pathlib.Path` 增加 `unlink()`、`rmdir()`、`with_suffix()` 和 `.parts`。
- 新增运行用例 140（heapq/bisect/binascii/base64/statistics/textwrap）与
  141（pathlib 扩展）。
- `itertools` 组合函数、`datetime.timedelta`、`operator` 留到下一批：
  它们分别需要新的 lowering、类型/运行期与 getter 建模。

## [0.140] - 2026-11-13

跨模块继承补完：运行期类表、导入类附加基类、导入字段默认值。

### 变更

- `PyAdamasClass` 增加 vtable 指针；`pa_class_new` 接收 vtable，
  新增 `pa_class_ctor` / `pa_class_method`。非入口模块编译的 classmethod
  把 `cls` 当运行期类对象：`cls(...)` 用实际子类的构造器，`cls.m(...)`
  通过类对象 vtable 动态分派；`cls.__name__` 返回实际类名。
- 新增 IR `class.addr` / `global.addr` / `call.ptr` 与 `pa_class_ctor` /
  `pa_class_method` 运行时入口；entry 模块内本地 classmethod 仍走静态
  cid 直调，避免回归。
- vtable 外部槽改为引用导出的裸代码符号
  `pyadamas$<模块>$ms$<类>$<槽>`，修复「引用 `vt + 8*slot` 数据槽地址」
  导致的跨模块继承调用崩溃。
- 模块接口 v10：字段带字面量默认值，方法带 `aliases` 槽表；导入类作为
  多继承附加基类时按 `(name, kind)` 把实现写到全部别名槽，不同名抢同一
  槽仍报错。新构建的模块也走一次接口编解码，保证导出签名里的类类型指向
  扁平 `ClassInfo`。
- `isinstance` 对本地类目标也改按限定名匹配，修复跨模块对象 MRO 里同号
  类编号的误匹配；本地类对象存限定名，`__name__` / `repr` 只显示最后一段。
- 边界：动态 classmethod 的 `cls.x` 类属性仍按声明类解析；子类 `__init__`
  签名需与基类兼容；动态 classmethod 的方法值暂不支持；导入字段的非字面量
  默认值在子类构造时报错。
- 扩展模块用例 `tests/modules/inherit_import`：字段默认值、`Sub2.make`
  构造子类、`cls.__name__`、导入类附加基类的别名槽与 `isinstance`。

## [0.139] - 2026-11-13

跨模块继承：导入类可以作为单继承的主基类。

### 变更

- 模块接口升到 v9：导出类的运行期限定名、完整祖先链、方法 `kind`
  （普通/静态/类方法/属性访问器）；导入方据此重建属性描述符并再次导出。
- 非入口模块为每个类导出 `pyadamas$<模块>$vt$<类>` 与
  `pyadamas$<模块>$os$<类>` 符号；导入方用外部 vtable 项填充子类未覆盖
  的槽，并为导入祖先发 cid=-1 的 mirror 元数据（限定名参与 `isinstance`，
  vtable/own_slots 参与 `super()`）。
- 方法槽跨模块对齐：类定义时预置导入基类的槽；同名同槽复用，同名不同槽
  或不同名同槽在类定义处报错，保证 vtable 同时满足两边的调用点。
- `super()` 的 after 类改为按运行期限定名定位，基类模块里的方法在子类
  实例上执行 `super()` 也能找到正确位置。
- 非入口模块不再做「类没有子类」的终结假设（关闭直调优化），避免导入方
  覆盖被基类模块绕过。
- 边界：只支持单个导入基类，导入类不能作为多继承的附加基类；导入基类的
  字段默认值、`@classmethod` 重绑定、装饰器方法不跨模块重建。
- 新增模块用例 `tests/modules/inherit_import`（多级继承、覆盖、`super()`、
  `isinstance`、导入类标注）。

## [0.138] - 2026-11-13

运行期类对象：类名可以当值用，类装饰器落地。

### 变更

- 新增 `PyAdamasClass` 运行时对象（类 id、构造指针、限定名）与
  `pa_class_new` / `pa_class_name` / `pa_class_repr` / `pa_isinstance_class`。
- 新类型 `ClassObj`：类名在值位置是 `type[类]`，可赋值、传参、打印、
  `__name__`、`isinstance`；本地类在类定义点生成单例，导入类按需生成
  （跨模块签名里仍降级为 unknown）。
- 静态类型已知的类值可直接构造；类装饰器在类定义点执行并接收类对象，
  返回值继续作为该类名的类对象。
- 边界：类装饰器必须返回类；返回替换类时 `C(...)` 仍按原类构造；
  异常类不支持类装饰器；unknown 类值不能当构造器。
- 新增运行用例 138 / 139，扩展模块 classes。

## [0.137] - 2026-11-13

方法装饰器的返回类型未知时不再被后端拒绝。

### 变更

- 装饰器返回类型在第一遍检查时可能还没定下来：类型检查收尾阶段为
  装饰方法补齐类属性条目，并按最终类型刷新 `decorated_ty` / `call_sig`
  （修复 `return f` 这类恒等装饰器在方法上的误拒）。
- 装饰器链结果类型未知时，调用改为按类属性里的函数值做 unknown 间接
  调用：实例调用与 `C.m(obj, ...)` 都支持纯位置实参；没有签名可重排，
  关键字实参仍报错。`super()` 上的未知装饰方法暂不支持。
- 新增运行用例 137。

## [0.136] - 2026-11-13

四个剩余边角：异构元组 repr、set 元素提升、导出类 varargs 构造、Queue 的 float 元素。

### 变更

- `repr()` / `str()` 支持异构元组：逐元素按各自静态类型渲染后用字符串拼接，
  单元素补尾逗号，嵌套元组递归；类元素走 `__repr__`，复数/range 也按各自
  文本形式。纯标量同构元组仍走 `pa_repr_tuple` 快路径。
- `set[int]` / `set[bool]` → `set[float]` 支持赋值与实参提升：按 set 的 dict
  载体逐元素转换后重建（float key 用数值哈希）。同时修正
  `pa_dict_key_at` 返回值的 ABI：float 元素也从 `rax` 取位模式，不再读
  `xmm0`。
- 模块导出类的 `__init__` 支持 `*args` / `**kwargs`：构造包装按方法 ABI
  追加 list/dict 形参并原样转发，模块接口沿用已有 varargs 编码，无需升
  格式版本。
- `asyncio.Queue` 支持 float 元素：`asyncio.Queue[float]` 标注把元素类型
  带进构造，运行时队列记录元素类别（裸值/引用），Future 增加
  `result_kind`，`put` / `get` / `put_nowait` / `get_nowait` 在裸值路径上
  跳过 RC；未标注队列保持原有引用语义。
- 新增运行用例 135 / 136，扩展 134 与模块 classes。

## [0.135] - 2026-11-13

语言边角收尾：嵌套类、类体 lambda、dunder 补齐、异步生成器表达式、导入类字段子模式、
容器元素隐式数值提升。

### 变更

- 函数内定义的类：类表收集会进入函数体，构造、方法调用、类内继承都可用；
  `Symbol` 新增 `is_class_name`，嵌套类名不再被当成「类实例变量」。
- 类体 `f = lambda self, ...: ...` 在脱糖阶段改写成方法定义，复用方法注册与编译。
- dunder 补齐：`__format__`（`format()` 与 f-string 普通格式说明）、
  `__bool__`（`bool()`、`if`/`while`、`and`/`or`/`not`/`assert` 的真值）、
  `__reversed__`（`reversed()` 物化成 list）。`and`/`or` 的真值按各操作数
  自己的静态类型判断，修掉了类实例被 join 成 int 后漏查 `__bool__` 的误判。
- `iter()` 支持类实例的 `__iter__` 和内置可迭代类型；`hasattr()` 对内建/容器
  类型静态判定、`Optional` 堆类型补裸指针非空检查；`setattr()` 支持带 setter
  的 property（走 setter 虚调用）。
- `C.m(obj, ...)` 直接调用类接收者的普通方法（显式传 self）。
- 异步生成器表达式 `(x async for x in agen())` 脱糖成异步生成器函数，可出现在
  任意表达式位置；`build_sig` 用语法预扫描区分「协程」和「异步生成器」，
  前置引用的异步生成器函数不再被当成 `Coroutine`。
- 导入类的类模式支持字段子模式：模块接口格式 8 导出按布局拍平的字段和
  `__match_args__`，`case mod.Point(x=1)` 与本地类语义一致。
- 容器元素隐式数值提升：`list[int]` / `dict[str,int]` 赋给或传给
  `list[float]` / `dict[str,float]` 时逐元素转成 double；`reduce()` 在
  回调形参为 float/unknown 时同样支持（回调按 int 检查过的组合仍明确报错）。
- 新增 `tests/run/132_dunder_edges.py`、`133_asyncgen_expr.py`、
  `134_container_promote.py`；`tests/modules/matchclasses` 增加导入类字段子模式。

## [0.134] - 2026-11-13

`finally` 体内跳出、`throw()`/`athrow()` 接受异常类、自定义 `__aiter__`。

### 变更

- `finally` 体内的 `return` / `break` / `continue`：跳出路径内联 finalbody，
  最终返回值/跳转以 finally 为准，异常被吞掉。非可挂起函数的 landing 路径
  先用 `ExcTakeValue` 把待重抛异常转成槽里的异常值，跳出时不会留下悬空的
  `pending_exc`。
- 生成器 `close()` 重放 finally 体时恢复 finally 词法上所属的循环，
  `break` / `continue` 能正常找到目标块（`Cleanup::Finally` 记录
  break/continue 目标与清理深度）。
- `throw()` / `athrow()` 接受异常类：`throw(ValueError)` 等价于
  `throw(ValueError())`；内置异常名与自定义异常类都支持，类型检查接受
  `Class` 形态的异常类实参。
- 自定义 `__aiter__`：`async for` 支持类实例。`__aiter__` 返回
  `AsyncGenerator` 时复用 `AsyncGenANext` 驱动；返回自身（或另一个带
  `async __anext__` 的对象）时每轮 `VCall __anext__` 并
  `suspend_on_awaitable`。装饰的 `__aiter__`/`__anext__` 走 `dunder_call`。
- 新增 `tests/run/129_finally_jump.py`（普通函数/生成器/协程的 finally
  跳出、异常吞掉、嵌套 finally）、`tests/run/130_throw_class.py`
  （内置/自定义异常类、未启动/已结束生成器、异步生成器）、
  `tests/run/131_async_aiter.py`（`__aiter__` 返回异步生成器、
  `__aiter__` 返回 self + `async __anext__`、break/continue/else），
  `build/memcheck.sh` 覆盖这三个用例。

## [0.133] - 2026-11-13

`Generator[Y, S, R]` 标注给 `send` 与 `yield` 表达式静态类型。

### 变更

- 注解解析：`Generator` 接受 1–3 个参数，`sub[0]` = yield 类型、`sub[1]` =
  `send` 值类型、`sub[2]` = `return` 值类型（只做记录，`StopIteration.value`
  仍是 unknown）；`AsyncGenerator` 仍只接受一个参数。
- 类型检查：函数返回 `Generator[Y, S, R]` 时，`x = yield v` 里 `x` 的类型是
  `S`，`.send(v)` 按 `S` 检查实参并返回 `Y`（`throw` 仍返回 unknown）。
  显式标注优先于生成器返回类型的推导结果，避免元素类型退化成 unknown 后
  把标注的 S/R 一起覆盖掉。
- 未标注时行为不变：`yield` 表达式与 `send` 值仍是 unknown，不做检查。
- 跨模块接口：`Generator` 额外编码 send / return 类型的 presence 字节与类型，
  模块格式版本升到 7。
- 新增 `tests/run/128_send_type.py`（`Generator[int, int, None]`、
  `Generator[str, str, None]`、只写 `Generator[int]` 三种）与反例
  `tests/fail/f20_send_type.py`（`send("oops")` 报
  `send() expects int, got str`），`build/memcheck.sh` 覆盖该用例。

## [0.132] - 2026-11-13

方法值（bound method）：`obj.m` / `C.m` 不加括号得到绑定接收者的函数值。

### 变更

- 类型检查：用户类的方法属性不再返回 `BoundMethod`，而是按
  `mk_method_value` 合成函数签名——实例上的普通方法去掉 `self`，静态/类
  方法保持原签名，装饰方法按包装函数签名去掉 `self`；类接收者
  （`C.m`）上的静态/类方法保持绑定语义。
- 降级：`lower_method_value` 为每次取方法值合成一个零参/一参闭包，
  捕获接收者（单元素 cell），调用时按方法种类分派：实例普通方法
  `vcall`（子类覆盖生效）、静态/类方法 `vcall_static`、类接收者
  `call_direct` 到该类的 vtable 最终实现、装饰方法调用类属性里的包装
  函数并补 `self`。默认参数与关键字参数沿用合成签名。
- 实例属性读取顺序调整：先查方法再查类属性，避免装饰方法的包装函数
  被当成普通类属性读出来。
- 新增 `tests/run/127_bound_method.py`（变量/容器/实参传递、默认与关键字
  参数、静态/类方法、子类覆盖、装饰方法、optional 接收者），
  `build/memcheck.sh` 覆盖该用例。

## [0.131] - 2026-11-13

vtable 槽 dunder 支持装饰器：构造、`with` / `async with`、`__call__`。

### 变更

- `__init__`、`__enter__`、`__exit__`、`__aenter__`、`__aexit__`、
  `__call__` 与算术/比较那批 dunder 一样，可以套自定义装饰器。装饰后的
  实现不填 vtable 槽，调用点统一用 `dunder_call` 从类属性取包装函数并
  显式补上实例，子类覆盖仍然生效。
- 构造路径：`lower_construct` 与跨模块/前向引用的构造包装函数
  （`compile_ctor_wrapper`）都按 `decorated` 分派，装饰器可以改
  `__init__` 的副作用和返回值。
- `with` / `async with`：`FuncCtx::Cleanup` 新增 `exit_cls`，退出路径
  （正常、`return`/`break`/`continue`、异常展开）都从类属性分派；绑定值
  的类型按包装函数的返回值计算。
- 实例调用 `obj(...)`：类型检查新增 `name_is_class`，用
  `class_obj_names_` 区分「类名引用（构造）」和「装着实例的变量/调用
  结果（`__call__`）」；没有 `__call__` 时报「not callable」。降级侧
  `lower_call` 用同样的判别走 `dunder_call`。
- 新增 `tests/run/126_decorated_slots.py`（装饰 `__init__`、`with`
  正常/异常/抑制、普通与装饰 `__call__`、链式 `Adder(10)(1, 1)`、
  装饰 `async with`），`build/memcheck.sh` 覆盖该用例。

## [0.130] - 2026-11-13

跨模块生成器 / 协程 / 异步生成器保留元素类型。

### 变更

- `.pya` 接口编码新增 tag 14/15/16：`Generator[T]`、`Coroutine[T]`、
  `AsyncGenerator[T]` 不再降级为 unknown；导入方可以直接用 `for` /
  `next` / `send` / `await` / `async for`，元素类型参与运算。
- 其余 asyncio 对象（`Task` / `Future` / `AsyncEvent` 等）仍按 unknown。
- 新增 `tests/modules/gen_interface`：跨模块生成器、协程、异步生成器。

## [0.129] - 2026-11-13

异步生成器内部的 `await` 结果不再丢失。

### 变更

- 异步生成器驱动对象新增 `driver_await_result`：内部 awaitable 完成时
  已把结果/异常写进帧槽（`p[2]` / `p[8]`），下一轮 `agen_drive` 不再用
  `driver_sent`（普通 `anext` 为 0）把它覆盖掉。
- 修复 `async for` / 异步推导式里「`__anext__` 返回堆值」时的引用计数
  错误：`handler_leave` 会按旧水位清理临时栈，之前刚送达的值已经入栈，
  被当场释放。现在先离开 handler，再把结果压入临时栈。
- `suspend_on_awaitable` 新增 `push_result_temp` 参数，`async for` 与
  异步推导式在 `handler_leave` 之后自行入栈。
- 新增 `124_asyncgen_await.py`（标量/字符串/列表结果、内部 await 抛异常
  被 `try/except` 接住、`asend`、`async for`、`aclose`），`build/memcheck.sh`
  覆盖该用例。

## [0.128] - 2026-11-13

生成器/协程的 `finally` 体内支持挂起。

### 变更

- `finally` 体内可以写 `yield` / `yield from` / `await`，恢复状态只有一套：
  可挂起函数里 finalbody 只编译一份，正常路径与异常 landing 汇聚到同一个
  `finally.fin` 块。异常路径不再把待重抛异常留在全局 `pending_exc`，而是
  用 `pa_exc_take_value` 复制成异常值存进帧槽（`Exception` 堆槽，随
  release/close 自动释放），finalbody 结束后 `pa_exc_reraise_value_if`
  非空就重抛——体内再 `raise` / 嵌套 `try/except` 不会覆盖它。
- `close()` 按快照重放 finalbody 时若再次遇到挂起点，按 CPython 报
  `RuntimeError: generator ignored GeneratorExit`（协程/异步生成器用对应
  措辞），不再真的挂起。
- `break` / `continue` / `return` 穿越含挂起 finally 的内联副本时，发射
  finalbody 期间把编译期 cleanup 栈截断到该 finally 之外，避免恢复时去
  suspend 已经释放的 handler 记录。
- 运行时新增 `pa_exc_take_value` / `pa_exc_reraise_value_if`。
- 新增 `123_finally_yield.py`（含异常重抛、close、`break`、`yield from`、
  `with`、`send` 与协程 `await`），`build/memcheck.sh` 覆盖该用例。
- 限制：`finally` 体内的 `return` / `break` / `continue` 仍不支持。

## [0.127] - 2026-11-13

生成器 `.throw()` 与 `yield from` 的 throw 转发。

### 变更

- `Generator.throw(exc)`：把异常注入当前挂起点，生成器自己的 `try` /
  `except` 能接住；没人接住时异常随生成器结束抛给调用方。已结束的生成器
  直接抛给调用方，未启动的生成器从函数入口抛出（函数体不执行）。只接受
  异常实例，不接受异常类。
- `yield from` 转发 `throw`：外层停在委托挂起点时异常交给子生成器的
  `throw`，按「子生成器产出 / 结束 / 抛出」分流——产出转 yield、结束取它的
  `return` 值、没接住则在外层挂起点继续传播（外层 `try` 能抓）。委托序列
  （list/str/...）不转发。
- 生成器的恢复入口现在和协程共用 throw 分发：先重挂 handler 并在本帧重新
  `setjmp` 再 rethrow，避免跨调用点 longjmp 到上次 resume 的栈帧。委托中的
  挂起点用新指令 `gen.take.exc` 取走异常（清零帧槽）后跳回委托循环。
- 运行时新增 `pa_gen_throw` / `pa_gen_throw_opt`；`yield from` 循环多了
  `exc_slot` / `ev_slot` 两个帧槽。
- 新增 `122_gen_throw.py`，`build/memcheck.sh` 覆盖该用例。

## [0.126] - 2026-11-13

装饰 dunder 方法。

### 变更

- 走 `dunder_call` 的那批 dunder 现在可以加自定义装饰器：`__str__` /
  `__repr__`、`__eq__` / `__ne__` / 比较、算术与反射/增量、`__contains__`、
  `__len__`、`__hash__`、`__iter__`。调用点从类属性取包装函数并显式补
  self；dict/set 键槽在 `__hash__` 或 `__eq__` 被装饰时退回机器字身份。
- 类型检查在类定义点把被装饰方法的类型回填给装饰器的第一个形参，
  单态化重查后刷新 `decorated_ty` / `call_sig` / 类属性类型。包装体里的
  `f(self, ...)` 不再退化成 unknown，字符串结果也不会被当整数拼接。
- 构造（`__init__`）、`with` 协议（`__enter__` / `__exit__`）、`__call__`
  等仍由 vtable 槽或专门路径分派，装饰时继续明确报错。
- 新增 `121_decorated_dunder.py`。

## [0.125] - 2026-11-13

推导式里的 lambda；`dict(iterable)`。

### 变更

- 含 lambda 的 list/set/dict 推导式在脱糖阶段改写成「生成器表达式 +
  `list()` / `set()` / `dict()`」：循环变量成为合成生成器函数的局部
  变量，lambda 的闭包捕获按常规路径工作，语义与 CPython 一致（所有
  lambda 共享循环结束时的值）。`[lambda: i * 10 for i in range(3)]`
  得到 20/20/20，`{i: (lambda: i) for i in range(2)}` 也能用。
  不含 lambda 的推导式仍走原来的就地展开路径，性能不变。
- `dict(iterable)`：元素是 `(key, value)` 二元组时构造字典，新增运行时
  `pa_dict_from_pairs`；`dict((x, x*x) for x in ...)` 与
  `dict([(1, 2), (3, 4)])` 都可用，`dict()` 的结果类型从元素类型推成
  `dict[K, V]`。`d[key]` 的 value 随之有精确类型。
- 新增 `120_comp_lambda.py` 回归用例。

### 仍未支持

- lambda 的默认参数引用循环变量（默认值全局只支持字面量常量，在调用点
  内联填充）。

## [0.124] - 2026-11-13

方法装饰器的剩余边界（类接收者与 super）。

### 变更

- `C.m(obj, ...)`：类接收者调用装饰方法时，按类属性里包装函数的未绑定
  签名检查实参（显式传 self），降级走 `class_attr_load` + 普通 `Call`。
  子类类接收者读的是继承链上的同一份类属性。
- `super().m(...)`：目标是装饰方法时，在声明类上取包装函数并自动补
  `self`；不再报「decorators on super targets are not supported yet」。
- `95_method_decorator.py` 增补类接收者、子类类接收者和 `super()` 用例。

### 仍未支持

- 装饰 dunder 方法（构造/运算符走 vtable 静态分派）。
- 装饰后的方法值绑定 self（需要 bound method 对象）。

## [0.123] - 2026-11-13

生成器/协程挂起点泛化。

### 变更

- `yield` / `await` / `yield from` 不再要求是语句里第一个求值的东西。
  降级完成后对 resume 函数做 vreg 活跃性分析：续跑块的 live-in 就是
  挂起期间还活着的值，挂起前 retain 一份写进生成器堆帧槽，恢复后装回
  并把所有权交给临时栈。`f(g(), (yield x))`、`10 + (yield x)`、比较链、
  f-string、容器字面量等位置都可用了；寄存器分配对「同一 vreg 多次
  定义」按跨块处理，独占一个栈槽。
- `except` 体内的挂起：resume 入口用 `pa_exc_enter` 换入本帧的异常
  上下文（当前异常 + 保存节点栈），挂起时 `pa_exc_leave` 换回调用方
  那份；异常逃出 resume 时驱动层按进入深度 `pa_exc_unwind` 退栈。
  生成器在 except 体里挂起期间，调用方的 except 状态不受影响，恢复后
  裸 `raise` 仍然重抛生成器当前处理的异常。修正了 except 体重挂
  handler 缺少 landing 分支的问题（之前禁止在 except 内挂起，不会
  触发）。
- `yield from` 委托生成器时，表达式的值是子生成器的 `return` 值
  （`pa_gen_send_opt` 在结束步转移 out 槽）；委托序列仍是 None。
  生成器的 return 值不再直接丢弃，没人取走时由帧销毁释放。
- 新增 `118_suspend_expr.py`、`119_gen_except_suspend.py`，扩写
  `28_yield_from.py`；`build/memcheck.sh` 纳入这三个生成器用例。

### 仍未支持

- `finally` 体内的挂起（close 路径要复制 finally 体，和体内挂起冲突）。
- `yield from` 的 `throw` 转发（生成器还没有 `.throw()`）。

## [0.122] - 2026-11-13

链式调用回归与未支持特性盘点。

### 变更

- 跨模块类引用修复后，`hashlib.md5(b"x").hexdigest()`、
  `logging.getLogger(...).info(...)` 这类链式调用已可用；新增
  `117_chain_call.py` 回归用例，更新 logging/hashlib 文档中的旧限制。

## [0.121] - 2026-11-13

release 基准复跑与 C 头文件导入（cdecl）。

### 新增

- `pyadamas cdecl <header.h> -o <out.cpya> [--link ARG]`：把 C 头文件的
  声明子集转换成 `.cpya`。支持 `#define` 数值/字符串常量、`typedef`、
  `struct`/`enum`、函数原型、`extern` 变量、函数指针参数与可变参数；
  `const char*` → `str`，`T*` → `ptr[T]`，`void*` → `ptr`。
  宏函数、条件编译、C `float`、union 按值、定长数组会跳过或在 stderr
  给出 warning；union/复杂 struct 降级为不透明 `struct`。
- `tests/cdecl/` 端到端用例：生成 `sample.cpya`、链接 `sample.c` shim、
  运行 `main.py`，并接入 `tests/run_tests.sh`。
- release preset 复跑九个基准（[RESULTS-release.md](bench/compare/RESULTS-release.md)、
  `results-release.json`）。生成代码默认 `-O2`，所以 release 与 dev 的
  运行时间基本一致，差异主要来自测量噪声。
- `bench/compare/run_compare.py` 的 Markdown 会按实际二进制标注
  `dev（-O0）` / `release（-O2）`。

## [0.120] - 2026-11-13

标准库补充：`hmac`。

### 新增

- 标准库 `hmac`：`new(key, msg=b"", digestmod="sha256")` 返回带
  `update` / `digest` / `hexdigest` 的对象，支持 `"md5"` / `"sha1"` /
  `"sha256"` 字符串名；>64 字节 key 先摘要，增量 update 与 CPython 一致。
- 测试新增 `116_hmac.py`（三种摘要、长 key、增量更新）。

## [0.119] - 2026-11-13

标准库补充：`base64` 与 `hashlib`，以及跨模块接口增强。

### 新增

- `bytes(list[int])` / `bytes(n)` / `bytes()` 内置转换；越界元素抛
  `ValueError`。字符串与字节串仍共用运行时表示，零成本。
- 标准库 `base64`：`b64encode` / `b64decode` / `urlsafe_b64encode` /
  `urlsafe_b64decode`，解码兼容两种字母表。
- 标准库 `hashlib`：`md5` / `sha1` / `sha256` 的 `update` / `digest` /
  `hexdigest`，纯 pyadamas 实现，标准向量与多块输入均与 CPython 一致。
- 跨模块接口现在按名字序列化类引用（tag 13），导入模块的函数返回类实例
  时类型不再退化成 `unknown`（如 `hashlib.md5(...)` 的返回值）。
- 默认参数字面量新增 `bytes`（tag 7），跨模块调用 `md5()` 这类带
  `b""` 默认值的函数不再把默认值当成 `None`。
- 模块接口格式版本升到 6。
- 测试新增 `114_base64.py`、`115_hashlib.py`（MD5 / SHA-1 / SHA-256
  标准向量、增量 update、200 字节多块输入）。

## [0.118] - 2026-11-13

基准扩充到九个用例。

### 新增

- `bench/compare/` 新增 fannkuch（全排列 + 前缀翻转）、binary_trees
  （完美二叉树分配与递归统计）、fasta（伪随机 DNA 序列生成）三个
  Benchmarks Game 经典用例，每个都有 PyAdamas / CPython / Go / C++
  四份实现，输出逐字节一致。
- `binary_trees.py` 用字符串标注 `"Optional[Node]"` 同时兼容 CPython
  与 PyAdamas（py23 没有 `typing` 模块）。
- 重新生成 [RESULTS.md](bench/compare/RESULTS.md)（每例 5 次取最小）：
  PyAdamas 比 CPython 快 6.6×–27.8×，与 gccgo 同数量级；新用例中
  binary_trees 10.1×、fannkuch 11.5×、fasta 12.8×。

## [0.117] - 2026-11-13

C 互操作：回调（C 函数指针）。

### 新增

- `.cpya` 参数类型 `fn(T1, T2, ...) -> R` 声明 C 回调，调用时传 py23
  函数或闭包；签名只允许标量/指针，参数上限 6 整数 + 8 浮点。
- codegen 为每个回调参数生成跳板与闭包槽：调用前登记闭包，跳板把闭包
  放回 `%r10` 后进入 py23 函数，因此捕获环境的闭包可用；`int32`/
  `uint32`/`bool` 参数按声明宽度提升。
- 语言标注支持 `ptr[int]`/`ptr[float]`/`ptr[bool]`，保留元素种类，
  回调里可直接 `a[0]` 解引用。
- 测试新增 `112_callbacks.py`（int/float/int32/bool、闭包捕获）、
  `113_qsort.py`（`qsort` 排序）与反例 `f54_callback_not_func.py`。

## [0.116] - 2026-11-13

C 互操作：指针解引用与可变参数。

### 新增

- `ptr[T]` 保留元素类型，支持 `p[i]` 读写（`int`/`float`/`int32`/`uint32`/
  `bool`），按元素宽度比例寻址；`int32` 符号扩展、`uint32` 零扩展，
  `float` 使用 `movsd`。指针为 `None` 时抛 `AttributeError`。
- 新增 IR 指令 `ptr.load` / `ptr.store`；元素类型随变量传播
  （手写 `Optional[int]` 标注会丢失元素类型）。
- `.cpya` 的 `extern def` 支持末尾 `...` 声明 C 可变参数函数；
  额外实参按 py23 类型编组（int/bool→整数寄存器、float→xmm、
  str→data 指针、Optional[str]/ptr），调用前按 SysV 设置 `%al`。
- 测试新增 `110_ptr_deref.py`、`111_varargs.py`，并在
  `108_structabi.py` 覆盖 4 字节指针的符号/零扩展。

## [0.115] - 2026-11-13

C 互操作：结构体按值传参与返回。

### 新增

- `.cpya` 的 `extern def` 可以直接使用带字段的 `struct`（不带 `ptr[]`），
  按 SysV x86-64 聚合 ABI 编组：≤16 字节按 8 字节 chunk 分为
  INTEGER/SSE，分别走整数寄存器或 `xmm0–7`；寄存器不足时整块走栈；
  >16 字节的参数全部在栈上，返回值通过隐藏指针（`rdi`）传回。
  INTEGER 返回值按 chunk 依次放在 `rax`/`rdx`，SSE 在 `xmm0`/`xmm1`。
- 单符号覆盖 4/8/16 字节 INTEGER、16 字节双 SSE、24 字节 MEMORY 的
  传参与返回；混合 `标量 + 大结构体 + 标量` 时隐藏指针与整数寄存器
  正确移位。
- C 结构体可以在 py23 里按字段构造（`DivT(7, 3)`），作为值类型参与
  赋值/传参/返回；payload 布局与 C 完全一致，复制沿用 `pa_struct_copy`。
- 不透明 `struct NAME` 仍只能以 `ptr[NAME]` 使用，按值出现会报
  `struct 'NAME' is opaque and cannot be passed by value`。
- 测试新增 `107_cstruct_byval.py`（`div`/`ldiv`/`inet_ntoa`）、
  `108_structabi.py` + `structabi.c`（SSE/MEMORY shim）与反例
  `f52_cstruct_opaque_byval.py`。
- `.cpya` 支持 `extern var NAME: type` 读取 C 全局变量，赋值按声明宽度
  写回；支持 int/int32/uint32/float/bool/ptr/ptr[Struct]/str，
  `str` 变量只读。`stdout`、`optind` 等可直接使用。
- 测试新增 `109_extern_var.py` 与反例 `f53_cimport_assign_func.py`。

## [0.114] - 2026-11-13

C 互操作 v4、四语言经典基准与常用标准库补充。

### 新增

- `.cpya` 结构体可以带字段：`struct NAME:` 后接缩进的
  `field: type`，按 C 规则计算布局（`int` 8、`int32`/`uint32` 4、
  `float` 8、`bool` 1、`ptr` 8）。`ptr[NAME]` 的类型是
  `Optional[ptr[NAME]]`，可直接读写字段，读取时做空指针检查。
- 字段读写复用编译期固定偏移；`int32` 符号扩展、`uint32` 零扩展，
  IR 的 `field.get`/`field.set` 新增 4 字节宽度与无符号标志。
- C 结构体指针只是 C 地址，没有 RC 头，不参与引用计数。
- `bench/compare/`：nbody、spectral_norm、matmul、nqueens、mandelbrot、
  fib 六个经典基准，每个都有 PyAdamas / CPython / Go / C++ 四份实现，
  `run_compare.py` 统一构建、计时（多次取最小）、校验输出并生成
  [RESULTS.md](bench/compare/RESULTS.md)。dev preset 下 PyAdamas 比
  CPython 快 6×–25×（spectral_norm 25×、fib 21×、matmul 19×、
  nbody 17×、nqueens 7×、mandelbrot 6×），与 gccgo 同一数量级，
  C++ 仍最快。
- 标准库 `logging`：等级常量、`basicConfig`、`getLogger`、
  `getLevelName`/`isEnabledFor` 与 `Logger` 的常用方法，默认写 stderr。
- 标准库 `statistics`：mean/median/mode/方差/标准差/调和平均等子集。
- 标准库 `textwrap`：`wrap`/`fill`/`dedent`/`indent` 子集。
- `argparse` 仍未提供：返回对象需要动态属性，与静态布局冲突，已在
  roadmap 技术债中记录。

### 验证

- 新增 `104_cstruct`（gmtime 的 `struct tm` 字段读写、负数 int32、
  `tm_zone` 判空）、`105_stdlib_logging_stats`、`106_textwrap`；
  反例 `f51_cstruct_field`（`.cpya` 字段类型不支持时报错）。
- `ctest --preset dev`、完整 `tests/run_tests.sh`、`build/memcheck.sh`
  全绿。

## [0.113] - 2026-11-13

语言级 `struct` 值类型，语义与 C 结构体一致。

### 新增

- 模块顶层 `struct NAME:` 定义，字段限 `int` / `float` / `bool`；`struct`
  是软关键字，`struct = 1`、`import struct` 不受影响。
- 布局自实现，与 C 完全一致：字段按声明顺序，`int`→`int64_t`、
  `float`→`double`、`bool`→`_Bool`，逐字段对齐，整体按最大对齐补齐；
  不依赖构建期 C 探针或外部编译器源码。
- `Point(...)` 按字段顺序构造；字段读写编译期固定偏移，`bool` 字段按
  1 字节读写；`sizeof(T)` / `sizeof(v)` 为编译期常量；`addr(p)` 得到
  `ptr[Point]`，就是 payload 地址，可直接传给 C。
- 值语义：赋值、传参、返回都复制整块 payload；容器仍存引用（与类实例
  一致，连续数组以后用 buffer）。
- `print(struct)` 明确报错，替代打印指针数值。

### 修复

- `re.Match.group(n)`（n > 1）构造列表时漏传元素 kind，暴露为
  `invalid list element kind`。

### 验证

- 新增 `102_struct`：布局/构造/字段/值语义/传参返回/`addr`/`bool` 字段/
  局部 struct/复合赋值；`103_struct_c`：`cimport clib` +
  `c.memset(addr(p), 0, 16)`，证明与 C 布局一致。
- 新增反例 `f46`–`f50`：字段类型、重复字段、构造参数个数、定义位置、
  直接 `print`。
- `ctest --preset dev`、完整 `tests/run_tests.sh` 全绿。

## [0.112] - 2026-11-13

C 互操作 v3 第一步：不透明结构体指针，可接 `FILE*`、`sqlite3*` 等句柄。

### 新增

- `.cpya` 支持 `struct NAME` 声明：只登记类型名，不描述布局；解析时先
  扫描全部 `struct` 行，允许先用后声明。
- `ptr[Struct]` 作为参数/返回类型，C 侧为 `Struct*`，py23 类型系统里仍是
  `Optional[int]`，可判 `is None`。
- `ptr[Struct]` 不接受 `list`/`bytes` 实参，避免把缓冲区误当句柄；普通
  `ptr`/`ptr[T]` 的缓冲区规则不变。
- 文档补充不透明结构体指针与 `fopen`/`fputs`/`fclose` 示例。

### 验证

- 新增 `101_cimport_struct`：`tmpfile` 取句柄、`fputs` 写入、`fclose`
  关闭，并验证 `fopen` 失败返回 `None`。
- 新增反例 `f45_cimport_opaque_buffer`：`list[float]` 传给 `ptr[FILE]`
  被拒绝。
- `ctest --preset dev`、完整 `tests/run_tests.sh` 全绿。

## [0.111] - 2026-11-13

C 互操作 v2：`ptr[T]` 可直接接收 py23 缓冲区，支持把数组交给 BLAS 等 C API。

### 新增

- **缓冲区传参**：`ptr[float]` 接 `list[float]`（`double*`）、`ptr[int]` 接
  `list[int]`（`long*`）、无类型 `ptr` 接 `list[float]`/`list[int]`/`bytes`；
  编译器取 `data` 字段（list/bytes 均偏移 16）直接传给 C。元素宽度不匹配的
  组合（如 `list[int]` → `ptr[float]`、`bytes` → `ptr[float]`）报错。
- **按调用点编组**：IR 的 `CallExtern` 记录每个实参的 `CKind`，同一符号既可
  接地址也可接 list/bytes；`BufF64`/`BufI64`/`BufBytes` 在汇编中先判空再取
  `data`，空 list 的 NULL 原样传 0。
- 文档补充缓冲区规则、生命周期约定与 `cblas_dgemm` 声明示例。

### 验证

- 新增 `98_cimport_buffer`：`modf` 写 `list[float]`、`time` 写 `list[int]`、
  `memcmp` 比较 `list[float]` 与 `bytes`。
- 新增 `99_cblas`：在真实 OpenBLAS 上跑 `cblas_dgemm`（2×2 方阵、2×3·3×2
  非方阵、带转置）与 `cblas_ddot`，验证 14 参数混合编组与数组指针；开发容器
  已安装 `libopenblas-dev`。
- 新增 stdlib `linalg`：`matmul`（支持 `trans_a`/`trans_b`）、`matmul_into`
  复用输出缓冲、`matvec`、`axpy`、`scal`、`norm`、`transpose`、`dot`
  封装 BLAS，用户只传行主序数据与维度；配套 `stdlib/cblas.cpya`。
- 新增 `100_linalg` 用例；`tests/run_tests.sh` 在缺少 OpenBLAS 时跳过
  `99_cblas`/`100_linalg` 而不是判失败。
- 新增反例 `f44_cimport_buffer_mismatch`。
- 本机基准（行主序方阵，BLAS 多线程 vs py23 三重循环）：64² 约 20–27×，
  128² 约 60–73×，256² 约 39–71×，512² 约 65–83×（512² 单次约 6 ms
  对 420 ms）。
- `ctest --preset dev`、完整 `tests/run_tests.sh`、`build/memcheck.sh` 全绿。

## [0.110] - 2026-11-13

C 互操作 v1：新增 `cimport` 语句与 `.cpya` 声明文件，可直接调用系统 C 库。

### 新增

- **`cimport`**：模块顶层语句，支持 `cimport a.b`、`as` 别名和一行多个名字；
  不产生运行时对象，当值使用会报错。
- **`.cpya` 声明文件**：`cmodule` / `link` / `const` / `extern def` 四种声明，
  支持符号改名、多个 `link` 去重、编译期常量内联。文件按 `a.b` → `a/b.cpya`
  相对源文件目录或 `-I` 路径查找，不进入模块缓存，**不影响既有 `.pya` 二进制接口**。
- **调用编组**：`int`(long)、`int32`/`uint32`、`float`(double)、`bool`、
  `str`(char*)、`ptr`/`ptr[T]`(void*/T*)、`None`、`Optional[str]`；返回值
  按类型做符号/零扩展、bool 归一、char* 复制成 `str`（NULL 对 `Optional[str]`
  返回 `None`）。
- 链接参数自动拼到最终 `cc` 命令。

### 验证

- 新增 `97_cimport`：调用 `libm`/libc 的 `sqrt`/`pow`/`strlen`/`labs`/`abs`/
  `isalpha`/`malloc`/`free`/`memset`/`getenv`/`syscall`，覆盖常量内联、别名、
  多绑定、关键字参数、`ptr[T]`、`Optional[str]` 传 NULL，以及 8 个整型参数
  走栈的编组。
- 新增模块用例 `cimport_shared`：两个模块各自 `cimport` 同一 `.cpya`，验证
  链接参数去重与模块缓存二次运行。
- 新增反例 `f41_cimport_value`、`f42_cimport_nested`、
  `f43_cimport_optional_str`（拒绝把可能为 NULL 的 `Optional[str]` 传给 `str`）。
- `str` 入参汇编先做空指针检查；类型检查拒绝 `Optional[str]` → `str` 的隐式收窄。
- `ctest --preset dev` 2/2，完整 `tests/run_tests.sh` 全绿。

## [0.109] - 2026-11-12

unknown 引用计数精确化：容器 kind 2 元素、unknown 字段与全局改走 RC 索引释放。

### 修复

- **容器 kind 2**：`list`/`dict`/`set` 的 unknown/混合元素不再「只 retain
  不 release」。写入统一 retain（堆→`rc_inc`，unknown→`rc_inc_unknown`），
  覆盖、截断、`del`、销毁统一 `pa_rc_dec_unknown`（先查 RC 索引，整数是
  空操作）。运行时辅助构造路径（extend/reversed/slice/enumerate/zip/
  keys/values/items/update/copy/popitem/set_from_list/deque/random/
  itertools）同步补齐 retain；目标容器原本是 kind 1 而源是 kind 2 时
  运行期把目标提升为 2。
- **unknown 字段**：`IRClass.field_heap` 增加 2（unknown 字段）；类析构器
  与 GC release 改调 `pa_rc_dec_unknown`，`store_field` 覆盖旧值同样按
  注册表释放，unknown 源写入字段时补一份引用。
- **unknown 全局**：`IRGlobal.maybe_unknown` 记录「写过的值可能非堆」；
  `store_global` 的旧值释放与出口 `release_globals` 改走注册表，
  unknown 源写入补 `rc_inc_unknown`。
- **槽/帧记录**：unknown 流过静态堆槽时打 `slot_unknown` 标记，出口、
  异常展开的帧记录和内联入口释放都改走 `pa_rc_dec_unknown`；生成器帧的
  `slot_heap` 增加 2，GC 扫描 kind 2 列表时保守查表标记。
- **队列所有权**：`pa_queue_get` / `get_nowait` 摘元素改用 `list_take_at`，
  把引用直接转交给 Future/调用方，避免新增的 `del` 释放提前回收。

### 验证

- 专项：`dict[str, unknown]` 2000 轮峰值 158MB → 2.1MB；unknown 字段
  310MB → 2.0MB；unknown 值流进堆槽的段错误用例输出正常。
- 新增 `96_unknown_rc`：unknown dict/list 的切片、反转、enumerate/zip，
  unknown 字段覆盖，unknown 全局覆盖，输出与 CPython 一致。
- `ctest --preset dev` 2/2；16MB `RLIMIT_AS` 验收 6/6；差分 runner
  280 例 0 失败；typed 基准 `03_len`/`08_range`/`11_min`/`13_sum`
  每轮耗时与快照持平。

## [0.108] - 2026-11-12

项目更名为 **PyAdamas**。

### 重构

- **命名**：可执行文件/CLI 从 `py23` 改为 `pyadamas`；C 符号前缀
  `py23_` → `pa_`；宏与环境变量 `PY23_*` → `PYADAMAS_*`；模块接口
  扩展名 `.py23i` → `.pya`；CMake 目标、测试名、Windows 包装脚本
  （`tools/win/pyadamas.*`）与文档统一改名。
- 仓库目录保持 `cpy23`；语言测试程序里的字符串数据（如
  `"hello, py23"`）保持不变，避免改变期望输出。

### 验证

- 全量重新配置/构建通过；`ctest --preset dev` 2/2（`pyadamas_suite` /
  `pyadamas_opt_levels`）；16MB `RLIMIT_AS` 验收 6/6；差分 runner
  280 例 0 失败；`94`/`95` 在 `-O0/-O1/-O2` 下输出一致。

## [0.107] - 2026-11-12

语言面收尾：自定义方法装饰器；yield/await 挂起点位置放宽。

### 新增

- **自定义方法装饰器**：`@deco def m(self, ...)` 在类创建点按从下到上
  的顺序应用装饰器，结果存进该类的类属性槽（与普通类属性共用存储/
  继承机制）；实例调用 `obj.m(...)` 加载这个函数值并把 `obj` 作为
  第一个实参传入，继承时沿 MRO 读到同一份包装函数。方法体仍按原签名
  检查，调用点按装饰器返回的函数类型查参。限制：装饰器必须返回已知
  签名的函数；不支持装饰 dunder 方法（vtable 静态分派）、`super()`
  调用和类接收者调用（`C.m(...)`），方法值本身不绑定 self。

### 修复

- **挂起点位置放宽**：`yield`/`await` 不再限于语句/赋值右值，只要它是
  当前语句里第一个求值的表达式即可，例如 `return (yield x)`、
  `if (await f()) > 0:`、`v = (await f()) + x`。生成器 resume 会重建
  C 栈帧，vreg 不跨挂起，所以挂起点之前的其它求值仍会被拒绝并给出
  明确诊断；`finally`/`except` 体内的挂起仍不支持。

### 验证

- 新增 `94_suspend_positions`（return 值/条件/二元表达式）与
  `95_method_decorator`（单层、链式、继承、访问 self 字段），输出与
  CPython 逐行一致。
- `ctest --preset dev` 2/2；16MB `RLIMIT_AS` 验收 6/6；差分 runner
  280 例 0 失败。

## [0.106] - 2026-11-12

生成器/协程异常逃出、close 清理抛异常时立即释放帧槽引用。

### 修复

- **gcrel 改为真实释放**：从「GC 清扫时只减子对象计数」改为逐槽
  `rc_dec` + 清零，并用「status==1 跳过 + 槽清零」保证幂等。GC 清扫
  期间 `pa_rc_dec` 自带「只调可达对象计数」的保护，语义与原来的
  `pa_gc_unref_child` 一致。
- **异常逃出释放帧槽**：`gen_run` / `coro_step` / `agen_drive` 的异常
  路径、`pa_gen_close` 的 finally 抛异常路径、`pa_gen_finalize`
  的清理失败路径都调用 `pa_gen_release_slots`。此前只把 `status`
  标成 2，只要生成器/协程对象还被引用着，帧里的容器/字符串就永远
  不回收：2000 个被保留的生成器（各带 20000 元素 list）峰值
  157MB → 2.3MB。
- `pa_gen_close` 补上 handler 与临时值水位：close 期间产生的临时值
  在边界统一清理，异常信息原样向外传。

### 验证

- 新增 `93_gen_exc_rc`：生成器 `next` 抛异常、`close` 的 finally 抛
  异常、协程 `await` 抛异常、异步生成器 `__anext__` 抛异常，各保留
  150–300 个对象，输出与 CPython 一致。
- 专项：四类保留对象场景峰值约 2–3MB；生成器帧参与引用环、
  20000 轮 GC 清扫峰值 63MB（有界，无崩溃）。
- `ctest --preset dev` 2/2；16MB `RLIMIT_AS` 验收 6/6；差分 runner
  280 例 0 失败。

## [0.105] - 2026-11-12

异常 longjmp 路径的引用计数清理：被跳过函数帧的局部堆值当场回收。

### 修复

- **普通函数帧记录**：非生成器函数在序言压一条
  `Py23Frame {prev; n; 槽地址×n}`（只登记持有引用的堆槽），正常返回前
  弹栈；`exc_longjmp` 在跳转前调 `pa_frame_unwind`，沿链把被异常跳过
  的帧里槽值先清空再 `rc_dec`。此前这些槽要等 GC，而 str 等叶子对象
  untracked、GC 不会回收：2000 轮「str + 20000 元素 list 后 raise」的
  非内联用例峰值 315MB → 2.9MB。
- **内联槽不再漏引用**：内联入口在复用堆槽前先释放旧值（形参槽在
  被实参覆盖前、`need_zero` 槽在清零前，覆盖了「异常在调用方自己的
  handler 里被捕获、不经过帧展开」的路径）；`release_slots` 释放后把
  槽清零（内联把槽留在调用方帧里跨多次执行复用，悬空指针会让下一次
  赋值二次释放）。堆槽一律在序言清零，且不再提升到 callee-saved
  寄存器（帧记录按栈槽地址读值）。
- **只读形参借用恢复**：出口清零曾让「函数体是否写过形参」误判，借用
  优化整体失效；判定时忽略这段清零序列，并在借用槽上把出口 `dec` 与
  随后的清零一起删掉，避免内联重映射后出现无落点的 store。
- handler 记录新增 `frame` 字段（压入/重压时的帧栈顶）；生成器/协程的
  槽在堆帧里，由帧销毁负责，不压记录。

### 验证

- `ctest --preset dev` 2/2 通过；`92_exc_inline_rc` 扩展为内联、递归
  非内联、只读形参三段，输出与 CPython 逐行一致（`-O0/-O1/-O2` 均验证）。
- 内联/非内联/str/list/参数变体在 2000 轮异常下峰值均 < 4MB；其中
  「内联体形参槽 + 调用方自己捕获」用例 2.5MB。
- `05_strings` / `10_rc` / `13_gc` / `14_dict` / `15_slice` / `54_stdlib`
  在 16MB `RLIMIT_AS` 下通过。

## [0.104] - 2026-11-12

字典/集合删除从 O(n) 改为摊还 O(1)：墓碑 + 插入顺序双向链表。

### 性能

- **删除不再 memmove / 整表重建索引**：存活项用 `prev`/`next` 串成插入
  顺序双向链表，删除只摘链，entry 原地变墓碑（`hash = 0`）挂进
  `free_head` 空闲链；索引槽改存 -1 墓碑，探测越过继续，插入时优先复用
  墓碑与墓碑槽。删除 20 万项的 dict 与 set、再做 10 万次「删一个插一个」
  的交替增删，整段程序 0.036s（旧实现单次删除 O(n)，同规模不可能跑完）。
- **遍历顺序不变**：按序号遍历走 `order` 快照（链表下标），删除后标记
  失效、下一次按序访问时重建；顺序遍历自身是 O(n)，因此不增加摊还代价。
  `popitem` 取链表尾，仍是最后一个插入项。

### 验证

- `ctest --preset dev` 2/2 通过；`16_dict_del`（含 popitem 顺序）恢复通过。
- 专项对比 CPython：50 万次增删、中间删除后重插、`popitem` LIFO、
  `set.pop`、墓碑复用、`copy`/`update`/`==`、`json.dumps(sort_keys=True)`、
  `defaultdict`、删除后的迭代顺序，输出逐行一致（`set.pop` 与 PyAdamas
  既有的确定性取尾语义除外）。
- `05_strings` / `10_rc` / `13_gc` / `14_dict` / `15_slice` / `54_stdlib`
  在 16MB `RLIMIT_AS` 下通过。

## [0.103] - 2026-11-12

字符串运行时深度优化：字符不再分配、搜索走 `memchr`、长键哈希缓存。

### 性能

- **单字符驻留**：`s[i]`、`for c in s`、`chr()` 返回 256 个静态永生单字节串
  （首次使用时初始化并登记 RC 索引），不再每取一个字符就新建 RC 对象；
  `"True"/"False"/"None"` 同样驻留。200k 次遍历 57 字节串：0.273s → 0.067s；
  下标取值：0.239s → 0.062s（同机 CPython 约 0.76s / 1.47s）。
- **`memchr` 搜索**：`find`/`count`/`replace`/`split(sep)`/`in` 统一改为
  「`memchr` 找首字节 + `memcmp` 校验」，替代逐位置整段比较。
  `find` 0.052s → 0.006s，`replace` 0.0027s → 0.0006s。
- **长键哈希缓存**：>7 字节字符串把 FNV-1a + mix64 结果缓存在 `hash` 字段；
  长串字面量的哈希在编译期算好、直接写进静态对象（`.data.rel.ro` 在
  RELRO 下只读，不能在运行期懒写）。100 万次长键字典查找 0.0045s，
  CPython 0.051s。
- **共享不可变串**：`s[:]`、无变化的 `strip()`、`''.join([x])` 直接返回原串
  （按 CPython 语义 inc 一次），省掉一次分配加拷贝；连续切片改为整段
  `memcpy`。

### 验证

- 字符串边界探针（strip/slice/join/replace/find/长键字典/字符迭代/格式化）
  与 CPython 输出逐行一致。
- `ctest --preset dev` 2/2 通过；`05_strings` / `10_rc` / `13_gc` / `14_dict`
  / `15_slice` / `54_stdlib` 在 16MB `RLIMIT_AS` 下通过。

## [0.102] - 2026-11-12

字符串单遍化与短串 SSO。

### 性能

- **单遍 `string.capwords`**：`stdlib/string.py` 的 `capwords` 改调内部
  intrinsic `__pa_capwords`，降级为 `pa_str_capwords`；按 ASCII 空白
  单遍扫描，词首大写、其余小写，直接写进一个预分配的输出串，不再建中间
  词表。实测 20 万次（44 字节 / 9 词）：0.108s → 约 0.011s，同机
  CPython 3.13.5 约 0.16–0.19s。
- **短串 SSO**：<=7 字节的字符串把字符内联在 `Py23Str::hash` 的 8 字节里
  （留 1 字节 NUL），`data` 指向这段内联区，省掉结构体后的字符区；长串
  布局不变。值仍是 8 字节裸指针、对象仍走 RC/GC，属于 ABI 兼容的内联，
  不是 tagged-value 级 SSO。

### 验证

- `ctest --preset dev` 2/2 通过。
- `10_rc` / `13_gc` / `14_dict` / `15_slice` / `05_strings` / `54_stdlib`
  在 16MB `RLIMIT_AS` 下全部通过。
- `capwords` 语义探针输出与 CPython 一致。

## [0.101] - 2026-11-12

字符串 / 列表分配继续优化。

### 性能

- **列表 freelist**：`Py23List` 结构体单独一档（最多 64 块）；`data` 缓冲
  <=512 字节按 64 字节分 8 档，每档最多 64 块。复用时容量按档对齐。
- **`split(sep)` 单遍填充**：单字节分隔符走 `memchr`，容量从 8 起，常见
  2–6 段不再 realloc。
- **`sm_push_str` 快路径**：容量足够时直接写槽，避开跨 TU 的
  `pa_list_append` 调用。
- 实测 20 万次（44 字节 / 9 词）：`capwords` 0.116s → 0.108s，同机
  CPython 3.13.5 约 0.15–0.19s；`split(sep)` 约 0.022s（CPython 约 0.024s）；
  `split + join` 约 0.048s（CPython 约 0.064s）。

### 验证

- `ctest --preset dev` 2/2 通过。
- `10_rc` / `13_gc` / `14_dict` / `15_slice` / `05_strings` 在 16MB
  `RLIMIT_AS` 下全部通过；50 万次 split 在 16MB 下通过。
- dev / release 预设生成的基准程序运行时间无实质差异：生成代码走
  `PyAdamas -O2`，`rt.o` 固定由 `cc -O2` 编译，与编译器自身的 dev/release
  预设无关。

## [0.100] - 2026-11-12

字符串分配优化与 `split` 泄漏修复。

### 修复

- **`str.split()` / `splitlines()` 分词泄漏**：每个新词被额外 `rc_inc` 一次，
  列表销毁后仍剩一个引用，分词结果永不释放（50 万次 `split` 在 32MB 地址
  空间下 OOM）。改为把 `str_new` 的初始引用直接转交给列表。

### 性能

- **短字符串 freelist**：<=128 字节的字符串按 8 字节分 16 档，每档最多缓存
  64 块；RC 归零时回收进池，下一次同档分配直接复用，不再每次走 malloc/free。
- **`split()` 预分配**：按空白切分前先数词数，用精确容量建表，省掉 append
  过程中的 realloc。
- 实测 20 万次 `string.capwords`（44 字节 / 9 个词）：0.219s → 0.116s
  （约 1.9×），同机 CPython 3.13.5 为 0.156s；`s.split()` 0.124s → 0.041s，
  同机 CPython 为 0.044s。

### 验证

- `ctest --preset dev` 2/2 通过。
- `10_rc` / `13_gc` / `14_dict` / `15_slice` / `05_strings` 在 16MB
  `RLIMIT_AS` 下全部通过；50 万次 split 在 32MB 下通过。

## 更早版本

0.41–0.99 的记录已归档到 [docs/archive/changelog-0.41-0.99.md](docs/archive/changelog-0.41-0.99.md)；
0.40 及更早见 [docs/archive/changelog-0.40.md](docs/archive/changelog-0.40.md)。
