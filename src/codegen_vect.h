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
    // 向量值分类（try_vect_loop 的分析结果）。放在类体里而不是 .cpp 的匿名
    // 命名空间：codegen_vect_jvec.cpp 的寄存器规划（同值常量归并 / 归纳变量
    // 生命周期 / 临时值线性扫描）要读同一份分类，判据只有一处。
    // InvF/InvI 是循环不变量（setup 广播）；J 是归纳变量；Affine 是 j+常量
    //（只用于下标位移）；VecF/VecI 是逐 lane 计算的 2-lane 值；Acc 是
    // reduction 累加器旧值；Fail 不可向量化。
    enum class VK : uint8_t { Fail, InvF, InvI, InvPtr, J, Affine, VecF, VecI, Acc };
    struct VClass {
        VK kind = VK::Fail;
        int64_t off = 0;          // Affine：j+off；Convert j_src：[j+off, j+off+1]
        VReg list = kNoReg;       // VecF/VecI 来自 IndexLoad 时的 list vreg
        bool is_load = false;     // VecF/VecI 是内存装载（vs 算术）
        VReg mov_src = kNoReg;    // Mov 透明别名 / 同值常量归并的规范 vreg
        bool j_src = false;       // Convert 输入是归纳变量 j（或 j+常量）
    };
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
        // 归纳变量物化寄存器（P5b，2-lane 侧由 vect_jvec2 物化 [j, j+1]）：
        // 4-lane 循环自己把它扩成 [j, j+1, j+2, j+3]（低 128 位 + vpinsrq
        // 拼高 128 位）。jvec == kMRegNone → 体块里没有 j 参与算术。
        MReg jvec = kMRegNone;
        bool has_j = false;
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
    bool vect_div_pow2(VReg b, int* sh) const;
    // 2-lane 发射：rd 已持有被除数 x（2 lane i64），结果原地写回 rd。tmp 是
    // scratch（seq 寄存器）。% 不用 scratch；// 的 m>0 用 4 条 packed 合成。
    void vect_intdiv2(MReg rd, MReg tmp, bool is_mod, int sh);

    // ── P5b · 归纳变量 j 作为整数算术操作数（实现见 codegen_vect_jvec.cpp）──
    // 判据只有一条结构性质：操作数是 j-alias 槽的装载（归纳变量本身或其
    // 副本槽）。j 在整型 Bin 里当 VecI 用（Add/Sub/Mul/除以 2 的幂常量），
    // 每轮迭代在首个使用点物化一次 [j, j+1]（4-lane 由 AVX2 段物化
    // [j..j+3]）。序列与 Convert 的 j_src 分支同一份，逐 lane 模 2^64 与
    // 标量 addq/subq/imulq 逐位一致，任意 i64 无需值域守卫。
    void vect_jvec2(MReg jreg, int64_t off, MReg dst, MReg scr);
    // 使用点分类：除 IndexLoad/IndexStore 的下标操作数外，任何用途都记进
    // out。折叠进寻址的仿射式必须**只**当下标用 —— j 参与算术后，同一个值
    // 可能既当下标又当算术语，折叠会让发射期取不到寄存器（半截 asm）。
    void vect_mark_nonindex_uses(std::unordered_map<VReg, char>* out) const;
    // 同值常量归并：按值把重复的 InvI/InvF 常量 vreg 接到首个 vreg 的
    // mov_src 上（广播寄存器只占一个）。语义等价，只为省寄存器。
    void vect_merge_const_invs(std::unordered_map<VReg, VClass>* cls);
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
    };
    bool vect_alloc_temps(VTempPlan* p);
