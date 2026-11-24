// opt/dict_bump.cpp —— `d[k] = d.get(k, def) + delta` 融合
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
#include "opt/detail.h"

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
bool same_slot(const IRFunction& fn, VReg x, VReg y) {
    if (x == y) return true;
    auto slot_of = [&](VReg v) -> int {
        if (v == kNoReg) return -1;
        for (const auto& b : fn.blocks)
            for (const auto& in : b.insts)
                if (in.dst == v && in.op == IrOp::LoadSlot) return static_cast<int>(in.idx);
        return -1;
    };
    const int a = slot_of(x);
    const int c = slot_of(y);
    return a >= 0 && a == c;
}

}  // namespace

bool fuse_dict_bump(IRFunction& fn) {
    bool changed = false;
    for (auto& b : fn.blocks) {
        auto& ins = b.insts;
        for (size_t i = 0; i + 5 < ins.size();) {
            Inst& get = ins[i];
            Inst& add = ins[i + 1];
            Inst& ld1 = ins[i + 2];
            Inst& ld2 = ins[i + 3];
            Inst& inck = ins[i + 4];
            // 「值的 retain」是可选的：值类型是 unknown 时是 RcIncUnknown，
            // 是 int 时根本没有 retain（int 不持有引用），dict.get 缺省值
            // 推断后计数器的值类型就是 int —— 两种形态都要能融合。
            size_t incv_i = SIZE_MAX;
            size_t set_i = i + 5;
            if (i + 6 < ins.size()) {
                Inst& cand = ins[i + 5];
                if ((is_rt(cand, RtFn::RcIncUnknown) || is_rt(cand, RtFn::RcInc)) &&
                    cand.args.size() == 1 && cand.args[0] == add.dst) {
                    incv_i = i + 5;
                    set_i = i + 6;
                }
            }
            if (set_i >= ins.size()) {
                ++i;
                continue;
            }
            Inst& set = ins[set_i];

            const bool match =
                is_rt(get, RtFn::DictGetOr) && get.args.size() == 3 &&
                add.op == IrOp::Bin && add.bop == BinKind::Add && add.a == get.dst &&
                is_int_or_unknown(add.ty) &&
                ld1.op == IrOp::LoadSlot && ld2.op == IrOp::LoadSlot &&
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
            const Inst* allowed[7] = {&get, &add, &ld1, &ld2, &inck, &set, nullptr};
            int nallowed = 6;
            if (has_incv) {
                allowed[5] = &ins[incv_i];
                allowed[6] = &set;
                nallowed = 7;
            }
            if (!only_used_by(fn, get.dst, allowed, nallowed) ||
                !only_used_by(fn, add.dst, allowed, nallowed)) {
                ++i;
                continue;
            }

            // **保留 ld1/ld2**：它们定义了 bump 要用的 dict/key vreg。
            // 第一版把它们一起删了，bump.args 引用未定义 vreg -> SIGSEGV。
            Inst bump;
            bump.op = IrOp::CallRt;
            bump.imm = static_cast<int64_t>(RtFn::DictBump);
            bump.args = {ld1.dst, ld2.dst, get.args[2], add.b};
            bump.ty = nullptr;
            // 原地改：i 位置换成 ld1，i+1 换成 ld2，i+2 换成 bump
            // 这样不用 erase/insert，索引稳定。
            Inst keep1 = std::move(ld1);
            Inst keep2 = std::move(ld2);
            ins[i] = std::move(keep1);
            ins[i + 1] = std::move(keep2);
            ins[i + 2] = std::move(bump);
            const size_t erase_end = has_incv ? i + 7 : i + 6;
            ins.erase(ins.begin() + static_cast<std::ptrdiff_t>(i + 3),
                      ins.begin() + static_cast<std::ptrdiff_t>(erase_end));
            changed = true;
            i += 3;
        }
    }
    return changed;
}

}  // namespace optdetail
}  // namespace pyadamas
