// backend/vect.h —— 通用循环自动向量化成员与声明（类体片段）
//
// 从 backend/impl.h 在类内 #include，不单独编译。实现见 backend/vect.cpp，
// 设计见 docs/simd-vectorization-design.md §11。与特化 matcher（codegen_simd*）
// 互补：特化认基准精确形状，本文件做**通用**自动向量化——基于 IR 结构 +
// 数据引用/别名分析识别任意可数 for 循环的逐元素 map / reduction，不依赖
// 精确指令形状。门控 PYADAMAS_VECT，**默认关**。
//
// 正确性命门：别名/依赖证不出 → 保守跳过（标量 fallback）；浮点 reduction
// 保序提取累加（与标量逐位一致）；整数 reduction 用 paddq 2-lane（模 2^64
// 结合律精确）+ 出口水平求和。向量循环零调用，守卫 + 标量尾保留全部异常语义。

    // 通用循环向量化开关（PYADAMAS_VECT，默认关）。
    bool vect_enabled_ = false;

    // 通用循环向量化入口（backend/vect.cpp）：gen_block 在特化 matcher 未命中
    // 后调用。识别可数 for + 逐元素 map/reduction，匹配成功发「守卫 + SSE2
    // 向量循环 + jmp cond（标量尾由原机器照常发射）」，返回 true。不匹配返回
    // false，一个字节不发。门控关时第一个判断就返回。
    bool try_vect_loop(const Block& b);
    // masked divergence（外层独立迭代 + 内层数据依赖早退），见 backend/masked.cpp。
    bool try_masked_outer_loop(const Block& b);

    // 体块是否「全整数」（P4 顺序分流用，实现见 backend/vect.cpp）：只看 IR
    // 类型（int/bool/定宽整数、以及元素为整数的 list），不出现任何浮点值。
    // 保守口径：类型缺失或任何拿不准的值都按「含浮点」返回 false —— 宁可让
    // 浮点体块走特化优先（现状），也不误改浮点路径。
    bool block_int_only(const Block& b) const;
    // 函数级前提（与 simd_func_ok 同口径，但用 vect_enabled_ 门控）。
    bool vect_func_ok() const;

    // xmm8-15 里**没有**被本函数「浮点槽 / 跨块 vreg 提升」占用的寄存器。
    // 向量循环可以把它们当临时值：循环体内没有任何调用，循环外的标量代码也
    // 不读写这些寄存器（RA 整个函数都没把它们分配给活跃值）。判据只看当前
    // 函数的提升状态，与基准无关。见 opt/vect_planner_loop.cpp 的池扩展。
    bool xmm_high_free(MReg r) const;

    // ── P1 仿射下标（docs/vectorization-generalization.md §P1）──────────
    // 下标模型从「j + 常量」扩到 coef*j + inv + off：coef>0 编译期常量、
    // inv 循环不变 int 值（可为 kNoReg）、off>=0 常量。实现见
    // backend/vect_affine.cpp。判据只看 IR 结构（j-alias 槽 / Add / Mul 常量），
    // 不含任何基准名或字面量 —— 0.384 的硬政策：不许针对基准特化。
    // 仿射下标相关的类型已移到 VectPlanner（src/opt/vect_planner.h），
    // 这里只留别名，类内写法不变。
    using AffIdx = ::pyadamas::AffIdx;
    using AffBase = ::pyadamas::AffBase;
    using AffEq = ::pyadamas::AffEq;
    static constexpr int64_t kAffMaxCoef = ::pyadamas::kAffMaxCoef;
    static constexpr int64_t kAffMaxOff = ::pyadamas::kAffMaxOff;

    // ── P3 · AVX2 4-lane（docs/vectorization-generalization.md §P3）────────
    // 只做**整数**路径：int map（out[j]=a[j]*2+1）与 int reduction
    // （s+=a[j]*b[j]）发 4 lane ymm。浮点路径**不动**（保持 2-lane）：保序
    // 累加（逐位一致）决定了它吃不到 4-lane 的好处，见文档 §P3 与 §5。
    //
    // 运行时派发：4-lane 前奏读运行时写好的 CPU 能力字节 pa_cpu_avx2
    // （runtime/rt_list.c 的构造函数，在 main 之前就绪）。非 0 → 4-lane 循环，
    // 0 → 现有的 SSE2 2-lane 循环。非 AVX2 机器上一条 VEX 指令都不会执行
    // （所以不会 SIGILL）；能力字节初值 0 是保守值。编译期 PYADAMAS_AVX2=0
    // 只发 SSE2 路径，便于对拍两条路径的一致性。
    //
    // 角色/访问/累加器描述已随阶段 A4 第 1 步移到 VectPlanner::Loop
    // （src/opt/vect_planner.h），这里只留别名；让 backend/vect_avx2.cpp 读
    // **同一份**分析结果：4-lane 不重新分类、不猜，判据只有一处。
    using VRole = ::pyadamas::VRole;
    // 向量值分类（try_vect_loop 的分析结果）。原本内嵌在类体里，阶段 3
    // 第 6 步随同值常量归并方法移到 VectPlanner（src/opt/vect_planner.h）；
    // 这里只留别名，类体内的 VClass / VK::X 写法保持不变。
    // 分类枚举与判据的唯一出处在 opt/vect_rules.h（具名 + 编译期真值表）。
    using VK = vect::VK;
    using VClass = ::pyadamas::VClass;
    // 一条下标访问的仿射描述（coef*j + inv + off）。定义见 opt/vect_planner.h。
    using VAccess = ::pyadamas::VAccess;
    // 一个 reduction 累加器（int 走 4-lane 时 IACC 当 ymm 用）。定义见 opt/vect_planner.h。
    using VRed = ::pyadamas::VRed;
    // 4-lane 发射上下文：全部是 try_vect_loop 已定死的分析/分配结果（只读）。
    // 定义见 opt/vect_planner.h。
    using VAvx2Ctx = ::pyadamas::VAvx2Ctx;
    // 发「运行时派发 + 4-lane 循环 + 折回 2 lane」。不满足 4-lane 前提时
    // 返回 false 且**一个字节都不发**（SSE2 路径照旧）。成功时 *vexit 收回
    // 两条循环的公共出口标签（由调用方在 SSE2 循环之后 label 出来）。
    bool vect_avx2_emit(const VAvx2Ctx& ctx, std::string* vexit);

    // 仿射下标解析（递归；depth 仅防病态深链）。成功 = 可折进寻址的仿射式。
    AffIdx vect_affine_index(VReg v, const std::vector<int>& jalias,
                             const std::function<bool(VReg)>& inv_ok,
                             int depth = 0) const {
        return vect_.vect_affine_index(v, jalias, inv_ok, depth);
    }
    // 规划期校验（发射前）：常量编不进 imm32/disp32 → 整段保守跳过。
    bool vect_affine_plan_ok(const std::vector<AffBase>& bases) const {
        return vect_.vect_affine_plan_ok(bases);
    }
    // 边界守卫：0 <= coef*(n-1)+inv+off < len；每步算术 jo 溢出 → 跳 tail。
    bool vect_affine_guard(const MOperand& list_obj, const AffBase& b,
                          const MOperand& n_op, const std::string& tail) {
        return vect_.vect_affine_guard(list_obj, b, n_op, tail);
    }
    // base = data(list_obj)（+ inv*8，若 inv != kNoReg）。
    void vect_affine_base(const MOperand& list_obj, const AffBase& b, MReg reg) {
        vect_.vect_affine_base(list_obj, b, reg);
    }
    // inv 判等守卫：同对象读写只在两个下标序列完全相同时才能就地向量化。
    void vect_affine_inv_eq(const AffEq& eq, const std::string& tail) {
        vect_.vect_affine_inv_eq(eq, tail);
    }
    // coef>1 的运行期跨步计数器：K = coef*8*j，每轮 += coef*16。
    void vect_affine_kinit(MReg k, MReg jreg, int64_t coef) { vect_.vect_affine_kinit(k, jreg, coef); }
    void vect_affine_step(MReg k, int64_t coef) { vect_.vect_affine_step(k, coef); }
    // 2-lane 装载：coef==1 用 movupd；coef>1 两次 movsd + unpcklpd（无 gather）。
    void vect_affine_load(MReg base, MReg j, MReg k, int64_t coef, int64_t off,
                          MReg dst, MReg scratch) {
        vect_.vect_affine_load(base, j, k, coef, off, dst, scratch);
    }
    // 2-lane 存储：只支持 coef==1（不做 scatter）。
    void vect_affine_store(MReg base, MReg j, int64_t off, MReg src) { vect_.vect_affine_store(base, j, off, src); }

    // ── P5 · 整数 // 与 % 的 2 的幂除数（实现见 codegen_vect_intdiv.cpp）──
    // 政策：**通用操作支持**（LLVM InstCombine 把 sdiv x, 2^k 降成移位 + 修正，
    // GCC tree-vect 同样处理），不是针对某个程序的模式匹配。判据只有一条：
    // 除数是编译期正的 2 的幂常量。除数是 2 的幂但不是常量、或任何一边不是
    // int → 分类器维持 Fail（整段标量 fallback）。
    //
    // 可向量化的闭式（与标量逐位一致，任意 int64 含 INT64_MIN）：
    //   x // 2^m → 算术右移（SSE2/AVX2 无 64 位算术右移，按 2 的补码合成）
    //   x %  2^m → 低 m 位（andq $(2^m-1) 的 packed 版）
    // 除数 vreg → 移位量 m（0 <= m <= 62；m == 0 是恒等/清零）；不是 2 的幂
    // 常量返回 false。
    bool vect_div_pow2(VReg b, int* sh) const { return vect_.vect_div_pow2(b, sh); }
    // 向量化器协作对象（阶段 3 起逐方法搬入）。
    VectPlanner vect_{*this, *this, *this, *this};
    // 2-lane 发射：rd 已持有被除数 x（2 lane i64），结果原地写回 rd。tmp 是
    // scratch（seq 寄存器）。% 不用 scratch；// 的 m>0 用 4 条 packed 合成。
    void vect_intdiv2(MReg rd, MReg tmp, bool is_mod, int sh) {
        vect_.vect_intdiv2(rd, tmp, is_mod, sh);
    }

    // ── P5b · 归纳变量 j 作为整数算术操作数（实现见 backend/vect_jvec.cpp）──
    // 判据只有一条结构性质：操作数是 j-alias 槽的装载（归纳变量本身或其
    // 副本槽）。j 在整型 Bin 里当 VecI 用（Add/Sub/Mul/除以 2 的幂常量），
    // 每轮迭代在首个使用点物化一次 [j, j+1]（4-lane 由 AVX2 段物化
    // [j..j+3]）。序列与 Convert 的 j_src 分支同一份，逐 lane 模 2^64 与
    // 标量 addq/subq/imulq 逐位一致，任意 i64 无需值域守卫。
    void vect_jvec2(MReg jreg, int64_t off, MReg dst, MReg scr) {
        vect_.vect_jvec2(jreg, off, dst, scr);
    }
    // 使用点分类：除 IndexLoad/IndexStore 的下标操作数外，任何用途都记进
    // out。折叠进寻址的仿射式必须**只**当下标用 —— j 参与算术后，同一个值
    // 可能既当下标又当算术语，折叠会让发射期取不到寄存器（半截 asm）。
    void vect_mark_nonindex_uses(std::unordered_map<VReg, char>* out) const;
    // 同值常量归并：按值把重复的 InvI/InvF 常量 vreg 接到首个 vreg 的
    // mov_src 上（广播寄存器只占一个）。语义等价，只为省寄存器。
    void vect_merge_const_invs(std::unordered_map<VReg, VClass>* cls) {
        vect_.vect_merge_const_invs(cls);
    }
    // 归纳变量的算术使用点区间：role==Bin 且操作数分类是 J 的首/末条。
    // 没有这样的使用点 → *first == -1。
    void vect_j_span(const std::vector<Inst>& body, const std::vector<VRole>& role,
                     const std::unordered_map<VReg, VClass>& cls,
                     int* first, int* last) const;
    // 临时值线性扫描（发射前 dry-run）：按体块顺序给 Load/Bin 的 dst 分配
    // xmm，操作数末次使用后还池。目标可以接管左操作数的寄存器；归纳变量的
    // 物化寄存器按 vect_j_span 的区间分配/释放。放不下 → false（一个字节不发）。
    struct VTempPlan {
        const std::vector<Inst>* body = nullptr;
        const std::vector<VRole>* role = nullptr;
        const std::unordered_map<VReg, int>* last_use = nullptr;
        const std::unordered_map<VReg, VClass>* cls = nullptr;
        std::vector<MReg>* pool = nullptr;              // 可用 xmm 池（原地消耗）
        std::unordered_map<VReg, MReg>* tmp_reg = nullptr;   // 分配结果
        MReg jvec = kMRegNone;                          // 归纳变量物化寄存器
        int j_first = -1;
        int j_last = -1;
        // 重复地址原地累加写：load/expr 的临时寄存器必须活到 store 位置。
        const std::vector<VAccStore>* acc_stores = nullptr;
    };
    bool vect_alloc_temps(VTempPlan* p);
