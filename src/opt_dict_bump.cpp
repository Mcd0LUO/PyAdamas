// opt_dict_bump.cpp —— `d[k] = d.get(k, def) + delta` 融合
//
// 词频统计 / 计数器最经典的写法。IR 里实际是 7 条指令：
//     %a = call.rt pa_dict_get_or(%d1, %k1, def)
//     %b = bin.add %a, delta
//     %d2 = load.slot <dict>
//     %k2 = load.slot <key>
//     call.rt pa_rc_inc(%k2)
//     call.rt pa_rc_inc_unknown(%b)
//     call.rt pa_dict_set(%d2, %k2, %b)
//
// **注意 %d1/%k1 与 %d2/%k2 是不同的 vreg**（中间重新 load 了槽），但
// 它们来自同一个槽，值相同。所以匹配要比槽号，不能比 vreg 号。
//
// get 和 set 各自算一次哈希 + 一次开放寻址探测，同一个 key 连续做两遍。
// chash 缓存之后哈希便宜了，但 dict_lookup 的探测仍要做两遍。
//
// 融合成一次 `pa_dict_bump(d, k, def, delta)`：一次哈希、一次查找。
//
// **正确性**：只匹配同一块内**紧邻**的这 7 条指令，且：
//   - %d1 与 %d2 来自同一个槽（同一个 dict 对象）；
//   - %k1 与 %k2 来自同一个槽（同一个 key）；
//   - %a / %b 在整个函数里只被这几条读写。
#include "opt_detail.h"

namespace pyadamas {
namespace optdetail {

namespace {

bool is_int_or_unknown(const TypePtr& t) {
    if (!t) return true;
    return t->kind == TyKind::Int || t->kind == TyKind::Bool ||
           t->kind == TyKind::Unknown;
}

bool only_used_by(const IRFunction& fn, VReg v, const Inst* const* allowed, int n) {
    if (v == kNoReg) return true;
    for (const auto& b : fn.blocks) {
        for (const auto& in : b.insts) {
            bool ok = false;
            for (int i = 0; i < n; ++i)
                if (allowed[i] == &in) { ok = true; break; }
            if (ok) continue;
            if (in.dst == v) return false;
            if (in.a == v || in.b == v || in.c == v) return false;
            for (VReg r : in.args)
                if (r == v) return false;
        }
    }
    return true;
}

bool is_rt(const Inst& in, RtFn f) {
    return in.op == IrOp::CallRt && in.imm == static_cast<int64_t>(f);
}

// 两个 vreg 是否指向同一个槽（或直接相等）。
// 两个 vreg 是否指向同一个来源：同槽（LoadSlot）/ 同全局（LoadGlobal）/ 同字符串
// 常量（ConstStr）。模块级代码（__pa_main）里 dict 与 key 通常是全局/常量，
// 只认 LoadSlot 会让模块级计数器永远匹配不上。
bool same_slot(const IRFunction& fn, VReg x, VReg y) {
    if (x == y) return true;
    auto origin_of = [&](VReg v) -> int64_t {
        if (v == kNoReg) return -1;
        for (const auto& b : fn.blocks)
            for (const auto& in : b.insts)
                if (in.dst == v) {
                    if (in.op == IrOp::LoadSlot)
                        return (int64_t{1} << 40) | static_cast<int64_t>(in.idx);
                    if (in.op == IrOp::LoadGlobal)
                        return (int64_t{2} << 40) | static_cast<int64_t>(in.idx);
                    if (in.op == IrOp::ConstStr)
                        return (int64_t{3} << 40) | static_cast<int64_t>(in.idx);
                    return -1;
                }
        return -1;
    };
    const int64_t a = origin_of(x);
    const int64_t c = origin_of(y);
    return a >= 0 && a == c;
}

}  // namespace

bool fuse_dict_bump(IRFunction& fn) {
    bool changed = false;
    for (auto& b : fn.blocks) {
        auto& ins = b.insts;
        for (size_t i = 0; i + 4 < ins.size();) {
            Inst& get = ins[i];
            if (!is_rt(get, RtFn::DictGetOr) || get.args.size() != 3) {
                ++i;
                continue;
            }
            // get 与 add 之间允许夹一条纯常量/搬运/装载（`+ 1` 的常量常在 get
            // 之后才 materialize）。
            size_t mid_i = SIZE_MAX;
            size_t add_i = i + 1;
            {
                const Inst& cand = ins[add_i];
                const bool bin_add = cand.op == IrOp::Bin && cand.bop == BinKind::Add;
                const bool pure_prefix =
                    cand.op == IrOp::ConstInt || cand.op == IrOp::ConstFloat ||
                    cand.op == IrOp::ConstStr || cand.op == IrOp::Mov ||
                    cand.op == IrOp::LoadSlot || cand.op == IrOp::LoadGlobal;
                if (!bin_add && pure_prefix && add_i + 1 < ins.size()) {
                    mid_i = add_i;
                    add_i = add_i + 1;
                }
            }
            if (add_i + 2 >= ins.size()) {
                ++i;
                continue;
            }
            Inst& add = ins[add_i];
            if (add.op != IrOp::Bin || add.bop != BinKind::Add || add.a != get.dst) {
                ++i;
                continue;
            }
            // add 之后：dict 装载（bump 需要它的 vreg），然后 key 装载**可选**
            // （LICM 常把循环不变的 key 装载外提到 preheader，此时 get/set/rc_inc
            // 用的是同一个 vreg，body 里没有 key 装载），再 rc_inc(key)，再 set。
            Inst& ld1 = ins[add_i + 1];
            if (!(ld1.op == IrOp::LoadSlot || ld1.op == IrOp::LoadGlobal)) {
                ++i;
                continue;
            }
            size_t ld2_i = SIZE_MAX;
            size_t inck_i = add_i + 2;
            {
                const Inst& cand = ins[inck_i];
                const bool key_load =
                    cand.op == IrOp::LoadSlot || cand.op == IrOp::LoadGlobal ||
                    cand.op == IrOp::ConstStr;
                if (key_load && inck_i + 1 < ins.size()) {
                    ld2_i = inck_i;
                    inck_i = inck_i + 1;
                }
            }
            if (inck_i + 1 >= ins.size()) {
                ++i;
                continue;
            }
            Inst& inck = ins[inck_i];
            size_t incv_i = SIZE_MAX;
            size_t set_i = inck_i + 1;
            if (set_i < ins.size()) {
                Inst& cand = ins[set_i];
                if ((is_rt(cand, RtFn::RcIncUnknown) || is_rt(cand, RtFn::RcInc)) &&
                    cand.args.size() == 1 && cand.args[0] == add.dst) {
                    incv_i = set_i;
                    set_i = set_i + 1;
                }
            }
            if (set_i >= ins.size()) {
                ++i;
                continue;
            }
            Inst& set = ins[set_i];
            const bool match =
                is_int_or_unknown(add.ty) &&
                is_rt(inck, RtFn::RcInc) && inck.args.size() == 1 &&
                is_rt(set, RtFn::DictSet) && set.args.size() == 3 &&
                set.args[2] == add.dst &&
                same_slot(fn, get.args[0], set.args[0]) &&
                same_slot(fn, get.args[1], set.args[1]) &&
                same_slot(fn, inck.args[0], set.args[1]);
            if (!match) {
                ++i;
                continue;
            }

            const bool has_incv = incv_i != SIZE_MAX;
            std::vector<const Inst*> allowed;
            allowed.push_back(&get);
            allowed.push_back(&add);
            allowed.push_back(&ld1);
            allowed.push_back(&inck);
            allowed.push_back(&set);
            if (ld2_i != SIZE_MAX) allowed.push_back(&ins[ld2_i]);
            if (has_incv) allowed.push_back(&ins[incv_i]);
            if (mid_i != SIZE_MAX) allowed.push_back(&ins[mid_i]);
            if (!only_used_by(fn, get.dst, allowed.data(), static_cast<int>(allowed.size())) ||
                !only_used_by(fn, add.dst, allowed.data(), static_cast<int>(allowed.size()))) {
                ++i;
                continue;
            }

            // bump 直接用 get 的 key vreg（可能来自 preheader），不再依赖 body 内
            // 的 key 装载；dict vreg 用 ld1（bump 需要它已定义）。
            Inst bump;
            bump.op = IrOp::CallRt;
            bump.imm = static_cast<int64_t>(RtFn::DictBump);
            bump.args = {ld1.dst, get.args[1], get.args[2], add.b};
            bump.ty = nullptr;
            std::vector<Inst> keep;
            if (mid_i != SIZE_MAX) keep.push_back(ins[mid_i]);
            keep.push_back(ld1);
            keep.push_back(bump);
            ins.erase(ins.begin() + static_cast<std::ptrdiff_t>(i),
                      ins.begin() + static_cast<std::ptrdiff_t>(set_i + 1));
            ins.insert(ins.begin() + static_cast<std::ptrdiff_t>(i), keep.begin(), keep.end());
            changed = true;
            i += keep.size();
        }
    }
    return changed;
}

}  // namespace optdetail
}  // namespace pyadamas
