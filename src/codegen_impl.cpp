// codegen_impl.cpp —— AsmGen 类体里搬出来的内联函数体
//
// 这些函数原来写在 codegen_impl.h / codegen_core.h / codegen_regalloc.h /
// codegen_frame.h / codegen_tag.h 的类体里（类内内联定义）。改其中一行会连带
// 重编 29 个 include codegen_impl.h 的 TU。搬到这里之后只重编本文件 1 个 TU。
//
// 判据：只搬「冷 / 每函数或每模块只调用几次 / 体较长」的那些。极热的小工具
// （emit2 族、op_* 操作数工厂、mem·smem·slot·src·dst 查表、line/label 发射
// 入口、fresh_label/func_label、move_to/move_fp）仍留在头里内联 —— 没有 LTO，
// 搬走就是真调用。

#include "codegen_impl.h"

namespace pyadamas {

std::string AsmGen::roots_label() const {
    return m_->module_symbol.empty() ? ".Lgroots"
                                     : module_roots_symbol(m_->module_symbol);
}

size_t AsmGen::local_global_count() const {
    size_t n = 0;
    for (auto& g : m_->globals)
        if (!g.external) ++n;
    return n;
}

void AsmGen::instr(MOp op, std::initializer_list<MOperand> ops) {
    MInstr ins;
    ins.op = op;
    ins.ops.assign(ops);
    emit_.out.push_instr(std::move(ins));
}

void AsmGen::cfi(const std::string& s) {
    if (target_abi().gnu_cfi) line(s);
}

void AsmGen::cfi_cfa_offset() {
    if (!target_abi().gnu_cfi) return;
    emit_.line_buf = ".cfi_def_cfa_offset ";
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof buf, frame_.cfa_off);
    emit_.line_buf.append(buf, r.ptr);
    directive(emit_.line_buf);
}

void AsmGen::cfi_restore(const char* reg) {
    if (!target_abi().gnu_cfi) return;
    emit_.line_buf = ".cfi_restore ";
    emit_.line_buf += reg;
    directive(emit_.line_buf);
}

void AsmGen::cfi_offset(const char* reg, int off) {
    if (!target_abi().gnu_cfi) return;
    emit_.line_buf = ".cfi_offset ";
    emit_.line_buf += reg;
    emit_.line_buf += ", -";
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof buf, off);
    emit_.line_buf.append(buf, r.ptr);
    directive(emit_.line_buf);
}

void AsmGen::section_relro() {
    directive(target_abi().gnu_cfi ? ".section .data.rel.ro,\"aw\",@progbits"
                                    : ".section .rdata,\"dr\"");
}

void AsmGen::section_rodata() {
    directive(target_abi().gnu_cfi ? ".section .rodata" : ".section .rdata,\"dr\"");
}

void AsmGen::emit_type_func(const std::string& n) {
    if (target_abi().gnu_cfi) directive(".type " + n + ", @function");
}

void AsmGen::emit_size_func(const std::string& n) {
    if (target_abi().gnu_cfi) directive(std::format(".size {}, .-{}", n, n));
}

void AsmGen::emit_gnu_stack_note() {
    if (target_abi().gnu_cfi) directive(".section .note.GNU-stack,\"\",@progbits");
}

std::string AsmGen::num_label(const char* prefix, int64_t n) {
    std::string s = prefix;
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof buf, n);
    s.append(buf, r.ptr);
    return s;
}

std::string AsmGen::shim_label(int i) {
    std::string s = ".Lshim";
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof buf, i);
    s.append(buf, r.ptr);
    return s;
}

int AsmGen::vreg_off(VReg r) const {
    int s = (r < vreg_slot_.size()) ? vreg_slot_[r] : 0;
    return frame_.out_bytes + 8 * (n_slots_ + s);
}

int AsmGen::rec_off(int i) const {
    int words = 6 + static_cast<int>(frame_.owned.size() + frame_.scan.size());
    return frame_.out_bytes + frame_.locals - 8 * (words - i);
}

std::string AsmGen::dbg_name_label(int fi) const {
    std::string s = ".Lfnname";
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof buf, fi);
    s.append(buf, r.ptr);
    return s;
}

bool AsmGen::is_mem_operand(const std::string& s) {
    return !s.empty() && s[0] != '%' && s[0] != '$';
}

void AsmGen::move_to(const std::string& dst, const std::string& src) {
    if (dst == src) return;
    // mem-to-mem：x86 没有这种编码，中间过一下 %rax。
    if (is_mem_operand(dst) && is_mem_operand(src)) {
        emit2("movq", src, "%rax");
        emit2("movq", "%rax", dst);
        return;
    }
    emit2("movq", src, dst);
}

bool AsmGen::is_mem_mop(const MOperand& o) {
    return o.kind != MOperandKind::Reg && o.kind != MOperandKind::Imm;
}

void AsmGen::move_to(const MOperand& dst, const MOperand& src) {
    if (dst.text == src.text) return;
    if (is_mem_mop(dst) && is_mem_mop(src)) {
        emit2(MOp::kMovq, src, kRax);
        emit2(MOp::kMovq, kRax, dst);
        return;
    }
    emit2(MOp::kMovq, src, dst);
}

void AsmGen::move_fp(const std::string& dst, const std::string& src) {
    if (dst == src) return;
    bool dreg = dst.size() > 4 && dst.compare(0, 4, "%xmm") == 0;
    bool sreg = src.size() > 4 && src.compare(0, 4, "%xmm") == 0;
    // SSE 不支持内存到内存的 movsd：两侧都在栈槽时必须经寄存器中转。
    // 走到这里的调用点都是「把已知值搬到某个槽/寄存器」，%xmm0 是
    // 各指令生成器的固定 scratch，不持有活值。（不中转的话汇编器直接
    // 报 operand size mismatch，整文件汇编失败。）
    if (!dreg && !sreg) {
        emit2("movsd", src, "%xmm0");
        emit2("movsd", "%xmm0", dst);
        return;
    }
    emit2(dreg && sreg ? "movapd" : "movsd", src, dst);
}

void AsmGen::move_fp(const MOperand& dst, const MOperand& src) {
    if (dst.text == src.text) return;
    bool dreg = dst.kind == MOperandKind::Reg && dst.reg >= kXmm0 && dst.reg <= kXmm15;
    bool sreg = src.kind == MOperandKind::Reg && src.reg >= kXmm0 && src.reg <= kXmm15;
    if (!dreg && !sreg) {
        emit2(MOp::kMovsd, src, kXmm0);
        emit2(MOp::kMovsd, kXmm0, dst);
        return;
    }
    emit2(dreg && sreg ? MOp::kMovapd : MOp::kMovsd, src, dst);
}

const MOperand& AsmGen::dst_mop(VReg r) const {
    if (!dst_loc_.empty()) return dst_loc_;
    return mem_mop(r);
}

int AsmGen::char_tab_literal(int idx) const {
    if (!m_ || idx < 0 || idx >= static_cast<int>(m_->strings.size())) return -1;
    const IRStringLit& lit = m_->strings[static_cast<size_t>(idx)];
    if (lit.is_bytes || lit.text.size() != 1) return -1;
    const unsigned char b = static_cast<unsigned char>(lit.text[0]);
    return b < 0x80 ? static_cast<int>(b) : -1;
}

TypePtr AsmGen::ty_of(VReg r) const {
    if (r >= f_->vreg_ty.size()) return t_unknown();
    return f_->vreg_ty[r];
}

int AsmGen::tag_off(int w) const {
    return frame_.out_bytes + 8 * (n_slots_ + n_vreg_slots_ + w);
}

bool AsmGen::is_unk_ty(const TypePtr& t) {
    return !t || t->kind == TyKind::Unknown;
}

bool AsmGen::unk_vreg(VReg r) const {
    return r != kNoReg && r < f_->vreg_ty.size() && is_unk_ty(f_->vreg_ty[r]);
}

bool AsmGen::unk_slot(int s) const {
    return s >= 0 && s < static_cast<int>(f_->slot_ty.size()) &&
           is_unk_ty(f_->slot_ty[static_cast<size_t>(s)]);
}

bool AsmGen::global_tag_eligible(int gi) const {
    if (gi < 0 || gi >= static_cast<int>(m_->globals.size())) return false;
    const IRGlobal& g = m_->globals[static_cast<size_t>(gi)];
    return !g.external && is_unk_ty(g.ty) &&
           (g.symbol.empty() || m_->is_entry);
}

std::string AsmGen::global_tag_label(int gi) const {
    return global_label(gi) + "$t";
}

int64_t AsmGen::static_tag_of(const TypePtr& t) const {
    return pyadamas::static_tag_of(t, tag_.type_tags);
}

bool AsmGen::vreg_tagged(VReg r) const {
    return tag_.tags_enabled && r != kNoReg &&
           r < tag_.vreg_tag.size() && tag_.vreg_tag[r] >= 0;
}

bool AsmGen::slot_tagged(int s) const {
    return tag_.tags_enabled && s >= 0 &&
           s < static_cast<int>(tag_.slot_tag.size()) && tag_.slot_tag[s] >= 0;
}

const MOperand& AsmGen::tag_mop(VReg r) const {
    if (r < tag_.vreg_tag_mem.size() && !tag_.vreg_tag_mem[r].empty())
        return tag_.vreg_tag_mem[r];
    static const MOperand kZero = op_imm(0);
    return kZero;
}

const MOperand& AsmGen::slot_tag_mop(int s) const {
    if (s >= 0 && s < static_cast<int>(tag_.slot_tag_mem.size()) &&
        !tag_.slot_tag_mem[static_cast<size_t>(s)].empty())
        return tag_.slot_tag_mem[static_cast<size_t>(s)];
    static const MOperand kZero = op_imm(0);
    return kZero;
}

std::string AsmGen::tag_src(VReg r) const {
    if (vreg_tagged(r)) return tag_mem(r);
    return "$" + std::to_string(static_tag_of(ty_of(r)));
}

void AsmGen::emit_tag_imm(VReg r, int64_t t) {
    if (vreg_tagged(r)) emit_imm_op("movq", t, tag_mem(r));
}

std::string AsmGen::ptag_label(int i) {
    std::string s = ".Lf";
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof buf, i);
    s.append(buf, r.ptr);
    s += "$pt";
    return s;
}

bool AsmGen::dst_unused(const Inst& in) const {
    return in.dst == kNoReg || (in.dst < static_cast<VReg>(use_count_.size()) &&
                                use_count_[static_cast<size_t>(in.dst)] == 0);
}

}  // namespace pyadamas
