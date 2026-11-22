// codegen_frame.h —— 栈槽分配 + 帧记录（类体片段）
//
// **本文件是 class AsmGen 的类体片段**：由 codegen_impl.h 在 class 体内
// #include，内容就是 AsmGen 的成员声明，不单独编译、没有 include guard。
// 不要在文件里写 namespace / 文件级 #include —— 那会出现在类体中间，编译失败。
//
// 从 codegen_impl.h 按原有分组注释整段切出（纯文本移动，无逻辑改动）。
// 另有两段没有分组注释、按内容属于栈帧的：帧布局/尾声
// （out_bytes_…emit_epilogue）和帧记录（GC 扫描 / 异常展开，frame_owned_…
// build_frame_scan_info）。它们原本夹在其它分组之间，一并收在本文件。
// 实现见 codegen_regalloc.cpp（assign_vreg_slots）与 codegen_block.cpp。

    // vreg 槽区里第 slot 个槽的偏移（vreg 槽紧挨着命名槽往上排）。
    int vreg_slot_off(int slot) const { return out_bytes_ + 8 * (n_slots_ + slot); }
    // 帧记录里的槽：持有堆引用的非借用槽，异常展开时按地址 dec。
    std::vector<int> frame_owned_;
    // 与 frame_owned_ 一一对应：槽里可能是非堆值，展开时走注册表释放。
    // 记录里用地址最低位置 1 标记（槽地址 8 字节对齐）。
    std::vector<bool> frame_owned_unknown_;
    // 借来的 unknown 槽：只登记给 GC 扫描，异常展开不释放。
    std::vector<int> frame_scan_;
    // 按槽号索引的「这个槽要给 GC 扫描」布尔表。
    // 融合/别名优化据此判断能否绕过栈槽 —— 被扫描的槽必须让 smem(s)
    // 保持权威副本，否则 GC 从帧记录读到旧值/0。
    std::vector<char> gc_scan_slot_;
    // 需要保守扫描的临时区字数（0 表示这个帧的临时区一个字节都不扫）。
    int frame_scan_words_ = 0;
    // 临时区位图：bit i = 1 表示第 i 个字可能需 GC 保守扫描。空表示没有
    // 位图（运行时按整段扫）。位图必须是超集，宁多勿少。
    std::vector<uint64_t> frame_scan_mask_;
    // 按 vreg 静态类型算出临时区的保守扫描范围 / 位图（见 codegen_block.cpp）。
    void build_frame_scan_info(const IRFunction& f);
    // 帧布局 = [出参区 out_bytes_][局部区 locals_][push 的 callee-saved][返回地址]。
    // rsp 在函数体里全程不动，槽位因此可以相对 rsp 寻址，帧里不需要 rbp。
    // SysV 要求 call 时 rsp 是 16 的倍数，布局阶段把 padding 补到最小。
    int out_bytes_ = 0;    // 出参区：整函数最大的那个调用点用量
    int locals_ = 0;       // 局部区（含对齐 padding）
    int frame_pad_ = 0;    // subq 的字节数 = out_bytes_ + locals_
    int frame_bytes_ = 0;  // 返回地址到 rsp 的距离 = frame_pad_ + 8 * n_saved_regs_
    int cfa_off_ = 0;      // 当前 .cfi_def_cfa_offset（CFA = rsp + cfa_off_）
    // 扫描全部调用点，算出整函数要预留多少出参区。
    int outgoing_bytes(const IRFunction& f) const;
    // 计算 out_bytes_ 之外的几个布局量；必须在 build_operand_caches 之前
    // 调用——槽位操作数是在那时格式化好的。
    void layout_frame(int total_slots);
    // 发 subq 并把 CFA 偏移推到函数体状态。
    void frame_alloc();

    // 尾声：先退局部区，再逐个弹回 callee-saved，然后 ret。
    void emit_epilogue();

    // ── 栈槽分配 ────────────────────────────────────────────────
    //
    // 线性扫描：给每个 vreg 算一个 [定义, 最后使用] 的区间，区间不重叠
    // 的可以共用一个栈槽。不做寄存器分配，所以不存在「值在寄存器里」
    // 的状态，规则非常简单；但槽数会从「vreg 总数」降到「最大同时活跃数」，
    // 对长函数是几十倍的差距。
    //
    // 关键前提：**只有「定义和使用都在同一基本块内」的值才能按线性区间
    // 复用**。跨块的值（尤其是循环头里被反复读取的）在静态上「已经用完」
    // 之后仍会在下一轮迭代被读，线性区间会把它的槽分给别人，然后循环
    // 第二次迭代就炸。这类值一律独占一个槽，区间拉满整个函数。
    void assign_vreg_slots();

    // 变量提升（旧路径，PYADAMAS_NO_RA=1 回退用）：按被读写次数排序，把
    // 最热的几个整型槽放进 callee-saved；浮点槽按 xmm8–15 处理。
    // 默认路径是下面的活跃区间的线性扫描（assign_slot_regs_linear /
    // linear_scan_pool），这里保留旧实现只为可回退对比。
    void assign_slot_regs();
    // 精确区间诊断（PYADAMAS_LI_STATS）：对比精确区间 vs 粗近似。
    void report_interval_precision();
    void assign_slot_regs_linear();
    // live range splitting：按块给变量槽分配寄存器。
    void assign_slot_regs_split();
    // 单个池的着色：cands 是槽号列表（同属一个池），结果写进 out[槽号]。
    void color_slot_pool(const std::vector<int>& cands, const char* const* regs,
                         int nreg, std::vector<std::string>& out);
    // 通用线性扫描：给定活跃区间与寄存器池，返回 slot -> 寄存器名。
    // 整型与浮点共用（R4：浮点也走线性扫描，不再按频次挑前 N 个）。
    void linear_scan_pool(const std::vector<int>& cands, const char* const* regs,
                          int nreg, std::vector<std::string>& out);
    bool slot_range_has_call(int64_t beg, int64_t end) const;

    // 跨块 vreg 提升：槽没占满的 callee-saved 分给跨块整型临时值，
    // 整函数独占；callee-saved 跨调用安全（CFG 活跃性见函数体注释）。
    void assign_global_vreg_regs();

    std::vector<char> ra_opt_readonly_alias();
    std::vector<std::pair<int, int>> ra_opt_call_block_cands(const std::vector<int64_t>& weight);

