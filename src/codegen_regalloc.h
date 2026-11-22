// codegen_regalloc.h —— 全局寄存器分配 + live range splitting（类体片段）
//
// **本文件是 class AsmGen 的类体片段**：由 codegen_impl.h 在 class 体内
// #include，内容就是 AsmGen 的成员声明，不单独编译、没有 include guard。
// 不要在文件里写 namespace / 文件级 #include —— 那会出现在类体中间，编译失败。
//
// 从 codegen_impl.h 按原有分组注释整段切出（纯文本移动，无逻辑改动）。
// 实现见 codegen_regalloc.cpp / codegen_regalloc_scan.cpp /
// codegen_regalloc_temp.cpp / ra_opt.cpp。

    // ── 全局寄存器分配（R1：整型槽的块级活跃性）──────────────────
    // slot_live_in_[b] / slot_live_out_[b] 是第 b 块入口/出口仍活跃的
    // 整型槽集合（按槽号直接索引的位图，每槽一位）。R2 的线性扫描
    // 分配器靠它算活跃区间。
    //
    // 只跟踪**可提升**的槽：堆槽（帧记录按地址读值）、取过地址的槽
    // （SlotAddr）、handler hazard 槽都不参与 —— 与 assign_slot_regs
    // 的排除条件保持一致，否则会分到一个永远不能用的寄存器。
    std::vector<std::vector<uint64_t>> slot_live_in_;
    std::vector<std::vector<uint64_t>> slot_live_out_;
    std::vector<int> slot_reg_cand_;   // 参与分配的槽号（升序）
    std::vector<char> slot_cand_is_fp_; // 与 slot_reg_cand_ 对齐：是否浮点槽
    std::vector<int> slot_reg_cand_idx_;  // 槽号 -> slot_reg_cand_ 下标，-1 = 不参与
    void build_slot_liveness();
    std::vector<char> fp_spill_mask_;  // 当前指令需要保存的槽

    bool fp_mask_valid_ = false;
    bool fp_live_off_ = false;  // PYADAMAS_NO_FPLIVE：退回全量 spill（A/B 用）
    void build_fp_liveness();
    void spill_fp_slots();
    void reload_fp_slots();
    // 生成器/协程 resume：槽位在堆帧里，%r15 固定指向帧负载；
    // 帧头 10 个字：state/status/sent/out/resume/closing/kind/waiter/exc/cur。
    bool gen_ = false;
    static constexpr int kGenHeader = 10;

    // vreg → 它当前所在的物理位置（"%r13" 这种）。只在块内有效：
    // 块边界一律清空，所以跨块的值永远走栈槽，不会读到过期的寄存器。
    std::unordered_map<VReg, MOperand> alias_;

    // 能被写成立即数的常量。同样只在块内有效（保存 "$imm" 串）。
    std::unordered_map<VReg, MOperand> const_val_;

    // 整函数范围的 const.int → "$imm"。跨块的常量不必占寄存器或栈槽，
    // 所有使用点直接用立即数；定义指令在 gen_block 里跳过。
    std::unordered_map<VReg, MOperand> global_imm_;

    // 本指令的 dst 该写到哪里。为空表示按 mem(dst)；非空是融合目标
    // （通常是一个变量寄存器），此时 mem(dst) 完全不参与。
    MOperand dst_loc_;

    std::vector<int> use_count_;
    std::vector<bool> cross_block_;

    // 调用的 dst 没人读：不必把返回值搬回栈槽（表达式语句里的调用）。
    bool dst_unused(const Inst& in) const;
    std::vector<int> def_block_;

    // 块内临时寄存器：整块没有会调用/占用 %r8–%r11 的指令时，块内定义、
    // 块内用完的临时值直接住在寄存器里，mem(v) 返回它。块边界清空。
    std::vector<MOperand> vreg_reg_;
    // 块内临时寄存器可用池：无调用函数里被跨块提升占走的 %r8–%r11 会剔除。
    std::vector<std::string> block_temp_pool_;
    bool temp_reg_op_ok(const Inst& in) const;
    // xmm3-7 浮点临时值的安全性（定义在 codegen_ops.cpp）：与 GPR 池
    // 独立 —— 整数 div/mod/floordiv 只用 GPR scratch，不碰 xmm3-7。
    bool fp_temp_op_ok(const Inst& in) const;
    void assign_block_temp_regs();

    // 生成期缓存：块标签、vreg/槽的内存操作数串。这些字符串在函数内
    // 固定不变，生成每条指令时反复 format 会占掉相当一部分编译时间。
    std::vector<std::string> block_labels_;
    // 操作数缓存是结构化 MOperand 单元：build_operand_caches 赋值时解析
    // 一次，发射点直取免回解析；.text 仍是 canonical 操作数字符串。
    std::vector<MOperand> vreg_mem_;
    std::vector<MOperand> smem_cache_;
    std::vector<MOperand> gen_smem_cache_;
    std::vector<std::string> global_labels_;
    // rip 相对全局操作数按模块缓存：长 mangled 符号（pyadamas$...）每次
    // 访问都建 canonical text 会堆分配，缓存后全局访问零分配。
    std::vector<MOperand> global_mops_;
    std::vector<MOperand> global_tag_mops_;
    std::vector<std::string> func_labels_;
    void build_operand_caches();

    // ── live range splitting ───────────────────────────────────
    //
    // `slot_loc_[b][s]` 非空表示「槽 s 在块 b 里住在那个寄存器」。
    // 与 `slot_reg_[s]`（整函数固定位置）互斥：split 模式用这张表，
    // 默认模式用 slot_reg_。两者都为空时走栈（smem）。
    //
    // slot_op 里加了一个分支：slot_loc_ 非空时按块查表，否则走 slot_reg_
    // （默认路径行为完全不变）。split 模式只在 PYADAMAS_RA_SPLIT=1 时启用。
    std::vector<std::vector<MOperand>> slot_loc_;
    // 块 b 入口需要从栈装进寄存器的槽（前驱可能在栈上）。
    std::vector<std::vector<int>> slot_reload_in_;
    // 块 b 出口要写回栈的槽（存在后继要从栈读）。
    // split 模式不再双写 store，这里是栈副本唯一的更新点。
    std::vector<std::vector<int>> slot_spill_out_;
    // 当前生成到哪个块（split 模式用）。
    int split_block_ = -1;
