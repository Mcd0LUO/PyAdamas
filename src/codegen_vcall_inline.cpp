// 极小整数叶子虚方法的守卫内联。保留 IR VCall 的调用规约与慢路径。
#include "codegen_impl.h"

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

// 只融合已存在的整数全局累加形态。LHS 是调用前的快照，不能
// 在慢调用之后读全局（方法可能已经更改它）。EH 和调用 ABI 照旧。
bool AsmGen::try_fuse_guarded_vcall_add(const Block& b, size_t pos) {
    if (opt_ < 2 || pos + 3 >= b.insts.size()) return false;
    const auto& call = b.insts[pos];
    const auto& add = b.insts[pos + 1];
    const auto& store = b.insts[pos + 2];
    if (call.op != IrOp::VCall || !call.args.empty() || !is_int(call.ty) ||
        !single_use_in_block(call.dst) || add.op != IrOp::Bin ||
        add.bop != BinKind::Add || !is_int(add.ty) || add.b != call.dst ||
        !is_int(ty_of(add.a)) || alias_.contains(add.a) || !can_fuse_global(add, store))
        return false;
    const std::string lhs = src_op(add.a);
    if (lhs.find("%rax") != std::string::npos) return false;
    set_loc(call.line);
    if (!gen_guarded_vcall(call, true)) return false;
    set_loc(add.line);
    comment(add.text() + " -> " + store.text());
    emit2("addq", lhs, "%rax");
    emit2("movq", "%rax", global_label(store.idx) + "(%rip)");
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

    struct Candidate { int cls; int function; IntLeaf leaf; };
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
        if (auto leaf = int_leaf(*m_->funcs[fn], c)) {
            // 小型多态点才值得展开；不对大层次堆一长串守卫。
            if (candidates.size() == 4) return false;
            candidates.push_back({i, fn, *leaf});
        }
    }
    if (candidates.empty()) {
        if (inline_opt_) return gen_guarded_vcall_direct(in, keep_int_result, base, slot);
        return false;
    }

    if (keep_int_result) comment(in.text() + " -> guarded return in %rax");
    const std::string self = arg_reg_i(0);
    move_to(self, src_op(in.a));
    line(std::format("movq ({}), %r11", self));
    const std::string done = fresh_label("vleafdone");
    std::unordered_map<int, std::string> targets;
    // 静态类型只用来找候选。必须比较实际虚表地址，不按类名或静态类型断言；
    // 同一局部方法被多个类继承时，共用内联代码但每个虚表仍要单独匹配。
    for (const auto& c : candidates) {
        auto [it, inserted] = targets.try_emplace(c.function);
        if (inserted) it->second = fresh_label("vleafhit");
        line(std::format("leaq .Lvt{}(%rip), %r10", c.cls));
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
    line(std::format("movq ({}), %r11", self));
    const std::string done = fresh_label("vdcallend");
    // 同一函数被多个类继承时共享命中标签，但每个虚表仍单独比较。
    std::unordered_map<int, std::string> targets;
    for (const auto& c : cands) {
        auto [it, inserted] = targets.try_emplace(c.fn);
        if (inserted) it->second = fresh_label("vdcallhit");
        line(std::format("leaq .Lvt{}(%rip), %r10", c.cls));
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
