// vect_planner_intfast.cpp —— 通用向量化：int64 索引算术的 32 位守卫快路径
//
// 背景：通用路径要对**任意 i64** 保持与标量逐位一致，所以 int 乘 / // 2^m /
// i64→f64 折算都是完整 64 位模拟（3 次 pmuludq 拼装 / 符号修正右移 / 拆高低
// 32 位各转一次再拼）。特化模板（codegen_simd*.cpp）靠运行期守卫证明索引 <
// 2^31，把这些压成 1~2 条指令。本文件把**这项技术**搬进通用路径，判据只有
// IR 结构 + 运行期守卫，与具体基准/形状无关：
//
//   1. 找 int→float 的 Convert（i64→f64），其源表达式必须含 int64 乘 ——
//      没有乘法的循环不付守卫成本（判据见 int_fast_plan）。
//   2. 上界分析：把归纳变量 J、每个循环不变 int（InvI）上界都记成 K-1，
//      常量用精确值；Add/Mul/FloorDiv-by-2^m 做区间算术。K 是运行期守卫的
//      上界（n <= K 且每个不变量 0 <= inv < K），所以 j <= n-1 <= K-1。
//   3. 二分找**最大的 K**，使最终折算值 < 2^31（pshufd+cvtdq2pd 的 signed
//      i32 精确范围）、乘法操作数 < 2^32（pmuludq 只取低 32 位）、中间
//      乘积/加法不溢出 int64（保证非负 + 位模式精确）。
//   4. 入口发守卫（setup 一次）：n <= K；每个参与的不变量 0 <= inv < K。
//      守卫失败回**标量尾**：两条向量
//      版本会让体块/寄存器规划翻倍，而大索引循环通常还有其它守卫（长度、
//      仿射上界）先失败；标量尾与现有 sqrt / 负下标 / 跨组别名守卫同一条
//      保守回退路径，语义与现在完全一致。
//   5. 发射期：乘 → 单条 pmuludq；// 2^m → 单条 psrlq；i64→f64 → pshufd
//      0x88 + cvtdq2pd（值在 [0,2^31)，signed i32 精确）。
//
// 正确性依据：守卫通过时，所有 fast 节点都是非负且 < 2^32（乘法操作数）/
// < 2^31（最终折算值）；pmuludq 对非负 32 位操作数是精确 64 位积，psrlq 对
// 非负被除数是 floor 除 2^m，pshufd+cvtdq2pd 对 [0,2^31) 精确 —— 与标量
// imulq / sarq / cvtsi2sdq 在这些值域上逐位一致。
#include "codegen_impl.h"

namespace pyadamas {

namespace {
constexpr int64_t kFastLimit = int64_t{1} << 31;        // 折算值必须 < 2^31
constexpr int64_t kMulOperandLimit = int64_t{1} << 32;  // pmuludq 只取低 32 位
constexpr int64_t kI64Max = INT64_MAX;
}  // namespace

// 上界分析：返回 false = 该表达式不能走 32 位快路径（含乘法以外的不可证
// 情形）。成功时 *ub 是值上界（非负），invs 收集需要入口守卫的不变量，
// has_mul 置位表示子树里出现了 int64 乘。
bool VectPlanner::Loop::int_fast_bound(VReg v, int64_t K, int64_t* ub,
                                       std::unordered_set<VReg>* invs, bool* has_mul) {
    v = resolve_src(v);
    const Inst* d = gen_.simd_def(v);
    if (d == nullptr) return false;
    if (d->op == IrOp::ConstInt) {
        if (d->imm < 0 || d->imm >= kFastLimit) return false;
        *ub = d->imm;
        return true;
    }
    auto it = cls.find(v);
    if (it == cls.end()) return false;
    switch (it->second.kind) {
        case VK::J:
            // 归纳变量：lane0 = j、lane1 = j+1，守卫 j>=0、j+1<n 与 n<=K
            // 保证两个 lane 都 <= n-1 <= K-1。
            *ub = K - 1;
            return true;
        case VK::InvI:
            // 非 const 的不变量：入口守卫保证 0 <= inv < K。
            if (invs != nullptr) invs->insert(v);
            *ub = K - 1;
            return true;
        case VK::VecI: {
            if (d->op != IrOp::Bin) return false;
            int64_t ua = 0, ub2 = 0;
            switch (d->bop) {
                case BinKind::Add: {
                    if (!int_fast_bound(d->a, K, &ua, invs, has_mul)) return false;
                    if (!int_fast_bound(d->b, K, &ub2, invs, has_mul)) return false;
                    if (ua > kI64Max - ub2) return false;  // 可能溢出 i64
                    *ub = ua + ub2;
                    return true;
                }
                case BinKind::Sub:
                    return false;  // 可能变负：非负性不成立
                case BinKind::Mul: {
                    if (!int_fast_bound(d->a, K, &ua, invs, has_mul)) return false;
                    if (!int_fast_bound(d->b, K, &ub2, invs, has_mul)) return false;
                    if (ua >= kMulOperandLimit || ub2 >= kMulOperandLimit) return false;
                    const __int128 p = static_cast<__int128>(ua) * ub2;
                    if (p > kI64Max) return false;  // 可能溢出 i64（lane 变负）
                    if (has_mul != nullptr) *has_mul = true;
                    *ub = static_cast<int64_t>(p);
                    return true;
                }
                case BinKind::FloorDiv: {
                    int sh = -1;
                    if (!owner_.vect_div_pow2(d->b, &sh)) return false;
                    if (sh < 0 || sh > 62) return false;
                    if (!int_fast_bound(d->a, K, &ua, invs, has_mul)) return false;
                    *ub = ua >> sh;
                    return true;
                }
                default:
                    return false;
            }
        }
        default:
            return false;
    }
}

// 分析入口：找「int→float 的 Convert，源表达式含 int64 乘」的体块，二分出
// 最大守卫上界 K，标记可走短序列的节点。返回 true 表示启用；失败不影响
// 向量化（调用方忽略返回值）。
bool VectPlanner::Loop::int_fast_plan() {
    struct Cand {
        VReg conv = kNoReg;   // Convert 结果（resolve 后）
        VReg src = kNoReg;    // Convert 的 int 源（resolve 后，VecI）
    };
    std::vector<Cand> cands;
    for (const Inst& in : BI) {
        if (in.op != IrOp::Convert) continue;
        TypePtr to = gen_.ty_of(in.dst);
        TypePtr from = gen_.ty_of(in.a);
        if (!to || to->kind != TyKind::Float) continue;
        if (!from || from->kind != TyKind::Int) continue;
        VReg r = resolve_src(in.dst);
        auto ri = cls.find(r);
        if (ri == cls.end() || ri->second.kind != VK::VecF || ri->second.j_src) continue;
        VReg src = resolve_src(in.a);
        auto sit = cls.find(src);
        if (sit == cls.end() || sit->second.kind != VK::VecI) continue;
        cands.push_back(Cand{r, src});
    }
    if (cands.empty()) return false;
    // 二分最大 K：上界分析对 K 单调（K 越小越容易满足）。
    auto fits = [&](int64_t K) -> bool {
        for (const Cand& c : cands) {
            bool has_mul = false;
            int64_t ub = 0;
            if (!int_fast_bound(c.src, K, &ub, nullptr, &has_mul)) return false;
            if (!has_mul || ub >= kFastLimit) return false;
        }
        return true;
    };
    int64_t lo = 2, hi = kFastLimit, best = -1;
    while (lo <= hi) {
        const int64_t mid = lo + (hi - lo) / 2;
        if (fits(mid)) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (best < 0) return false;
    // 标记：Convert 结果 → 短转换；子树里的 Mul / FloorDiv → 短序列。
    std::unordered_set<VReg> invs;
    for (const Cand& c : cands) {
        bool has_mul = false;
        int64_t ub = 0;
        if (!int_fast_bound(c.src, best, &ub, &invs, &has_mul) || !has_mul) continue;
        intfast_conv.insert(c.conv);
        int_fast_mark(c.src, invs);
    }
    if (intfast_conv.empty()) return false;
    for (VReg v : invs) intfast_invs.push_back(v);
    intfast_k = best;
    intfast = true;
    if (std::getenv("PYADAMAS_VECT_DBG"))
        std::fprintf(stderr, "[vect] intfast bb%d K=%lld convs=%zu invs=%zu\n",
                     body_bi, static_cast<long long>(best), intfast_conv.size(),
                     intfast_invs.size());
    return true;
}

// 标记子树：VecI 的 Mul/FloorDiv 节点进快路径集合。语义上 fast 乘用 pmuludq
// 对非负 (<2^32) 操作数逐位精确，即使该节点的其它使用者也安全。
void VectPlanner::Loop::int_fast_mark(VReg v, std::unordered_set<VReg>& invs) {
    v = resolve_src(v);
    const Inst* d = gen_.simd_def(v);
    if (d == nullptr) return;
    auto it = cls.find(v);
    if (it == cls.end()) return;
    if (it->second.kind == VK::InvI) {
        if (d->op != IrOp::ConstInt) invs.insert(v);
        return;
    }
    if (it->second.kind != VK::VecI || d->op != IrOp::Bin) return;
    switch (d->bop) {
        case BinKind::Mul:
            intfast_mul.insert(v);
            int_fast_mark(d->a, invs);
            int_fast_mark(d->b, invs);
            return;
        case BinKind::FloorDiv:
            intfast_div.insert(v);
            int_fast_mark(d->a, invs);
            return;
        case BinKind::Add:
            int_fast_mark(d->a, invs);
            int_fast_mark(d->b, invs);
            return;
        default:
            return;
    }
}

// 入口守卫（setup 一次）：守卫失败 jmp 标量尾 —— 与 sqrt / 负下标 / 跨组
// 别名守卫同一条回退路径，语义与现在完全一致。
void VectPlanner::Loop::emit_int_fast_guards() {
    if (!intfast) return;
    // n <= K：归纳变量 lane1 = j+1 <= n-1 <= K-1 靠这条。
    gen_.emit_imm(MOp::kCmpq, intfast_k, n_op);
    gen_.emit_jmp(MOp::kJg, tail);
    for (VReg v : intfast_invs) {
        gen_.emit2(MOp::kMovq, list_base_mop(v), kRax);
        gen_.emit2(MOp::kTestq, kRax, kRax);
        gen_.emit_jmp(MOp::kJs, tail);        // inv < 0：非负性不成立
        gen_.emit_imm(MOp::kCmpq, intfast_k, kRax);
        gen_.emit_jmp(MOp::kJge, tail);       // inv >= K：上界不成立
    }
}

}  // namespace pyadamas
