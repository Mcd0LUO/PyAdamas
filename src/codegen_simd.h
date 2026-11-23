// codegen_simd.h —— SIMD 向量化成员与声明（类体片段）
//
// 从 codegen_impl.h 拆出（文件规模闸门 1200 行）。本文件是 class AsmGen
// 的类体片段：由 codegen_impl.h 在类内 #include，不单独编译。
// 实现见 codegen_simd.cpp，设计见 docs/simd-vectorization-design.md。
// 开关 set_simd 留在 codegen_impl.h 的 public 段（generate_asm 外部调用）。

    // 浮点循环 SIMD 向量化（PYADAMAS_SIMD，默认关）。开启后 gen_block
    // 对匹配到的 reduction 热点循环发 SSE2 packed 向量体 + 标量尾循环。
    bool simd_enabled_ = false;

    // SIMD 向量化（codegen_simd.cpp，PYADAMAS_SIMD 门控）：识别 reduction
    // 热点循环体块，匹配成功则发「守卫 + SSE2 向量循环 + 跳标量尾」前奏，
    // 返回 true（随后 gen_block 照常发射原标量指令作为尾循环/fallback）。
    // 不匹配返回 false，一个字节不发。
    bool try_simd_reduction(const Block& b);

    // ── 0.334 扩展：nbody（浮点 map）+ matmul（整数 map）──────────────
    // gen_block 统一入口：按模式分发（reduction / int map / nbody pos /
    // nbody pair）。门控关时第一个判断就返回，一个字节不发。
    bool try_simd_vectorize(const Block& b);
    // matmul 整数内循环 c[base+j] += aik*b[base2+j]（codegen_simd_int.cpp）。
    bool try_simd_int_map(const Block& b);
    // nbody 位置更新 b[i7+k] += dt*b[i7+k+3]（codegen_simd_nbody.cpp）。
    bool try_simd_nbody_pos(const Block& b);
    // nbody pairwise 力循环（codegen_simd_nbody_pair.cpp）。
    bool try_simd_nbody_pair(const Block& b);

    // ── 共享判据（各模式复用，实现见 codegen_simd.cpp）────────────────
    // 函数级前提：门控开 + opt>=2 + 非生成器 + 默认分配 + SysV + 无 handler。
    bool simd_func_ok() const;
    // vreg 的定义指令（def_block_ 口径，与定义位置无关）。
    const Inst* simd_def(VReg v) const override;
    bool simd_const_int(VReg v, int64_t want) const;
    // 体块过滤常量定义后恰为 core_n 条核心指令（总数 <= max_total），末尾 Jump。
    bool simd_core_body(const Block& b, size_t core_n, size_t max_total,
                        std::vector<Inst>& core) const;
    // incr/cond 块形状（步长 1 的可数 for）；返回 cond/end 块号与上界 n。
    bool simd_incr_cond_ok(int incr_bi, int body_bi, int sJ,
                           int& cond_bi, int& end_bi, VReg& n) const;
    // while 形状（cond/body 两块，体块尾部是增量）：手写的
    // `while i < n: ...; i += 1` 降成这样，补一条与 for 等价的路径。
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
