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
