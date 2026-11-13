// opt_cse.cpp —— 跨块公共子表达式消除
//
// Python 的条件循环会被降成「条件块 + 体块」：
//
//     while i < n and zr * zr + zi * zi <= 4.0:
//         ...
//
// 条件块算出 zr*zr / zi*zi，体块里又原样算一遍。本遍用前向「可用表达式」
// 数据流把这类冗余合并掉：体块直接复用条件块算出的 vreg，省掉两次乘法。
//
// 表达式键的构成：
//   * 局部槽的当前值编码成 Slot(s) —— 同一槽的两次装载因此能对上号；
//   * 常量按值编码，不同的常量 vreg 也能对上；
//   * 其余操作数用 vreg 编号；交换律算子按操作数排序规范化。
//
// store.slot s 会杀掉所有依赖 s 的表达式；依赖集合来自操作数 vreg 的传递
// 闭包。块入口状态 = 所有前驱出口状态的交集（同名键必须指向同一个 vreg），
// 这同时保证了替换值在每条路径上都定义过。
//
// 保守约定：
//   * 只删除「所有使用都在本块内」的重复指令，避免把跨块使用者的定义搬走；
//   * vreg 有多个定义（生成器续跑等）不参与；
//   * 函数里取过槽地址时整函数跳过 —— 间接写可能改槽，分析会失效。
#include "opt_detail.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

namespace pyadamas {
namespace optdetail {
namespace {

// 表达式操作数：局部槽的当前值 / 编译期常量 / 某个 vreg。
struct Operand {
    enum Kind : uint8_t { Reg = 0, Slot = 1, ConstInt = 2, ConstFloat = 3 };
    uint8_t kind = Reg;
    int64_t val = 0;

    bool operator==(const Operand& o) const { return kind == o.kind && val == o.val; }
    bool operator<(const Operand& o) const {
        if (kind != o.kind) return kind < o.kind;
        return val < o.val;
    }
};

struct Key {
    uint8_t op = 0;
    uint8_t sub = 0;  // bop / uop / cop
    uint8_t tyk = 0;
    Operand a{};
    Operand b{};

    bool operator==(const Key& o) const { return !(*this < o) && !(o < *this); }
    bool operator<(const Key& o) const {
        if (op != o.op) return op < o.op;
        if (sub != o.sub) return sub < o.sub;
        if (tyk != o.tyk) return tyk < o.tyk;
        if (!(a == o.a)) return a < o.a;
        return b < o.b;
    }
};

// 表达式依赖的槽集合；all 表示「算不清，任何 store 都杀掉它」。
struct Deps {
    bool all = false;
    std::vector<int> slots;
};

struct Entry {
    VReg v = kNoReg;
    Deps deps;
};

using State = std::map<Key, Entry>;

uint8_t ty_class(const TypePtr& t) {
    if (!t) return 0;
    switch (t->kind) {
        case TyKind::Int: return 1;
        case TyKind::Float: return 2;
        case TyKind::Bool: return 3;
        default: return 4;
    }
}

bool bin_commutative(BinKind k) {
    return k == BinKind::Add || k == BinKind::Mul || k == BinKind::BitAnd ||
           k == BinKind::BitOr || k == BinKind::BitXor;
}

bool cmp_commutative(CmpKind k) { return k == CmpKind::Eq || k == CmpKind::Ne; }

bool heap_like(const TypePtr& t) {
    if (!t) return true;
    return t->kind == TyKind::Str || t->kind == TyKind::Bytes ||
           t->kind == TyKind::Complex || ir_is_heap(t);
}

// 单个指令的静态定义信息。拷贝一份，避免块内指令被删后指针悬空。
struct DefInfo {
    IrOp op = IrOp::ConstNone;
    int32_t idx = -1;
    int64_t imm = 0;
    double fimm = 0.0;
    VReg a = kNoReg, b = kNoReg, c = kNoReg;
};

struct Ctx {
    IRFunction& fn;
    int nb = 0;
    VReg nv = 0;
    std::vector<int> ndefs;
    std::vector<DefInfo> def;
    std::vector<int> first_use_block;
    std::vector<char> multi_use;
    // vreg 的槽依赖（记忆化）
    std::vector<Deps> dep;
    std::vector<char> dep_done;
    std::vector<char> dep_busy;

    explicit Ctx(IRFunction& f)
        : fn(f),
          nb(static_cast<int>(f.blocks.size())),
          nv(static_cast<VReg>(f.vreg_ty.size())) {}

    TypePtr vt(VReg v) const {
        return v < nv ? fn.vreg_ty[v] : nullptr;
    }
};

bool cse_candidate(const Inst& in) {
    TypePtr ty = in.ty;
    switch (in.op) {
        case IrOp::Bin:
            if (heap_like(ty)) return false;
            switch (in.bop) {
                case BinKind::Add:
                case BinKind::Sub:
                case BinKind::Mul:
                case BinKind::BitAnd:
                case BinKind::BitOr:
                case BinKind::BitXor:
                case BinKind::Shl:
                case BinKind::Shr:
                    return true;
                case BinKind::Div:
                    // 整数除法有零除检查，走单独的慢路径，不合并。
                    return ty && ty->kind == TyKind::Float;
                default:
                    return false;
            }
        case IrOp::Un:
            return !heap_like(ty);
        // Cmp 不参与：它常常和紧跟的分支融合成一条 cmp+jcc，融合后不会真的
        // 写出布尔值；跨块复用这个 vreg 会读到没写过的寄存器。
        case IrOp::Convert:
            return ty && (ty->kind == TyKind::Bool || ty->kind == TyKind::Int ||
                          ty->kind == TyKind::Float);
        default:
            return false;
    }
}

bool same_state(const State& x, const State& y) {
    if (x.size() != y.size()) return false;
    for (auto i = x.begin(), j = y.begin(); i != x.end(); ++i, ++j)
        if (!(i->first == j->first) || i->second.v != j->second.v) return false;
    return true;
}

class CsePass {
public:
    explicit CsePass(IRFunction& f) : cx_(f) {}

    bool run();

private:
    void build_index();
    Operand encode(VReg v) const;
    Deps deps_of(VReg v);
    Deps merge_deps(VReg a, VReg b, VReg c);
    Key make_key(const Inst& in, VReg a, VReg b) const;
    bool process(Block& b, State& st, bool transform, int bi);

    Ctx cx_;
};

void CsePass::build_index() {
    Ctx& cx = cx_;
    cx.ndefs.assign(cx.nv, 0);
    cx.def.assign(cx.nv, DefInfo{});
    cx.first_use_block.assign(cx.nv, -1);
    cx.multi_use.assign(cx.nv, 0);
    cx.dep.assign(cx.nv, Deps{});
    cx.dep_done.assign(cx.nv, 0);
    cx.dep_busy.assign(cx.nv, 0);

    for (int bi = 0; bi < cx.nb; ++bi) {
        for (const auto& in : cx.fn.blocks[static_cast<size_t>(bi)].insts) {
            auto use = [&](VReg v) {
                if (v == kNoReg || v >= cx.nv) return;
                if (cx.first_use_block[v] < 0) cx.first_use_block[v] = bi;
                else if (cx.first_use_block[v] != bi) cx.multi_use[v] = 1;
            };
            use(in.a);
            use(in.b);
            use(in.c);
            for (VReg r : in.args) use(r);
            if (in.dst != kNoReg && in.dst < cx.nv) {
                cx.ndefs[in.dst]++;
                DefInfo& d = cx.def[in.dst];
                d.op = in.op;
                d.idx = in.idx;
                d.imm = in.imm;
                d.fimm = in.fimm;
                d.a = in.a;
                d.b = in.b;
                d.c = in.c;
            }
        }
    }
}

Operand CsePass::encode(VReg v) const {
    const Ctx& cx = cx_;
    Operand o;
    if (v == kNoReg) {
        o.kind = Operand::ConstInt;
        o.val = 0;
        return o;
    }
    if (v < cx.nv && cx.ndefs[v] == 1) {
        const DefInfo& d = cx.def[v];
        if (d.op == IrOp::LoadSlot && d.idx >= 0) {
            o.kind = Operand::Slot;
            o.val = d.idx;
            return o;
        }
        if (d.op == IrOp::ConstInt) {
            o.kind = Operand::ConstInt;
            o.val = d.imm;
            return o;
        }
        if (d.op == IrOp::ConstFloat) {
            int64_t bits = 0;
            std::memcpy(&bits, &d.fimm, sizeof(bits));
            o.kind = Operand::ConstFloat;
            o.val = bits;
            return o;
        }
    }
    o.kind = Operand::Reg;
    o.val = static_cast<int64_t>(v);
    return o;
}

Deps CsePass::deps_of(VReg v) {
    Ctx& cx = cx_;
    if (v == kNoReg) return Deps{};
    if (v >= cx.nv || cx.ndefs[v] != 1 || cx.dep_busy[v]) return Deps{true, {}};
    if (cx.dep_done[v]) return cx.dep[v];

    cx.dep_busy[v] = 1;
    const DefInfo& d = cx.def[v];
    Deps r;
    auto add = [&](const Deps& o) {
        if (o.all) {
            r.all = true;
            return;
        }
        r.slots.insert(r.slots.end(), o.slots.begin(), o.slots.end());
    };
    switch (d.op) {
        case IrOp::LoadSlot:
            if (d.idx >= 0) r.slots.push_back(d.idx);
            break;
        case IrOp::Bin:
        case IrOp::Un:
        case IrOp::Cmp:
        case IrOp::Convert:
        case IrOp::Mov:
            add(deps_of(d.a));
            add(deps_of(d.b));
            add(deps_of(d.c));
            break;
        default:
            break;  // 常量 / 全局装载 / 调用结果都不依赖槽
    }
    cx.dep_busy[v] = 0;
    if (!r.all) {
        std::ranges::sort(r.slots);
        r.slots.erase(std::unique(r.slots.begin(), r.slots.end()), r.slots.end());
    }
    cx.dep[v] = std::move(r);
    cx.dep_done[v] = 1;
    return cx.dep[v];
}

Deps CsePass::merge_deps(VReg a, VReg b, VReg c) {
    Deps r = deps_of(a);
    auto add = [&](const Deps& o) {
        if (r.all) return;
        if (o.all) {
            r.all = true;
            return;
        }
        r.slots.insert(r.slots.end(), o.slots.begin(), o.slots.end());
    };
    add(deps_of(b));
    add(deps_of(c));
    if (!r.all) {
        std::ranges::sort(r.slots);
        r.slots.erase(std::unique(r.slots.begin(), r.slots.end()), r.slots.end());
    }
    return r;
}

Key CsePass::make_key(const Inst& in, VReg a, VReg b) const {
    Key k;
    k.op = static_cast<uint8_t>(in.op);
    switch (in.op) {
        case IrOp::Bin: k.sub = static_cast<uint8_t>(in.bop); break;
        case IrOp::Un: k.sub = static_cast<uint8_t>(in.uop); break;
        case IrOp::Cmp: k.sub = static_cast<uint8_t>(in.cop); break;
        default: break;
    }
    // Cmp 的 in.ty 恒为 bool，按键要区分的是操作数类型。
    k.tyk = in.op == IrOp::Cmp ? ty_class(cx_.vt(in.a)) : ty_class(in.ty);
    k.a = encode(a);
    k.b = encode(b);
    if (in.op == IrOp::Bin && bin_commutative(in.bop) && k.b < k.a) std::swap(k.a, k.b);
    if (in.op == IrOp::Cmp && cmp_commutative(in.cop) && k.b < k.a) std::swap(k.a, k.b);
    return k;
}

bool CsePass::process(Block& b, State& st, bool transform, int bi) {
    Ctx& cx = cx_;
    std::unordered_map<VReg, VReg> rename;
    std::vector<size_t> dead;
    auto rn = [&](VReg v) {
        if (v == kNoReg) return v;
        for (int guard = 0; guard < 32; ++guard) {
            auto it = rename.find(v);
            if (it == rename.end()) break;
            v = it->second;
        }
        return v;
    };
    for (size_t i = 0; i < b.insts.size(); ++i) {
        Inst& in = b.insts[i];
        VReg a = rn(in.a);
        VReg bb = rn(in.b);
        VReg c = rn(in.c);
        if (transform) {
            in.a = a;
            in.b = bb;
            in.c = c;
            for (VReg& r : in.args) r = rn(r);
        }

        if (in.op == IrOp::StoreSlot && in.idx >= 0) {
            int s = in.idx;
            for (auto it = st.begin(); it != st.end();) {
                const Deps& d = it->second.deps;
                bool hit = d.all || std::binary_search(d.slots.begin(), d.slots.end(), s);
                it = hit ? st.erase(it) : std::next(it);
            }
            continue;
        }

        if (in.dst == kNoReg || in.dst >= cx.nv || cx.ndefs[in.dst] != 1) continue;
        if (!cse_candidate(in)) continue;

        // 所有使用都在本块里才可以搬走定义（否则别的块还引用着旧 vreg）。
        bool local_use = cx.first_use_block[in.dst] < 0 ||
                         (cx.first_use_block[in.dst] == bi && !cx.multi_use[in.dst]);

        Key k = make_key(in, a, bb);
        auto it = st.find(k);
        if (it != st.end() && it->second.v != kNoReg && it->second.v != in.dst && local_use) {
            rename[in.dst] = it->second.v;
            if (transform) dead.push_back(i);
            continue;
        }
        if (it == st.end()) {
            Entry e;
            e.v = in.dst;
            e.deps = merge_deps(a, bb, c);
            st.emplace(std::move(k), std::move(e));
        }
    }
    if (transform && !dead.empty()) {
        std::vector<Inst> kept;
        kept.reserve(b.insts.size() - dead.size());
        size_t di = 0;
        for (size_t i = 0; i < b.insts.size(); ++i) {
            if (di < dead.size() && dead[di] == i) {
                ++di;
                continue;
            }
            kept.push_back(std::move(b.insts[i]));
        }
        b.insts = std::move(kept);
    }
    return !dead.empty();
}

bool CsePass::run() {
    Ctx& cx = cx_;
    if (cx.nb == 0 || cx.nv == 0) return false;

    // 取过槽地址的函数不做：间接写可能改槽，这里的依赖分析覆盖不到。
    for (const auto& b : cx.fn.blocks)
        for (const auto& in : b.insts)
            if (in.op == IrOp::SlotAddr) return false;

    build_index();

    std::vector<std::vector<int>> succ(static_cast<size_t>(cx.nb));
    std::vector<std::vector<int>> pred(static_cast<size_t>(cx.nb));
    for (int i = 0; i < cx.nb; ++i) {
        const auto& insts = cx.fn.blocks[static_cast<size_t>(i)].insts;
        if (insts.empty()) continue;
        const Inst& t = insts.back();
        auto add = [&](int tgt) {
            if (tgt < 0 || tgt >= cx.nb) return;
            succ[static_cast<size_t>(i)].push_back(tgt);
            pred[static_cast<size_t>(tgt)].push_back(i);
        };
        if (t.op == IrOp::Jump) {
            add(t.target);
        } else if (t.op == IrOp::Branch) {
            add(t.target);
            add(t.target2);
        }
    }

    std::vector<State> in(static_cast<size_t>(cx.nb));
    std::vector<State> out(static_cast<size_t>(cx.nb));
    std::vector<char> ready(static_cast<size_t>(cx.nb), 0);

    // 迭代求不动点：块入口 = 所有已处理前驱出口的交集。入口块恒为空。
    ready[0] = 1;
    {
        State st;
        process(cx.fn.blocks[0], st, false, 0);
        out[0] = std::move(st);
    }
    std::deque<int> wl(succ[0].begin(), succ[0].end());
    while (!wl.empty()) {
        int b = wl.front();
        wl.pop_front();
        if (b == 0) continue;
        State st;
        bool first = true;
        for (int p : pred[static_cast<size_t>(b)]) {
            if (!ready[static_cast<size_t>(p)]) continue;
            if (first) {
                st = out[static_cast<size_t>(p)];
                first = false;
                continue;
            }
            State merged;
            for (const auto& [k, e] : st) {
                auto it = out[static_cast<size_t>(p)].find(k);
                if (it != out[static_cast<size_t>(p)].end() && it->second.v == e.v)
                    merged.emplace(k, e);
            }
            st = std::move(merged);
        }
        if (first) continue;  // 前驱都还没处理，等它们
        if (ready[static_cast<size_t>(b)] && same_state(st, in[static_cast<size_t>(b)]))
            continue;
        in[static_cast<size_t>(b)] = st;
        State o = st;
        process(cx.fn.blocks[static_cast<size_t>(b)], o, false, b);
        bool changed = !ready[static_cast<size_t>(b)] ||
                       !same_state(o, out[static_cast<size_t>(b)]);
        ready[static_cast<size_t>(b)] = 1;
        out[static_cast<size_t>(b)] = std::move(o);
        if (changed)
            for (int s : succ[static_cast<size_t>(b)]) wl.push_back(s);
    }

    bool any = false;
    for (int i = 0; i < cx.nb; ++i) {
        if (!ready[static_cast<size_t>(i)]) continue;
        State st = in[static_cast<size_t>(i)];
        if (process(cx.fn.blocks[static_cast<size_t>(i)], st, true, i)) any = true;
    }
    return any;
}

}  // namespace

bool cse_function(IRFunction& fn) {
    CsePass pass(fn);
    return pass.run();
}

}  // namespace optdetail
}  // namespace pyadamas
