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

bool AsmGen::gen_guarded_vcall(const Inst& in) {
    if (opt_ < 2 || !in.args.empty() || !is_int(in.ty) || in.imm < 0) return false;
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
        if (auto leaf = int_leaf(*m_->funcs[fn], c)) {
            // 小型多态点才值得展开；不对大层次堆一长串守卫。
            if (candidates.size() == 4) return false;
            candidates.push_back({i, fn, *leaf});
        }
    }
    if (candidates.empty()) return false;

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
    gen_vcall(in);
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
            move_to(dst_op(in.dst), "%rax");
        }
        emit_jmp("jmp", done);
    }
    label(done);
    return true;
}

}  // namespace pyadamas
