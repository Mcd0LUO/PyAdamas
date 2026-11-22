// codegen_vect.h —— 通用循环自动向量化成员与声明（类体片段）
//
// 从 codegen_impl.h 在类内 #include，不单独编译。实现见 codegen_vect.cpp，
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

    // 通用循环向量化入口（codegen_vect.cpp）：gen_block 在特化 matcher 未命中
    // 后调用。识别可数 for + 逐元素 map/reduction，匹配成功发「守卫 + SSE2
    // 向量循环 + jmp cond（标量尾由原机器照常发射）」，返回 true。不匹配返回
    // false，一个字节不发。门控关时第一个判断就返回。
    bool try_vect_loop(const Block& b);
    // 函数级前提（与 simd_func_ok 同口径，但用 vect_enabled_ 门控）。
    bool vect_func_ok() const;

    // ── P1 仿射下标（docs/vectorization-generalization.md §P1）──────────
    // 下标模型从「j + 常量」扩到 coef*j + inv + off：coef>0 编译期常量、
    // inv 循环不变 int 值（可为 kNoReg）、off>=0 常量。实现见
    // codegen_vect_affine.cpp。判据只看 IR 结构（j-alias 槽 / Add / Mul 常量），
    // 不含任何基准名或字面量 —— 0.384 的硬政策：不许针对基准特化。
    struct AffIdx {
        bool ok = false;
        int64_t coef = 1;      // j 的系数（> 0）
        VReg inv = kNoReg;     // 循环不变 int 值（折进基址）
        int64_t off = 0;       // 常量偏移（>= 0）
    };
    // coef/off 的编译期上界：保证 imulq/addq 立即数与 disp32 编码安全。
    static constexpr int64_t kAffMaxCoef = int64_t{1} << 24;
    static constexpr int64_t kAffMaxOff = int64_t{1} << 24;

    // 一个数组基址键：同一逻辑数组 + 同一 inv 的访问共用一条「data + inv*8」
    // 基址寄存器；coef>1 的访问另需运行期跨步计数器（按 coef 去重）。
    struct AffBase {
        std::string key;          // 逻辑数组身份（list_key）
        VReg inv = kNoReg;        // kNoReg → 基址就是 data
        MOperand inv_src;         // inv 在循环入口的可读来源（list_base_mop 口径）
        MReg reg = kMRegNone;     // 基址寄存器
        int64_t coef = 1;         // 该键下访问的最大 coef
        int64_t off_max = 0;
        int64_t off_min = 0;
    };
    // 同对象读写的 inv 判等守卫：x/y 是 inv 在入口的可读来源；kNoReg 侧
    // 等价于「与 0 比较」（无 inv 的下标就是 j+off）。
    struct AffEq {
        VReg x = kNoReg;
        VReg y = kNoReg;
        MOperand xm;
        MOperand ym;
    };

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
    // 角色/访问/累加器描述定义在类体里（而不是 try_vect_loop 的局部结构），
    // 让 codegen_vect_avx2.cpp 读**同一份**分析结果：4-lane 不重新分类、
    // 不猜，判据只有一处。
    enum class VRole : uint8_t { Skip, Load, Bin, MapStore, RedStore, Fail };
    // 一条下标访问的仿射描述（coef*j + inv + off）。
    struct VAccess {
        VReg list = kNoReg;
        int64_t off = 0;
        int64_t coef = 1;
        VReg inv = kNoReg;
        bool write = false;
        int pos = 0;              // 体块内指令序号（读在写前判定）
        int bidx = -1;            // 基址键下标（AffBase）
    };
    // 一个 reduction 累加器（int 走 4-lane 时 IACC 当 ymm 用）。
    struct VRed {
        int slot = -1;
        VReg acc = kNoReg, expr = kNoReg, add = kNoReg;
        int store_pos = -1;
        bool is_float = false;
        MOperand loc{};                 // 槽位置
        bool acc_stack = false;         // float acc 在栈（scratch+每轮写回）
        MReg XT = kMRegNone;            // float acc 寄存器
        MReg IACC = kMRegNone;          // int acc 寄存器
    };
    // 4-lane 发射上下文：全部是 try_vect_loop 已定死的分析/分配结果（只读）。
    struct VAvx2Ctx {
        const std::vector<Inst>* body = nullptr;
        const std::vector<VRole>* role = nullptr;
        const std::vector<VAccess>* acc = nullptr;
        const std::vector<int>* acc_at = nullptr;
        const std::vector<AffBase>* bases = nullptr;
        const std::vector<VRed>* reds = nullptr;
        const std::function<MReg(VReg)>* xmm_of = nullptr;
        const std::vector<VReg>* uniq_inv = nullptr;
        const std::unordered_map<VReg, MReg>* inv_reg = nullptr;
        const std::vector<int64_t>* kcoefs = nullptr;
        const std::unordered_map<int64_t, MReg>* kreg = nullptr;
        MReg jreg = kMRegNone;
        bool j_stack = false;
        MOperand jloc;
        MOperand n_op;
        MReg gscr = kMRegNone;    // coef>1 装载的打包 scratch（VPINSRQ 用）
        MReg seq1 = kMRegNone;    // int 乘 pmuludq 重建 scratch
        MReg seq2 = kMRegNone;
        MReg hscr = kMRegNone;    // int reduction 水平求和 scratch（折叠也用）
        const std::string* vec = nullptr;   // SSE2 2-lane 循环标签（回退目标）
        bool int_ok = false;      // 体块全是 int 值（无任何 VecF/InvF/float acc）
    };
    // 发「运行时派发 + 4-lane 循环 + 折回 2 lane」。不满足 4-lane 前提时
    // 返回 false 且**一个字节都不发**（SSE2 路径照旧）。成功时 *vexit 收回
    // 两条循环的公共出口标签（由调用方在 SSE2 循环之后 label 出来）。
    bool vect_avx2_emit(const VAvx2Ctx& ctx, std::string* vexit);

    // 仿射下标解析（递归；depth 仅防病态深链）。成功 = 可折进寻址的仿射式。
    AffIdx vect_affine_index(VReg v, const std::vector<int>& jalias,
                             const std::function<bool(VReg)>& inv_ok,
                             int depth = 0) const;
    // 规划期校验（发射前）：常量编不进 imm32/disp32 → 整段保守跳过。
    bool vect_affine_plan_ok(const std::vector<AffBase>& bases) const;
    // 边界守卫：0 <= coef*(n-1)+inv+off < len；每步算术 jo 溢出 → 跳 tail。
    bool vect_affine_guard(const MOperand& list_obj, const AffBase& b,
                           const MOperand& n_op, const std::string& tail);
    // base = data(list_obj)（+ inv*8，若 inv != kNoReg）。
    void vect_affine_base(const MOperand& list_obj, const AffBase& b, MReg reg);
    // inv 判等守卫：同对象读写只在两个下标序列完全相同时才能就地向量化。
    void vect_affine_inv_eq(const AffEq& eq, const std::string& tail);
    // coef>1 的运行期跨步计数器：K = coef*8*j，每轮 += coef*16。
    void vect_affine_kinit(MReg k, MReg jreg, int64_t coef);
    void vect_affine_step(MReg k, int64_t coef);
    // 2-lane 装载：coef==1 用 movupd；coef>1 两次 movsd + unpcklpd（无 gather）。
    void vect_affine_load(MReg base, MReg j, MReg k, int64_t coef, int64_t off,
                          MReg dst, MReg scratch);
    // 2-lane 存储：只支持 coef==1（不做 scatter）。
    void vect_affine_store(MReg base, MReg j, int64_t off, MReg src);
