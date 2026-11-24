// backend/vect_avx2.cpp —— 通用循环向量化的 AVX2 4-lane（P3）
//
// 设计见 docs/vectorization-generalization.md §「P3 · 4-lane（AVX2）」。
//
// 范围**只有整数路径**：int map（out[j] = a[j]*2 + 1）与 int reduction
// （s += a[j]*b[j]）发 4 lane ymm。浮点路径**保持** SSE2 2-lane —— 浮点
// reduction 必须保序累加（逐位一致），4-lane 分批累加拿不到收益也不允许
// 换语义（文档 §P3 与 §5）。
//
// 结构（三步，全部只依赖 IR 结构，不含任何基准名或基准字面量）：
//
// 1) 计划（emit 前半段）：按 backend/vect.cpp 已经判定的角色表（VRole）把
//    体块摊平成 4-lane 步骤，寄存器/偏移/系数全部取 SSE2 路径**同一份**
//    分配结果（tmp_reg / inv_reg / bases / kreg）。任何一条证不出来
//    （非 int 值、写出跨步、缺 scratch、寄存器不在 xmm 池）→ 直接返回
//    false，**一个字节都不发**，SSE2 路径照旧。
// 2) 运行时派发：前奏读运行时写好的 CPU 能力字节 pa_cpu_avx2（见
//    runtime/rt_list.c：构造函数在 main 之前用 CPUID 写好；初值 0 是保守
//    值）。非 0 才进 4-lane 循环，0 直接跳到现有 SSE2 2-lane 循环 ——
//    非 AVX2 机器上一条 VEX 指令都不会执行，所以不会 SIGILL。
// 3) 出口：4 lane 累加器折回 2 lane（vextracti128 + vpaddq），让 SSE2 的
//    水平求和出口**原样复用**；离开 AVX2 段前发 vzeroupper（后面全是
//    legacy SSE，不清会吃 AVX-SSE 过渡惩罚）。
//
// 装载/写出：coef==1 用 vmovupd 一次 32 字节；coef>1（跨步装载，SSE2 里是
// 两次 movsd + unpcklpd）用 vmovq + vpinsrq 把 4 个标量拼成 ymm[0..3]，
// 只要 1 个 scratch。写出仍只支持 coef==1（不做 scatter，与 2-lane 同口径）。
//
// 寄存器：4-lane 沿用 2-lane 的寄存器编号（ymmN 与 xmmN 是同一个物理寄存器
// 的两种宽度视图），所以分配结果直接复用，不需要第二套分配器；两条循环互斥
// 执行，不存在「谁把谁的 xmm 写坏」的问题。
#include "backend/impl.h"

#include <cstdio>

namespace pyadamas {

namespace {

// 4-lane 体的一条步骤。分析期把寄存器/偏移全部定死，发射期只做文本展开，
// 不再有任何判断 —— dry-run 完备：不可能「发到一半 return false」。
struct Avx2Step {
    // Div / Mod：整数 // 与 %（除数是 2 的幂常量，判据与 2-lane 同源，见
    // codegen_vect_intdiv.cpp）。除数不占寄存器，只用编译期移位量 sh。
    enum Kind : uint8_t { Load, Add, Sub, Mul, Div, Mod, Store, Red } kind = Load;
    int sh = 0;               // Div / Mod 的移位量 m（除数 == 2^m）
    MReg dst = kMRegNone;     // Load / Add / Sub / Mul / Div / Mod 的目标
    MReg a = kMRegNone;       // Add / Sub / Mul / Div / Mod 左操作数
    MReg b = kMRegNone;       // Add / Sub / Mul 右操作数
    MReg base = kMRegNone;    // Load / Store 的基址（data + inv*8）
    MReg src = kMRegNone;     // Store 的值 / Red 的累加器
    MReg expr = kMRegNone;    // Red 的表达式
    MReg k = kMRegNone;       // coef>1 的运行期跨步计数器
    int64_t coef = 1;
    int64_t off = 0;
};

// 编译期开关：PYADAMAS_AVX2=0 时不发 4-lane（只发 SSE2），默认开。
// 运行时有没有 AVX2 由 pa_cpu_avx2 在派发点判断，这里只管「发不发」。
bool avx2_compile_on() {
    const char* v = std::getenv("PYADAMAS_AVX2");
    return !(v != nullptr && v[0] == '0' && v[1] == '\0');
}

std::string xr(MReg r) { return std::string(mreg_name(r)); }          // %xmmN
std::string yr(MReg r) {                                             // %ymmN
    return "%ymm" + std::to_string(static_cast<int>(r) - static_cast<int>(kXmm0));
}

// disp(%base,%index,scale) —— 与 op_mem(base,index,scale,disp) 同字节。
std::string mem_op(int64_t disp, MReg base, MReg idx, int scale) {
    std::string s = std::to_string(disp);
    s += '(';
    s += mreg_name(base);
    s += ',';
    s += mreg_name(idx);
    s += ',';
    s += std::to_string(scale);
    s += ')';
    return s;
}

bool in_xmm_pool(MReg r) { return r >= kXmm0 && r <= kXmm15; }
bool gpr_scratch_ok(MReg r) { return r != kMRegNone && r != kRax && r >= kRax && r <= kR15; }

}  // namespace

bool AsmGen::vect_avx2_emit(const VAvx2Ctx& c, std::string* vexit) {
    if (vexit == nullptr || !c.int_ok || !avx2_compile_on()) return false;
    if (c.body == nullptr || c.role == nullptr || c.acc == nullptr ||
        c.acc_at == nullptr || c.bases == nullptr || c.reds == nullptr ||
        c.xmm_of == nullptr || c.uniq_inv == nullptr || c.inv_reg == nullptr ||
        c.kcoefs == nullptr || c.kreg == nullptr || c.vec == nullptr)
        return false;
    // j 必须是真寄存器（栈上 j 已在前奏提到 JREG），且不能是 rax（循环临时）。
    if (!gpr_scratch_ok(c.jreg) || c.jreg == kRax) return false;

    // ── 1) 计划：把体块摊平成 4-lane 步骤 ────────────────────────────────
    std::vector<Avx2Step> steps;
    std::vector<MReg> iaccs;                    // int reduction 的 4-lane 累加器
    bool need_mul = false;
    bool need_div = false;                      // // 2^m 的符号修正要 1 个 scratch
    bool need_j = false;                        // 归纳变量 4-lane 物化要 1 个 scratch
    for (size_t i = 0; i < c.body->size(); ++i) {
        const Inst& in = (*c.body)[i];
        const int at = (*c.acc_at)[i];
        Avx2Step s;
        switch ((*c.role)[i]) {
            case VRole::Skip:
                continue;
            case VRole::Load: {
                if (at < 0 || static_cast<size_t>(at) >= c.acc->size()) return false;
                const VAccess& a = (*c.acc)[static_cast<size_t>(at)];
                if (a.coef < 1 || a.coef > kAffMaxCoef) return false;
                if (a.off < 0 || a.off > kAffMaxOff) return false;
                if (a.bidx < 0 || static_cast<size_t>(a.bidx) >= c.bases->size()) return false;
                s.kind = Avx2Step::Load;
                s.base = (*c.bases)[static_cast<size_t>(a.bidx)].reg;
                s.coef = a.coef;
                s.off = a.off;
                s.dst = (*c.xmm_of)(in.dst);
                if (!gpr_scratch_ok(s.base) || !in_xmm_pool(s.dst)) return false;
                if (a.coef != 1) {
                    // 跨步装载要两次标量装载打包，靠 1 个 scratch + 跨步计数器。
                    auto k = c.kreg->find(a.coef);
                    if (k == c.kreg->end() || c.gscr == kMRegNone) return false;
                    if (!in_xmm_pool(c.gscr) || !gpr_scratch_ok(k->second)) return false;
                    s.k = k->second;
                }
                break;
            }
            case VRole::Bin: {
                // 4-lane 只认 int 加/减/乘；Convert（i64→f64）留在 2-lane。
                if (in.op == IrOp::Convert) return false;
                TypePtr td = ty_of(in.dst);
                TypePtr ta = ty_of(in.a);
                TypePtr tb = ty_of(in.b);
                if (!td || td->kind != TyKind::Int || !ta || ta->kind != TyKind::Int ||
                    !tb || tb->kind != TyKind::Int)
                    return false;
                // P5：// 与 % 的除数是 2 的幂常量 → 4-lane 也能做（判据与
                // 2-lane 同源：只看除数是不是编译期正的 2 的幂）。除数不进
                // 寄存器、不参与广播，只用编译期移位量。
                int dsh = -1;
                const bool intdiv2 =
                    (in.bop == BinKind::FloorDiv || in.bop == BinKind::Mod) &&
                    vect_div_pow2(in.b, &dsh);
                switch (in.bop) {
                    case BinKind::Add: s.kind = Avx2Step::Add; break;
                    case BinKind::Sub: s.kind = Avx2Step::Sub; break;
                    case BinKind::Mul: s.kind = Avx2Step::Mul; need_mul = true; break;
                    case BinKind::FloorDiv:
                    case BinKind::Mod:
                        if (!intdiv2) return false;       // 非 2 的幂除数：留给标量
                        s.kind = (in.bop == BinKind::Mod) ? Avx2Step::Mod : Avx2Step::Div;
                        s.sh = dsh;
                        if (s.kind == Avx2Step::Div && dsh > 0) need_div = true;
                        break;
                    default: return false;
                }
                s.dst = (*c.xmm_of)(in.dst);
                s.a = (*c.xmm_of)(in.a);
                if (!in_xmm_pool(s.dst) || !in_xmm_pool(s.a)) return false;
                s.b = (s.kind == Avx2Step::Div || s.kind == Avx2Step::Mod)
                          ? kMRegNone : (*c.xmm_of)(in.b);   // // 与 % 不读除数
                if (s.b != kMRegNone && !in_xmm_pool(s.b)) return false;
                // P5b：归纳变量当算术操作数 → 4-lane 循环顶部要物化 [j..j+3]。
                if (c.has_j && (s.a == c.jvec || s.b == c.jvec)) need_j = true;
                break;
            }
            case VRole::MapStore: {
                if (at < 0 || static_cast<size_t>(at) >= c.acc->size()) return false;
                const VAccess& a = (*c.acc)[static_cast<size_t>(at)];
                // 写出只支持相邻 4 lane（和 2-lane 一样不做 scatter）。
                if (a.coef != 1 || a.off < 0 || a.off > kAffMaxOff) return false;
                if (a.bidx < 0 || static_cast<size_t>(a.bidx) >= c.bases->size()) return false;
                TypePtr tc = ty_of(in.c);
                if (!tc || tc->kind != TyKind::Int) return false;
                s.kind = Avx2Step::Store;
                s.src = (*c.xmm_of)(in.c);
                s.base = (*c.bases)[static_cast<size_t>(a.bidx)].reg;
                s.off = a.off;
                if (!gpr_scratch_ok(s.base) || !in_xmm_pool(s.src)) return false;
                break;
            }
            case VRole::RedStore: {
                const VRed* r = nullptr;
                for (const VRed& x : *c.reds)
                    if (x.slot == in.idx) r = &x;
                if (r == nullptr || r->is_float) return false;   // 浮点累加必须保序
                if (!in_xmm_pool(r->IACC)) return false;
                s.kind = Avx2Step::Red;
                s.src = r->IACC;
                s.expr = (*c.xmm_of)(r->expr);
                if (!in_xmm_pool(s.expr)) return false;
                if (std::find(iaccs.begin(), iaccs.end(), r->IACC) == iaccs.end())
                    iaccs.push_back(r->IACC);
                break;
            }
            default:
                return false;                        // Fail 不该传到发射器
        }
        steps.push_back(s);
    }
    if (steps.empty()) return false;
    if (need_mul && (!in_xmm_pool(c.seq1) || !in_xmm_pool(c.seq2))) return false;
    if (need_div && !in_xmm_pool(c.seq1)) return false;
    // 归纳变量 4-lane 物化：借用任一序列/gather/水平求和 scratch 拼高 128 位
    //（物化在循环体最前面原子完成，之后 scratch 才被各自的步骤复用）。
    MReg jscr = kMRegNone;
    if (need_j) {
        if (!c.has_j || !in_xmm_pool(c.jvec)) return false;
        for (MReg cand : {c.seq1, c.gscr, c.hscr})
            if (cand != c.jvec && in_xmm_pool(cand)) { jscr = cand; break; }
        if (jscr == kMRegNone) return false;      // 没有 scratch → 交给 2-lane
    }
    // 累加器折回 2 lane 要一个 xmm scratch（SSE2 出口的水平求和 scratch，
    // 分配在 IACC 之后，保证互不冲突）。
    if (!iaccs.empty() && !in_xmm_pool(c.hscr)) return false;

    // 不变量广播清单：4-lane 体里所有不变量都要 ymm 副本（SSE2 前奏已经把值
    // 铺在 xmm 低 64 位，vpbroadcastq 直接铺满 4 lane）。
    std::vector<MReg> bcast;
    for (VReg v : *c.uniq_inv) {
        auto it = c.inv_reg->find(v);
        if (it == c.inv_reg->end() || !in_xmm_pool(it->second)) return false;
        bcast.push_back(it->second);
    }
    // 跨步计数器推进量：一条 4-lane 迭代消费 4 个 lane × coef 个元素 × 8 字节。
    std::vector<std::pair<MReg, int64_t>> ksteps;
    for (int64_t co : *c.kcoefs) {
        auto it = c.kreg->find(co);
        if (it == c.kreg->end() || !gpr_scratch_ok(it->second)) return false;
        ksteps.emplace_back(it->second, co * 32);
    }

    // ── 2) 发射：运行时派发 + 4-lane 循环 ────────────────────────────────
    const std::string vexit_lbl = fresh_label("vectexit");
    const std::string v4 = fresh_label("vect4");
    *vexit = vexit_lbl;
    if (std::getenv("PYADAMAS_VECT_DBG"))
        std::fprintf(stderr, "[vect] avx2 4-lane (steps=%zu reds=%zu inv=%zu)\n",
                     steps.size(), iaccs.size(), bcast.size());
    comment("VECT AVX2 4-lane: int map/reduction, 运行时派发 pa_cpu_avx2");
    // 派发：pa_cpu_avx2 由运行时构造函数写好（0 = 无 AVX2 / 未初始化）。
    line("cmpl $0, pa_cpu_avx2(%rip)");
    emit_jmp(MOp::kJe, *c.vec);
    // 不足 4 个元素 → 交给 2-lane 循环（它自带 j+1<n 入口守卫）。
    emit2(MOp::kLeaq, op_mem(c.jreg, 3), kRax);
    emit2(MOp::kCmpq, c.n_op, kRax);
    emit_jmp(MOp::kJge, *c.vec);
    for (MReg r : bcast) line("vpbroadcastq " + xr(r) + ", " + yr(r));
    if (!iaccs.empty()) {
        // 低 128 位 = [槽前值, 0]（SSE2 前奏的 movq 已保证），高 128 位补 0：
        // 累加初值只能加一次，不能广播成 4 份。
        line("vpxor " + xr(c.hscr) + ", " + xr(c.hscr) + ", " + xr(c.hscr));
        for (MReg r : iaccs)
            line("vinserti128 $1, " + xr(c.hscr) + ", " + yr(r) + ", " + yr(r));
    }
    label(v4);
    bool j_built = false;
    for (const Avx2Step& s : steps) {
        // P5b：归纳变量物化 —— 首个使用点（每轮迭代一次）拼 [j, j+1, j+2, j+3]。
        // 低 128 位用 vmovq/vpinsrq 从 JREG 直接拼进 xmm(jvec)，高 128 位拼到
        // jscr 再 vinserti128 插入。j 是任意 i64（含负数），与 2-lane 路径
        // 逐 lane 同值；这里没有任何值域假设。
        if (need_j && !j_built &&
            (s.a == c.jvec || s.b == c.jvec || s.src == c.jvec || s.expr == c.jvec)) {
            emit2(MOp::kLeaq, op_mem(c.jreg, 0), kRax);
            line("vmovq %rax, " + xr(c.jvec));
            emit2(MOp::kLeaq, op_mem(c.jreg, 1), kRax);
            line("vpinsrq $1, %rax, " + xr(c.jvec) + ", " + xr(c.jvec));
            emit2(MOp::kLeaq, op_mem(c.jreg, 2), kRax);
            line("vmovq %rax, " + xr(jscr));
            emit2(MOp::kLeaq, op_mem(c.jreg, 3), kRax);
            line("vpinsrq $1, %rax, " + xr(jscr) + ", " + xr(jscr));
            line("vinserti128 $1, " + xr(jscr) + ", " + yr(c.jvec) + ", " + yr(c.jvec));
            j_built = true;
        }
        switch (s.kind) {
            case Avx2Step::Load:
                if (s.coef == 1) {
                    // 4 个相邻 i64 = 32 字节，一条 32 字节装载。
                    line("vmovupd " + mem_op(s.off * 8, s.base, c.jreg, 8) + ", " + yr(s.dst));
                } else {
                    // 跨步装载：逐 lane 64 位装载 + vpinsrq 打包（无 gather 指令；
                    // 下标已由守卫证完 coef*(n-1)+inv+off < len，4 个都越不了界）。
                    line("vmovq " + mem_op(s.off * 8, s.base, s.k, 1) + ", " + xr(s.dst));
                    line("vpinsrq $1, " + mem_op((s.off + s.coef) * 8, s.base, s.k, 1) + ", " +
                         xr(s.dst) + ", " + xr(s.dst));
                    line("vmovq " + mem_op((s.off + 2 * s.coef) * 8, s.base, s.k, 1) + ", " +
                         xr(c.gscr));
                    line("vpinsrq $1, " + mem_op((s.off + 3 * s.coef) * 8, s.base, s.k, 1) + ", " +
                         xr(c.gscr) + ", " + xr(c.gscr));
                    line("vinserti128 $1, " + xr(c.gscr) + ", " + yr(s.dst) + ", " + yr(s.dst));
                }
                break;
            case Avx2Step::Add:
            case Avx2Step::Sub:
                // 三操作数 VEX：目标可以和源同寄存器，和 SSE2 的 movapd 拷贝等价。
                line(std::string(s.kind == Avx2Step::Add ? "vpaddq " : "vpsubq ") + yr(s.b) +
                     ", " + yr(s.a) + ", " + yr(s.dst));
                break;
            case Avx2Step::Mul: {
                // 全 i64 乘重建（与 SSE2 的 pmuludq 序列同一推导，4 个 lane 各自
                // 成立）：a*b mod 2^64 = a_lo*b_lo + ((a_lo*b_hi + a_hi*b_lo) mod
                // 2^32) << 32。vpsrlq/vpmuludq/vpaddq 都是逐 64 位 lane，模 2^64
                // 与标量 imulq 低 64 位逐位一致，任意 i64 无需值域守卫。
                line("vmovdqa " + yr(s.b) + ", " + yr(c.seq1));
                line("vpsrlq $32, " + yr(c.seq1) + ", " + yr(c.seq1));       // b_hi
                line("vpmuludq " + yr(s.a) + ", " + yr(c.seq1) + ", " + yr(c.seq1));
                line("vmovdqa " + yr(s.a) + ", " + yr(c.seq2));
                line("vpsrlq $32, " + yr(c.seq2) + ", " + yr(c.seq2));       // a_hi
                line("vpmuludq " + yr(s.b) + ", " + yr(c.seq2) + ", " + yr(c.seq2));
                line("vpaddq " + yr(c.seq2) + ", " + yr(c.seq1) + ", " + yr(c.seq1));
                line("vpsllq $32, " + yr(c.seq1) + ", " + yr(c.seq1));
                line("vpmuludq " + yr(s.b) + ", " + yr(s.a) + ", " + yr(s.dst));
                line("vpaddq " + yr(c.seq1) + ", " + yr(s.dst) + ", " + yr(s.dst));
                break;
            }
            case Avx2Step::Div: {
                // floor(x / 2^m)：AVX2 同样没有 64 位 lane 的算术右移（vpsraq
                // 要 AVX-512），按 2 的补码恒等式合成（推导见 codegen_vect_intdiv.cpp）：
                //   x >>a m == (x >>u m) | (mask & ~((1<<(64-m)) - 1))
                //   mask    = 每 lane 符号掩码（x >= 0 时 0，x < 0 时全 1）
                // 四/五条都是逐 64 位 lane 的运算，任意 i64（含 INT64_MIN）成立。
                if (s.sh == 0) {                         // x // 1 == x
                    line("vmovdqa " + yr(s.a) + ", " + yr(s.dst));
                    break;
                }
                // 符号掩码：vpsrad $31（每 dword 符号）→ vpshufd 0xF5（每 64 位
                // lane 取高 dword 掩码，复制满整 lane）。不能用「psrlq $63 再
                // psllq」，那只置 1 位而不是高 m 位。
                line("vpsrad $31, " + yr(s.a) + ", " + yr(c.seq1));
                line("vpshufd $245, " + yr(c.seq1) + ", " + yr(c.seq1));
                line("vpsllq $" + std::to_string(64 - s.sh) + ", " + yr(c.seq1) + ", " +
                     yr(c.seq1));
                line("vpsrlq $" + std::to_string(s.sh) + ", " + yr(s.a) + ", " + yr(s.dst));
                line("vpor " + yr(c.seq1) + ", " + yr(s.dst) + ", " + yr(s.dst));
                break;
            }
            case Avx2Step::Mod:
                // x % 2^m == x & (2^m-1)：左移丢高位、再逻辑右移回来（低 m 位
                // 就是补码余数，负数同样成立）；x % 1 == 0。
                if (s.sh == 0) {
                    line("vpxor " + yr(s.dst) + ", " + yr(s.dst) + ", " + yr(s.dst));
                    break;
                }
                line("vpsllq $" + std::to_string(64 - s.sh) + ", " + yr(s.a) + ", " + yr(s.dst));
                line("vpsrlq $" + std::to_string(64 - s.sh) + ", " + yr(s.dst) + ", " + yr(s.dst));
                break;
            case Avx2Step::Store:
                line("vmovupd " + yr(s.src) + ", " + mem_op(s.off * 8, s.base, c.jreg, 8));
                break;
            case Avx2Step::Red:
                // paddq 模 2^64 精确（加法结合律/交换律成立）→ 与标量逐位一致。
                line("vpaddq " + yr(s.expr) + ", " + yr(s.src) + ", " + yr(s.src));
                break;
        }
    }
    // 跨步计数器每轮 += coef*32（4 个 lane，每 lane 跨 coef 个元素）。
    for (const auto& [reg, amt] : ksteps) emit_imm(MOp::kAddq, amt, reg);
    emit_imm(MOp::kAddq, 4, c.jreg);                    // j += 4
    if (c.j_stack) emit2(MOp::kMovq, c.jreg, c.jloc);   // while 形状：j 写回槽
    emit2(MOp::kLeaq, op_mem(c.jreg, 3), kRax);         // j+3 < n ?
    emit2(MOp::kCmpq, c.n_op, kRax);
    emit_jmp(MOp::kJl, v4);
    // 折回 2 lane：低 128 = [l0+l2, l1+l3]，高 128 由 vextracti128 取出后并入。
    // 出口的 SSE2 水平求和算 lo+hi = 前值 + l0+l1+l2+l3（模 2^64 精确）。
    for (MReg r : iaccs) {
        line("vextracti128 $1, " + yr(r) + ", " + xr(c.hscr));
        line("vpaddq " + xr(c.hscr) + ", " + xr(r) + ", " + xr(r));
    }
    // 离开 AVX2 段前清 YMM 高位：后面是 legacy SSE（水平求和/标量尾）。
    line("vzeroupper");
    emit_jmp(MOp::kJmp, *vexit);
    return true;
}

}  // namespace pyadamas
