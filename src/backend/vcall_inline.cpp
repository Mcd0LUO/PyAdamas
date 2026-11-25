// 极小整数叶子虚方法的守卫内联。保留 IR VCall 的调用规约与慢路径。
#include "backend/impl.h"

namespace pyadamas {
namespace {

struct IntLeaf {
    enum class Kind { Constant, Field, Add, Sub, Mul } kind;
    int64_t a;
    int64_t b = 0;
};

bool is_int(const TypePtr& ty) { return ty && ty->kind == TyKind::Int; }

std::optional<IntLeaf> int_leaf(const IRFunction& f, const IRClass& cls) {
    if (f.is_generator || f.env_slot >= 0 || f.has_handlers || !f.eh_regions.empty() ||
        f.params.size() != 1 || !f.params[0].ty || f.params[0].ty->kind != TyKind::Class ||
        !is_int(f.ret) || f.blocks.size() != 1)
        return {};
    // 只匹配整块，不跳过任何指令：RC、动态取属性、写入、调用等都会拒绝。
    const auto& body = f.blocks[0].insts;
    if (body.empty() || body.back().op != IrOp::Return) return {};
    const VReg result = body.back().a;
    const auto field = [&](size_t i) -> bool {
        const Inst& self = body[i];
        const Inst& load = body[i+1];
        return self.op == IrOp::LoadSlot && self.dst != kNoReg &&
               self.idx == f.params[0].slot && load.op == IrOp::FieldLoad &&
               load.a == self.dst && load.dst != kNoReg && is_int(load.ty) &&
               load.fwidth == 8 && load.imm >= 8 && load.imm % 8 == 0 &&
               load.imm / 8 <= cls.n_fields;
    };
    if (body.size() == 2 && body[0].op == IrOp::ConstInt && is_int(body[0].ty) &&
        result != kNoReg && result == body[0].dst)
        return IntLeaf{IntLeaf::Kind::Constant, body[0].imm};
    if (body.size() == 3 && field(0) && result == body[1].dst)
        return IntLeaf{IntLeaf::Kind::Field, body[1].imm};
    if (body.size() != 6 || !field(0) || !field(2)) return {};
    const Inst& bin = body[4];
    if (bin.op != IrOp::Bin || !is_int(bin.ty) || result == kNoReg || result != bin.dst ||
        body[1].dst == body[2].dst || body[1].dst == body[3].dst ||
        bin.a != body[1].dst || bin.b != body[3].dst)
        return {};
    switch (bin.bop) {
        case BinKind::Add: return IntLeaf{IntLeaf::Kind::Add, body[1].imm, body[3].imm};
        case BinKind::Sub: return IntLeaf{IntLeaf::Kind::Sub, body[1].imm, body[3].imm};
        case BinKind::Mul: return IntLeaf{IntLeaf::Kind::Mul, body[1].imm, body[3].imm};
        default: return {};
    }
}

}  // namespace

// 虚调用：self 只装入 ABI 的第一个整数实参寄存器一次，随后从中读
// vtable。槽地址直接作为间接 call 的内存操作数，不另取函数指针。
// direct_fn >= 0：守卫命中路径的直调版本（INLINE_OPT P2）——装填/spill/
// 结果落盘与间接版完全一致，只有 call 目标从虚表槽换成函数入口标签。
void AsmGen::gen_vcall(const Inst& in, bool keep_int_result, int direct_fn) {
    std::vector<TypePtr> types;
    types.reserve(in.args.size() + 1);
    types.push_back(t_unknown());  // self：指针，非浮点
    for (VReg a : in.args) types.push_back(ty_of(a));
    auto locs = classify(types);

    for (size_t i = 0; i < in.args.size(); ++i) {
        const auto& l = locs[i + 1];
        if (l.stack_off < 0) continue;
        emit2(MOp::kMovq, src_mop(in.args[i]), kR10);
        emit2(MOp::kMovq, kR10, op_mem(kRsp, l.stack_off));
    }
    move_to(mreg_operand(mreg_from_name(arg_reg_i(0))), src_mop(in.a));
    emit2(MOp::kMovq, op_mem(mreg_from_name(arg_reg_i(0))), kR11);
    for (size_t i = 0; i < in.args.size(); ++i) {
        const auto& l = locs[i + 1];
        if (l.stack_off >= 0) continue;
        if (l.fp)
            emit2(MOp::kMovsd, src_mop(in.args[i]), static_cast<MReg>(kXmm0 + l.reg));
        else
            emit2(MOp::kMovq, src_mop(in.args[i]), arg_reg_i(l.reg));
    }
    line("xorl %r10d, %r10d");
    // P2：虚调用点没有可信的静态签名 → 侧信道写 0（被调回退 UNK）。
    if (tag_.type_tags) {
        line("movq $0, pa_ptag_in(%rip)");
        line("movq $0, pa_ptag_len(%rip)");
    }
    if (opt_ < 2) line("xorl %eax, %eax");
    spill_fp_slots();
    if (direct_fn >= 0) {
        line("call " + entry_ref(direct_fn));
    } else {
        MOperand vt = op_mem(kR11, in.imm * 8); vt.indirect = true;
        emit1(MOp::kCall, vt);
    }
    reload_fp_slots();

    if (!keep_int_result && !dst_unused(in)) {
        if (ir_is_float(in.ty)) move_fp(dst_mop(in.dst), mreg_operand(kXmm0));
        else emit2(MOp::kMovq, kRax, dst_mop(in.dst));
    }
}

// 只融合已存在的整数累加形态。LHS 是调用前的快照，不能在慢调用之后
// 再读全局/槽（方法可能已经改了它）。EH 和调用 ABI 照旧。
// 目标既可以是全局，也可以是帧槽：入口模块私有标量全局提升会把
// `total = total + s.area()` 从 store.global 改写成 store.slot，
// 这里走与 Bin+StoreSlot 融合相同的判据，保住「结果留在 %rax」。
bool AsmGen::try_fuse_guarded_vcall_add(const Block& b, size_t pos) {
    if (opt_ < 2 || pos + 3 >= b.insts.size()) return false;
    const auto& call = b.insts[pos];
    const auto& add = b.insts[pos + 1];
    const auto& store = b.insts[pos + 2];
    const bool to_global = store.op == IrOp::StoreGlobal;
    const bool to_slot = store.op == IrOp::StoreSlot;
    if (!to_global && !to_slot) return false;
    if (call.op != IrOp::VCall || !call.args.empty() || !is_int(call.ty) ||
        !single_use_in_block(call.dst) || add.op != IrOp::Bin ||
        add.bop != BinKind::Add || !is_int(add.ty) || add.b != call.dst ||
        !is_int(ty_of(add.a)))
        return false;
    if (to_global) {
        if (!can_fuse_global(add, store)) return false;
    } else {
        // 与 Bin+StoreSlot 融合同一套判据（GC 根槽 / tag / 寄存器类 / 单使用）。
        if (!can_fuse(add, store)) return false;
        if (store.idx < 0 || store.idx >= static_cast<int>(ra_.slot_reg.size()))
            return false;
    }
    const std::string lhs = src_op(add.a);
    // LHS 是调用前的快照，调用之后必须还能读：只接受 callee-saved 寄存器
    // 或栈槽。槽提升后的 `load.slot total` 会别名到 callee-saved 槽寄存器
    // （%rbx/%rbp/%r12-%r15），安全；caller-saved 会被调用破坏。
    {
        static const char* kCaller[] = {"%rax", "%rcx", "%rdx", "%rsi", "%rdi",
                                        "%r8", "%r9", "%r10", "%r11"};
        for (const char* r : kCaller)
            if (lhs.find(r) != std::string::npos) return false;
    }
    set_loc(call.line);
    if (!gen_guarded_vcall(call, true)) return false;
    set_loc(add.line);
    comment(add.text() + " -> " + store.text());
    emit2("addq", lhs, "%rax");
    if (to_global) {
        emit2("movq", "%rax", global_label(store.idx) + "(%rip)");
    } else {
        emit2("movq", "%rax", slot_op(store.idx));
        invalidate_slot(store.idx);
    }
    return true;
}

bool AsmGen::gen_guarded_vcall(const Inst& in, bool keep_int_result) {
    if (opt_ < 2 || in.imm < 0) return false;
    // 叶子路径只接零参整数返回；INLINE_OPT 开启时带实参/非整数返回的调用
    // 允许走 P2 守卫直调（非叶子小方法），叶子候选仍按旧判据收集。
    const bool leaf_ok = in.args.empty() && is_int(in.ty);
    if (!inline_opt_ && !leaf_ok) return false;
    TypePtr recv = ty_of(in.a);
    if (!recv || recv->kind != TyKind::Class || !recv->cls || recv->cls->imported())
        return false;
    int base = -1;
    for (int i = 0; i < static_cast<int>(m_->classes.size()); ++i) {
        if (m_->classes[i].name != recv->cls->name) continue;
        if (base >= 0) return false;
        base = i;
    }
    if (base < 0) return false;

    struct Candidate { int cls; int function; IntLeaf leaf; int delta; };
    std::vector<Candidate> candidates;
    const size_t slot = static_cast<size_t>(in.imm);
    for (int i = 0; i < static_cast<int>(m_->classes.size()); ++i) {
        const IRClass& c = m_->classes[i];
        if (i != base && std::find(c.mro.begin(), c.mro.end(), base) == c.mro.end()) continue;
        if (slot >= c.vtable.size() || (slot < c.vtable_ext.size() && !c.vtable_ext[slot].empty()))
            continue;
        int fn = c.vtable[slot];
        if (fn < 0 || static_cast<size_t>(fn) >= m_->funcs.size()) continue;
        if (!leaf_ok) continue;
        // 多继承的槽里放的是 adjustor thunk（0.509）：守卫要比较 thunk 入口
        // （运行期槽里就是它），内联体取 thunk 目标的叶子，命中后先补回
        // delta 让 self 指向目标基类的字段块。
        int body_fn = fn;
        int delta = 0;
        const IRFunction& slot_fn = *m_->funcs[static_cast<size_t>(fn)];
        if (slot_fn.mi_thunk && slot_fn.mi_thunk_target >= 0 &&
            static_cast<size_t>(slot_fn.mi_thunk_target) < m_->funcs.size()) {
            body_fn = slot_fn.mi_thunk_target;
            delta = slot_fn.mi_thunk_delta;
        }
        if (auto leaf = int_leaf(*m_->funcs[static_cast<size_t>(body_fn)], c)) {
            // 小型多态点才值得展开；不对大层次堆一长串守卫。
            if (candidates.size() == 4) return false;
            candidates.push_back({i, fn, *leaf, delta});
        }
    }
    if (candidates.empty()) {
        if (inline_opt_) return gen_guarded_vcall_direct(in, keep_int_result, base, slot);
        return false;
    }

    if (keep_int_result) comment(in.text() + " -> guarded return in %rax");
    const std::string self = arg_reg_i(0);
    move_to(self, src_op(in.a));
    // 静态类型只用来找候选。守卫比较**实际实现指针**（vtable 槽里存的值）：
    // 慢路径的 `call *slot(%r11)` 目标就是它，所以指针相同 ⇒ 慢路径会调用
    // 同一个函数 ⇒ 内联体（由该函数的 IR 生成）与慢路径逐条等价。
    //
    // 按实现指针去重是安全的关键：Square 继承 Rect.area 时两个类的 vtable 槽
    // 指向同一个入口（.Lvt1 / .Lvt2 的第一个字都是 .Lf5），内联体也完全一样
    // （字段偏移来自函数自身的 IR，与接收者的具体类无关；子类布局以基类字段
    // 打头，偏移不变）——所以一次比较就够，不必再比 vtable 地址。
    // 覆写方法在槽里是另一个入口，自然落到慢路径；多继承的每个基方法仍是
    // 各自入口，按指针分组后每组比较一次，命中任一指针对应的类都走同一份内联体。
    line(std::format("movq ({}), %r11", self));
    line(std::format("movq {}(%r11), %r11", in.imm * 8));
    const std::string done = fresh_label("vleafdone");
    // 比较顺序：按「该实现指针覆盖几个候选类」降序。没有 profile 数据时，
    // 覆盖 k 个类的实现被命中的先验是 k 倍（候选类等概率），先比它能让
    // 期望比较次数最少。对 Square 继承 Rect.area 的形态就是「先比合并组」。
    std::vector<std::pair<int, int>> groups;  // {-覆盖类数, 实现函数}
    std::vector<int> gidx;                    // 首次出现顺序的去重表
    for (const auto& c : candidates) {
        auto it = std::find(gidx.begin(), gidx.end(), c.function);
        if (it == gidx.end()) {
            gidx.push_back(c.function);
            groups.push_back({-1, c.function});
        } else {
            groups[static_cast<size_t>(it - gidx.begin())].first -= 1;
        }
    }
    std::stable_sort(groups.begin(), groups.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::unordered_map<int, std::string> targets;
    for (const auto& g : groups) {
        const int fn = g.second;
        auto [it, inserted] = targets.try_emplace(fn);
        if (inserted) it->second = fresh_label("vleafhit");
        line(std::format("leaq {}(%rip), %r10", entry_ref(fn)));
        line("cmpq %r10, %r11");
        emit_jmp("je", it->second);
    }
    gen_vcall(in, keep_int_result);
    emit_jmp("jmp", done);
    for (const auto& c : candidates) {
        auto hit = targets.find(c.function);
        if (hit == targets.end()) continue;
        label(hit->second);
        targets.erase(hit);
        const auto& leaf = c.leaf;
        // thunk 命中：self 还指向子类头部，先挪到目标基类的字段块起点，
        // 叶子体里的字段偏移就是相对那个块算的。
        if (c.delta != 0 && leaf.kind != IntLeaf::Kind::Constant)
            line(std::format("addq ${}, {}", c.delta, self));
        if (!dst_unused(in)) {
            if (leaf.kind == IntLeaf::Kind::Constant) {
                emit_imm_op("movq", leaf.a, "%rax");
            } else {
                line(std::format("movq {}({}), %rax", leaf.a, self));
                if (leaf.kind != IntLeaf::Kind::Field) {
                    const char* op = leaf.kind == IntLeaf::Kind::Add ? "addq" :
                                     leaf.kind == IntLeaf::Kind::Sub ? "subq" : "imulq";
                    line(std::format("{} {}({}), %rax", op, leaf.b, self));
                }
            }
            if (!keep_int_result) move_to(dst_op(in.dst), "%rax");
        }
        emit_jmp("jmp", done);
    }
    label(done);
    return true;
}


// INLINE_OPT P2：非叶子小方法的守卫直调。守卫比较**实际虚表地址**（与叶子
// 路径同构，不按类名/静态类型断言），命中 → call .LfN 直调（省一级虚表装载
// + 间接分支），失败回退原 vcall —— 运行期子类/替换类全覆盖，语义不变。
// 只接 monomorphic/dimorphic 调用点（≤2 候选）：方法体 ≤32 条指令、≤4 块、
// 无 handler/EH 区域/生成器/闭包环境。慢路径原样保留；整个守卫序列发射在
// VCall 指令位置，EH 区域标签（EhBegin/EhEnd）覆盖全部快/慢路径调用。
bool AsmGen::gen_guarded_vcall_direct(const Inst& in, bool keep_int_result, int base,
                                      size_t slot) {
    if (in.args.size() > 2) return false;
    struct Cand { int cls; int fn; };
    std::vector<Cand> cands;
    for (int i = 0; i < static_cast<int>(m_->classes.size()); ++i) {
        const IRClass& c = m_->classes[i];
        if (i != base && std::find(c.mro.begin(), c.mro.end(), base) == c.mro.end())
            continue;
        if (slot >= c.vtable.size() || (slot < c.vtable_ext.size() && !c.vtable_ext[slot].empty()))
            continue;
        int fn = c.vtable[slot];
        if (fn < 0 || static_cast<size_t>(fn) >= m_->funcs.size()) continue;
        const IRFunction& f = *m_->funcs[static_cast<size_t>(fn)];
        if (f.is_generator || f.env_slot >= 0 || f.has_handlers || !f.eh_regions.empty())
            continue;
        if (f.params.size() != in.args.size() + 1 || f.blocks.empty()) continue;
        int n = 0;
        for (const auto& b : f.blocks) n += static_cast<int>(b.insts.size());
        if (n > 32 || f.blocks.size() > 4) continue;
        // 超过 2 个候选说明是多态热点，守卫链收益不抵代码增长，保守放弃。
        if (cands.size() == 2) return false;
        cands.push_back({i, fn});
    }
    if (cands.empty()) return false;

    comment(in.text() + " -> guarded direct call");
    const std::string self = arg_reg_i(0);
    move_to(self, src_op(in.a));
    // 与叶子路径同构：比较 vtable 槽里的实现指针，同一函数被多个类继承时
    // 只比较一次（内联/直调体按实现指针分组，命中任一指针走同一份代码）。
    line(std::format("movq ({}), %r11", self));
    line(std::format("movq {}(%r11), %r11", in.imm * 8));
    const std::string done = fresh_label("vdcallend");
    std::unordered_map<int, std::string> targets;
    for (const auto& c : cands) {
        auto [it, inserted] = targets.try_emplace(c.fn);
        if (inserted) it->second = fresh_label("vdcallhit");
        if (!inserted) continue;
        line(std::format("leaq {}(%rip), %r10", entry_ref(c.fn)));
        line("cmpq %r10, %r11");
        emit_jmp("je", it->second);
    }
    gen_vcall(in, keep_int_result);
    emit_jmp("jmp", done);
    for (const auto& c : cands) {
        auto hit = targets.find(c.fn);
        if (hit == targets.end()) continue;
        label(hit->second);
        targets.erase(hit);
        gen_vcall(in, keep_int_result, c.fn);
        emit_jmp("jmp", done);
    }
    label(done);
    return true;
}

}  // namespace pyadamas
