// backend/vect_gates.h —— 通用向量化器共享判据的声明（类体片段）
//
// 由 backend/core.h 在类内 #include，不单独编译。实现见 backend/vect_gates.cpp。
// 0.480 起本文件只保留 VectPlanner 复用的纯 IR 形状判据；特化 SIMD 模板与其
// 开关（PYADAMAS_SIMD）整段删除，见 CHANGELOG 0.480。

    // vreg 的定义指令（def_block_ 口径，与定义位置无关）。
    const Inst* simd_def(VReg v) const override;
    bool simd_const_int(VReg v, int64_t want) const;
    // 体块过滤常量定义后恰为 core_n 条核心指令（总数 <= max_total），末尾 Jump。
    bool simd_core_body(const Block& b, size_t core_n, size_t max_total,
                        std::vector<Inst>& core) const;
    // incr/cond 块形状（步长 1 的可数 for）；返回 cond/end 块号与上界 n。
    bool simd_incr_cond_ok(int incr_bi, int body_bi, int sJ,
                           int& cond_bi, int& end_bi, VReg& n) const;
    // while 形状（cond/body 两块，体块尾部是增量）。
    bool simd_while_shape_ok(int cond_bi, int body_bi, int& sJ,
                             int& end_bi, VReg& n, int& jupd_pos,
                             int& step_slot) const;
    // 循环三块 op 白名单（无调用/全局写；allow_sqrt 放行 CallRt，
    // allow_idxset 放行 IndexStore——具体位置/个数由体块形状匹配钉死）。
    bool simd_loop_ops_ok(int cond_bi, int body_bi, int incr_bi,
                          bool allow_sqrt, bool allow_idxset) const;
    int simd_slot_writers(int slot, int cond_bi, int body_bi, int incr_bi) const;
    bool simd_slot_unaddressed(int slot) const;
    bool simd_outside(VReg v, int cond_bi, int body_bi, int incr_bi) const;
    bool simd_slot_stable(VReg v, int cond_bi, int body_bi, int incr_bi) const;
    // 输入操作数不能落在向量体要 clobber 的 scratch（rax/rcx/rdx/rsi/rdi/xmm0-7）。
    static bool simd_scratch_safe(const MOperand& o);
