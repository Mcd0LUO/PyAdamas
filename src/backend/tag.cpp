// backend/tag.cpp —— P3 shadow tag 发射（0.327，docs/p3-tagged-union-design.md）
//
// 从 backend/call_rt.cpp 拆出（文件规模硬限制 1200 行）：tag 版动态算子
// 装填/双字返回落盘、复用匹配、unknown RC tag 内联、堆标志导出、下标
// 装载/写入的 tag 路由。门控 tag_.tags_enabled 关时这些函数不被调用。
#include "backend/impl.h"

namespace pyadamas {

// ── P3 shadow tag：tag 版动态算子发射（0.327，设计 §3/§4）─────────────
//
// 降级器仍发旧 AnyArith/AnyCmp/AnyNeg（IR 实参 {op,a,b,desc_a,desc_b} /
// {a,desc}）；门控内这里拦截改发 pa_any_*_t：tag 从平行 tag 区/静态类型
// 装填，desc 不再需要（tag  subsumes it；UNK tag 运行时回退探针）。
// AnyArithT/AnyNegT 返回 PaAnyPair：SysV {i64,i64} → %rax=payload、
// %rdx=tag，分别落 dst 的 payload 槽与 tag 字。

void AsmGen::emit_call_dst_tag(const Inst& in) {
    if (!vreg_tagged(in.dst) || dst_unused(in)) return;
    int64_t t = kTagUnk;
    if (in.op == IrOp::CallRt) {
        RtFn fn = static_cast<RtFn>(in.imm);
        if (fn == RtFn::AnyFromF64) t = kTagFbox;
        else if (fn == RtFn::StrAny) t = kTagStr;
        else if (fn == RtFn::DictGet || fn == RtFn::DictGetOr ||
                 fn == RtFn::DictGetOpt || fn == RtFn::DictPop ||
                 fn == RtFn::DictPopLast) {
            // P3-1b：dict 值无 tag 来源 → 一次探针导 tag，后续 RC/算术
            // 免探针（比「每次使用都探针」少；设计 §3.3 取出侧同款）。
            emit_tag_probe(dst_op(in.dst), tag_mem(in.dst));
            return;
        }
    }
    emit_tag_imm(in.dst, t);
}

void AsmGen::emit_tag_probe(const std::string& payload_op,
                            const std::string& tag_op) {
    emit2("movq", payload_op, arg_reg_i(0));
    spill_fp_slots();
    line("call pa_any_load_tag");
    reload_fp_slots();
    emit2("movq", "%rax", tag_op);
}

// 复用匹配（设计 §4）：结果 dst 被存回 S（同块 StoreSlot/StoreGlobal），
// 且某实参恰是同块对 S 的装载、单使用（只被本算子读）、装载到存储之间
// 无对 S 的其它写入 → 该实参就是「即将被覆盖的槽值」，可作复用提示。
// 运行时再验 tag==FBOX && rc==1（唯一所有权）才原位改写。
int64_t AsmGen::any_reuse_ok(const Inst& in) {
    if (!tag_.tags_enabled || opt_ < 1 || cur_block_ < 0 ||
        cur_block_ >= static_cast<int>(f_->blocks.size()))
        return 0;
    const Block& b = f_->blocks[static_cast<size_t>(cur_block_)];
    int store_pos = -1, store_idx = -1;
    bool store_global = false;
    for (int j = cur_pos_ + 1; j < static_cast<int>(b.insts.size()); ++j) {
        const Inst& nx = b.insts[static_cast<size_t>(j)];
        if (nx.op == IrOp::StoreSlot && nx.a == in.dst) {
            store_pos = j; store_idx = nx.idx; break;
        }
        if (nx.op == IrOp::StoreGlobal && nx.a == in.dst) {
            store_pos = j; store_idx = nx.idx; store_global = true; break;
        }
        if (nx.op == IrOp::Jump || nx.op == IrOp::Branch ||
            nx.op == IrOp::Return || nx.op == IrOp::GenYield ||
            nx.op == IrOp::GenAwait || nx.op == IrOp::GenDone)
            break;
    }
    if (store_pos < 0) return 0;
    auto check = [&](VReg v) -> bool {
        if (v == kNoReg || v >= use_count_.size() || use_count_[v] != 1)
            return false;
        int dp = -1;
        for (int j = cur_pos_ - 1; j >= 0; --j)
            if (b.insts[static_cast<size_t>(j)].dst == v) { dp = j; break; }
        if (dp < 0) return false;
        const Inst& def = b.insts[static_cast<size_t>(dp)];
        bool ok = store_global
            ? (def.op == IrOp::LoadGlobal && def.idx == store_idx)
            : (def.op == IrOp::LoadSlot && def.idx == store_idx);
        if (!ok) return false;
        for (int j = dp + 1; j < store_pos; ++j) {
            const Inst& mx = b.insts[static_cast<size_t>(j)];
            if ((mx.op == IrOp::StoreSlot || mx.op == IrOp::StoreGlobal) &&
                mx.idx == store_idx)
                return false;
        }
        return true;
    };
    RtFn fn = static_cast<RtFn>(in.imm);
    if (fn == RtFn::AnyArith)
        return (check(in.args[1]) || check(in.args[2])) ? 1 : 0;
    return check(in.args[0]) ? 1 : 0;
}

void AsmGen::gen_any_tagged(const Inst& in, RtFn fn) {
    if (fn == RtFn::AnyArith || fn == RtFn::AnyCmp || fn == RtFn::AnyBigBinary) {
        // IR: {op, a, b, desc_a, desc_b} → C: (op, a, tag_a, b, tag_b[, reuse])
        auto arg_tag = [&](VReg v) {
            TypePtr t = ty_of(v);
            if (fn == RtFn::AnyBigBinary && t) {
                if (t->kind == TyKind::U64) return std::string("$-1");
                if (is_fixed_int(t)) return std::string("$1");
            }
            return tag_src(v);
        };
        emit_arg(0, false, 0, src_mop(in.args[0]));
        emit_arg(1, false, 1, src_mop(in.args[1]));
        emit_arg(2, false, 2, arg_tag(in.args[1]));
        emit_arg(3, false, 3, src_mop(in.args[2]));
        emit_arg(4, false, 4, arg_tag(in.args[2]));
        if (fn == RtFn::AnyArith)
            emit_imm_op("movq", any_reuse_ok(in), arg_reg_i(5));
        spill_fp_slots();
        line(std::format("call {}", fn == RtFn::AnyArith ? "pa_any_arith_t"
                          : fn == RtFn::AnyCmp ? "pa_any_cmp_t" : "pa_any_big_binary_t"));
        reload_fp_slots();
        if (!dst_unused(in)) {
            emit2("movq", "%rax", dst_op(in.dst));
            // AnyArithT 双字返回：%rdx=tag 落 tag 字；AnyCmpT 返回 bool。
            if (fn != RtFn::AnyCmp && vreg_tagged(in.dst))
                emit2("movq", "%rdx", tag_mem(in.dst));
        }
        return;
    }
    if (fn == RtFn::AnyNeg || fn == RtFn::AnyInvert) {
        // IR: {a, desc} → C: (a, tag_a, reuse_ok)
        emit_arg(0, false, 0, src_mop(in.args[0]));
        emit_arg(1, false, 1, tag_src(in.args[0]));
        if (fn == RtFn::AnyNeg) emit_imm_op("movq", any_reuse_ok(in), arg_reg_i(2));
        spill_fp_slots();
        line(fn == RtFn::AnyNeg ? "call pa_any_neg_t" : "call pa_any_invert_t");
        reload_fp_slots();
        if (!dst_unused(in)) {
            emit2("movq", "%rax", dst_op(in.dst));
            if (vreg_tagged(in.dst)) emit2("movq", "%rdx", tag_mem(in.dst));
        }
        return;
    }
    // 单操作数：AnyIsFloat / AnyToF64 / AnyToI64 → (a, tag_a)。
    emit_arg(0, false, 0, src_mop(in.args[0]));
    emit_arg(1, false, 1, tag_src(in.args[0]));
    spill_fp_slots();
    line(std::format("call {}", fn == RtFn::AnyIsFloat ? "pa_any_is_float_t"
                     : fn == RtFn::AnyToF64 ? "pa_any_to_f64_t"
                                            : "pa_any_to_i64_t"));
    reload_fp_slots();
    if (!dst_unused(in)) {
        if (fn == RtFn::AnyToF64) move_fp(dst_op(in.dst), "%xmm0");
        else emit2("movq", "%rax", dst_op(in.dst));
    }
}


// P3：unknown RC 三入口 tag 判别内联。门控内且操作数带 tag 字时：
//   tag==0(UNK) → 旧 pa_rc_*_unknown（探针，逐字旧语义）
//   tag 1/2     → 非堆标量，空操作
//   tag>=3      → 堆指针，直接 pa_rc_inc/dec/temp（免探针）
// P2（tag_.type_tags）：堆判定改成 3..5 或 >=8；6/7（NONE/BOOL）非堆跳过。
void AsmGen::gen_rc_unknown_tagged(const Inst& in, RtFn fn) {
    VReg v = in.args[0];
    const std::string fb = fresh_label("rcut_fb");
    const std::string done = fresh_label("rcut_done");
    const std::string heap = tag_.type_tags ? fresh_label("rcut_heap") : std::string();
    emit2("movq", tag_mem(v), "%rax");
    line("testq %rax, %rax");
    emit_jmp("je", fb);
    if (tag_.type_tags) {
        line("cmpq $3, %rax");
        emit_jmp("jl", done);          // 0..2：非堆标量/裸位模式，跳过
        line("cmpq $6, %rax");
        emit_jmp("jl", heap);          // 3..5：堆
        line("cmpq $8, %rax");
        emit_jmp("jl", done);          // 6/7：NONE/BOOL，非堆
        emit_jmp("jmp", heap);         // >=8：堆子类 tag
        label(heap);
    } else {
        line("cmpq $3, %rax");
        emit_jmp("jl", done);
    }
    emit2("movq", src_op(v), arg_reg_i(0));
    spill_fp_slots();
    line(fn == RtFn::RcIncUnknown ? "call pa_rc_inc"
         : fn == RtFn::RcDecUnknown ? "call pa_rc_dec"
                                    : "call pa_rc_temp");
    reload_fp_slots();
    emit_jmp("jmp", done);
    label(fb);
    emit2("movq", src_op(v), arg_reg_i(0));
    spill_fp_slots();
    line(std::format("call {}", rt_name(fn)));
    reload_fp_slots();
    label(done);
}


// P3：tag -> 堆标志（0 非堆 / 1 堆 / 2 unknown 回退）。append/make_list/
// index_store 用它在门控内导出精确堆性：append 传 0/1 保住 kind2 位图
// （设计 §1.5），元素侧装载就能从位图直接得 tag，免探针。
void AsmGen::emit_heap_from_tag(VReg v, const std::string& dst) {
    if (tag_.type_tags) {
        // 堆 ⇔ 3..5 或 >=8；6/7（NONE/BOOL）与非 0 标量都导出 0。
        const std::string l0 = fresh_label("hft_l0");
        const std::string h1 = fresh_label("hft_h1");
        const std::string h2 = fresh_label("hft_h2");
        const std::string go = fresh_label("hft_go");
        emit2("movq", tag_src(v), "%rax");
        line("cmpq $3, %rax");
        emit_jmp("jl", l0);
        line("cmpq $6, %rax");
        emit_jmp("jl", h1);            // 3..5：堆
        line("cmpq $8, %rax");
        emit_jmp("jl", l0);            // 6/7：非堆标量
        emit_jmp("jmp", h1);           // >=8：堆子类
        label(l0);
        line("testq %rax, %rax");
        emit_jmp("je", h2);            // 0 = UNK：回退堆标志 2
        emit_imm_op("movq", 0, dst);
        emit_jmp("jmp", go);
        label(h1);
        emit_imm_op("movq", 1, dst);
        emit_jmp("jmp", go);
        label(h2);
        emit_imm_op("movq", 2, dst);
        label(go);
        return;
    }
    const std::string h1 = fresh_label("hft_h1");
    const std::string h2 = fresh_label("hft_h2");
    const std::string go = fresh_label("hft_go");
    emit2("movq", tag_src(v), "%rax");
    line("cmpq $3, %rax");
    emit_jmp("jae", h1);
    line("testq %rax, %rax");
    emit_jmp("je", h2);
    emit_imm_op("movq", 0, dst);
    emit_jmp("jmp", go);
    label(h1);
    emit_imm_op("movq", 1, dst);
    emit_jmp("jmp", go);
    label(h2);
    emit_imm_op("movq", 2, dst);
    label(go);
}

// P3：unknown 值的 append（门控内）：堆标志从 tag 导出，整段交运行时。
void AsmGen::gen_append_tagged(const Inst& in) {
    emit2("movq", src_op(in.args[0]), arg_reg_i(0));
    emit2("movq", src_op(in.args[1]), arg_reg_i(1));
    emit_heap_from_tag(in.args[1], arg_reg_i(2));
    spill_fp_slots();
    line("call pa_list_append");
    reload_fp_slots();
}

// P3：kind2 下标写入（门控内、新值带 tag）：堆标志从 tag 导出交运行时，
// 位图按精确堆性维护。内联 bts/btr 路径（静态 hk）完全不涉及。
void AsmGen::gen_index_store_tagged(const Inst& in, bool unchecked) {
    const std::string r0 = arg_reg_i(0), r1 = arg_reg_i(1);
    const std::string sl = target_abi().scratch_len;
    emit2("movq", src_op(in.a), r0);
    emit2("movq", src_op(in.b), r1);
    if (!unchecked) {
        const std::string fast = fresh_label("ixst_fast");
        const std::string neg = fresh_label("ixst_neg");
        line(std::format("movq 0({}), {}", r0, sl));
        line(std::format("cmpq {}, {}", sl, r1));
        emit_jmp("jb", fast);
        line(std::format("testq {}, {}", r1, r1));
        emit_jmp("js", neg);
        line(std::format("xorl {}, {}", reg32(arg_reg_i(2)), reg32(arg_reg_i(2))));
        emit_imm_op("movq", 2, arg_reg_i(3));
        line("call pa_list_set");
        line("ud2");
        label(neg);
        line(std::format("addq {}, {}", sl, r1));
        line(std::format("cmpq {}, {}", sl, r1));
        emit_jmp("jb", fast);
        line(std::format("movq {}, {}", sl, r1));
        line(std::format("xorl {}, {}", reg32(arg_reg_i(2)), reg32(arg_reg_i(2))));
        emit_imm_op("movq", 2, arg_reg_i(3));
        line("call pa_list_set");
        line("ud2");
        label(fast);
    }
    emit2("movq", src_op(in.c), arg_reg_i(2));
    emit_heap_from_tag(in.c, arg_reg_i(3));
    spill_fp_slots();
    line("call pa_list_set");
    reload_fp_slots();
}

// P3：unknown 元素装载的 tag 导出（设计 §3.3）。have_regs：快路径 r0/r1
// （list 指针/归一化下标）仍活，可按 elem_kind/位图内联导 tag；冷路径
// （运行时 pa_list_get 返回后）寄存器已死，走一次探针 pa_any_load_tag。
//   kind0 → INT（全标量；unknown 域 float 已装箱，裸位模式不会出现在
//           kind0 的 unknown 装载里）
//   kind1 → HEAP（全堆）
//   kind2 有位图 → 读位：1→HEAP（种类由算子解码头，免探针）/ 0→INT
//   kind2 无位图 → 探针定 tag（比「每次使用都探针」少）
void AsmGen::gen_index_load_tag(const Inst& in, bool have_regs) {
    if (!vreg_tagged(in.dst)) return;
    TypePtr ot = ty_of(in.a);
    bool listish = ot && (ot->kind == TyKind::List || ot->kind == TyKind::Tuple);
    const std::string tint = fresh_label("ixlt_int");
    const std::string theap = fresh_label("ixlt_heap");
    const std::string tprobe = fresh_label("ixlt_probe");
    const std::string tdone = fresh_label("ixlt_done");
    if (have_regs && listish) {
        const std::string r0 = arg_reg_i(0), r1 = arg_reg_i(1);
        line(std::format("movq 24({}), %rax", r0));
        line("testq %rax, %rax");
        emit_jmp("je", tint);
        line("cmpq $1, %rax");
        emit_jmp("je", theap);
        line(std::format("movq 32({}), %rax", r0));
        line("testq %rax, %rax");
        emit_jmp("je", tprobe);
        line(std::format("btq {}, (%rax)", r1));
        emit_jmp("jc", theap);
        emit_jmp("jmp", tint);
    } else {
        emit_jmp("jmp", tprobe);
    }
    label(tint);
    emit_tag_imm(in.dst, kTagInt);
    emit_jmp("jmp", tdone);
    label(theap);
    emit_tag_imm(in.dst, kTagHeap);
    emit_jmp("jmp", tdone);
    label(tprobe);
    emit2("movq", dst_op(in.dst), arg_reg_i(0));
    spill_fp_slots();
    line("call pa_any_load_tag");
    reload_fp_slots();
    emit2("movq", "%rax", tag_mem(in.dst));
    label(tdone);
}

// ── P3-1b：直调 tag ABI（0.328）─────────────────────────────────────
//
// 双字 ABI 的「旁路」形态：payload 的寄存器/栈落位逐字不变（classify
// 原样），tag 走平行数组 + %r11 指针通道。被调两个入口：
//   .LfN（旧入口，既有全部引用：闭包/vtable/间接/跨模块）：
//       xorl %r11d, %r11d; jmp .LfN$pb
//   .LfN$pt（tag 入口，仅门控内直调引用）：
//       （%r11 = 调用方 tag 数组指针，直通函数体）
// 函数体序言在 tag 区清零后按 %r11 装载形参 tag；%r11==0 → 全 UNK。
// 正确性不依赖传播完备：任何旧约定调用都落旧入口，tag 丢失=探针回退。

// 形参是否在 tag 数组里占一个字：**按声明类型**（p.ty）而不是槽类型。
//
// 0.328 的 P3-1b 按槽类型（slot_ty）算，因为 tag 字按 slot_ty 分配。但
// **目录 ABI 必须在被调定义之外也能推出来**：跨模块/间接调用的调用方只
// 看得到函数签名（p.ty），看不到被调内部槽升级。所以 P2（tag_.type_tags）
// 把 tag 数组布局统一改成「声明为 unknown 的形参各占一字」：
//   - 被调序言读到 tag 后，只在**该槽确实有 tag 字**（slot_tagged）时写入，
//     槽升级成静态类型的形参直接丢弃 tag（它内部已按静态类型处理）；
//   - 调用方按签名算数组长度，两边一致。
// 槽升级不再影响 tag 数组长度 → 跨模块可传。门控关时该布局与 0.339 相同
// （没有槽升级差异时）或更保守（升级过的形参多占一字但被调忽略）。
bool AsmGen::ptag_param(const IRFunction& f, const IRParam& p) const {
    if (p.slot < 0) return false;
    // 门控关：逐字沿用 0.328 的槽类型口径（asm 不变）。
    if (!tag_.type_tags)
        return p.slot < static_cast<int>(f.slot_ty.size()) &&
               is_unknown(f.slot_ty[static_cast<size_t>(p.slot)]);
    // P2：声明类型口径（跨模块可推导，见上方注释）。
    return is_unknown(p.ty);
}

int AsmGen::func_ptag_words() const {
    if (!tag_.tags_enabled || gen_) return 0;
    if (tag_.type_tags) {
        // P2：tag 数组按「第 i 个形参 = 第 i 个字」排布，长度 = 形参个数；
        // 序言只要有一个形参带 tag 字就需要读（槽升级的形参读出来丢弃）。
        for (const auto& p : f_->params)
            if (slot_tagged(p.slot)) return static_cast<int>(f_->params.size());
        return 0;
    }
    int n = 0;
    for (const auto& p : f_->params)
        if (ptag_param(*f_, p)) ++n;
    return n;
}

int AsmGen::call_ptag_words(const Inst& in) const {
    if (!tag_.tags_enabled) return 0;
    if (in.op == IrOp::CallDirect) {
        if (in.idx < 0 || in.idx >= static_cast<int>(m_->funcs.size())) return 0;
        const IRFunction& c = *m_->funcs[static_cast<size_t>(in.idx)];
        if (c.is_generator) return 0;
        // 实参/形参个数不一致（可变参数等）：tag 数组对不上号，回退旧约定。
        if (in.args.size() != c.params.size()) return 0;
        if (tag_.type_tags) return static_cast<int>(c.params.size());
        int n = 0;
        for (const auto& p : c.params)
            if (ptag_param(c, p)) ++n;
        return n;
    }
    if (in.op == IrOp::Call && tag_.type_tags) {
        // P2：间接调用（跨模块导入的函数、函数值、闭包）按**静态函数签名**
        // 推导 tag 数组布局 —— 被调同一次构建里也按声明类型排布（ptag_param）。
        // 含浮点形参时走 closure shim 重排，tag 布局不保证 → 不传（0 回退）。
        // 变参/关键字/个数对不上 → 不传。
        const FuncInfo* fi = nullptr;
        TypePtr ft = ty_of(in.a);
        if (ft && ft->kind == TyKind::Func) fi = ft->fn.get();
        if (!fi || fi->varargs || fi->kwargs) return 0;
        if (fi->pos.size() != in.args.size()) return 0;
        int n = 0;
        for (const auto& p : fi->pos) {
            if (ir_is_float(p.ty)) return 0;
            if (tag_.type_tags) ++n;      // P2：每形参一字（含静态形参的占位）
            else if (is_unk_ty(p.ty)) ++n;
        }
        return n;
    }
    return 0;
}

bool AsmGen::func_ret_tag(const IRFunction& f) const {
    return tag_.tags_enabled && !f.is_generator && is_unk_ty(f.ret);
}

void AsmGen::emit_ptag_entry(int fi) {
    // type_tags 构建也保留清零 gadget：所有既有引用（闭包/vtable/导入
    // 符号/间接调用）行为逐字不变，tag 数组走 pa_ptag_in 侧信道，不占
    // 任何寄存器 —— 这是跨模块传递不破坏旧 ABI 的关键。
    line("xorl %r11d, %r11d");
    emit_jmp("jmp", func_label(fi) + "$pb");
    label(ptag_label(fi));
    label(func_label(fi) + "$pb");
}

void AsmGen::emit_ret_tag(const Inst& in) {
    if (!func_ret_tag(*f_)) return;
    // 无值返回/浮点 vreg（unknown 返回正常已被降级器装箱）保守 UNK。
    if (in.a == kNoReg || is_fp(in.a)) line("xorl %edx, %edx");
    else emit2("movq", tag_src(in.a), "%rdx");
}

void AsmGen::emit_ptag_param_load() {
    const std::string skip = fresh_label("ptp_skip");
    if (tag_.type_tags) {
        // P2：从侧信道读 tag 数组并立刻清零。长度校验（= 本函数形参个数）
        // 不匹配就整体忽略 —— 错位 tag 会让 RC 按错误堆性 inc/dec。
        line("movq pa_ptag_in(%rip), %r11");
        line("movq $0, pa_ptag_in(%rip)");
        line("movq pa_ptag_len(%rip), %rax");
        line("movq $0, pa_ptag_len(%rip)");
        line(std::format("cmpq ${}, %rax", static_cast<int64_t>(f_->params.size())));
        emit_jmp("jne", skip);
    }
    line("testq %r11, %r11");
    emit_jmp("je", skip);
    int k = 0;
    for (const auto& p : f_->params) {
        if (tag_.type_tags) {
            // P2：第 i 个形参 = 第 i 个字；槽升级的形参读出来直接丢弃。
            if (slot_tagged(p.slot)) {
                line(std::format("movq {}(%r11), %rax", 8 * k));
                emit2("movq", "%rax", slot_tag_op(p.slot));
            }
            ++k;
            continue;
        }
        if (!ptag_param(*f_, p)) continue;
        line(std::format("movq {}(%r11), %rax", 8 * k));
        emit2("movq", "%rax", slot_tag_op(p.slot));
        ++k;
    }
    label(skip);
}

int AsmGen::emit_ptag_array(const Inst& in, const std::vector<ArgLoc>& locs) {
    int nstack = 0;
    for (const auto& l : locs)
        if (l.stack_off >= 0) ++nstack;
    // 数组紧跟本调用点的栈实参之后；outgoing_bytes 已按 tag 字数扩容。
    const int base = out_base() + 8 * nstack;
    const IRFunction& c = *m_->funcs[static_cast<size_t>(in.idx)];
    int k = 0;
    for (size_t i = 0; i < c.params.size(); ++i) {
        if (tag_.type_tags) {
            // P2：每个形参一个字（静态形参也写，被调按需忽略）—— 位置
            // 与形参序号一一对应，跨模块/间接调用不会因为形参类型推断
            // 差异而错位。
            move_to(std::format("{}(%rsp)", base + 8 * k), tag_src(in.args[i]));
            ++k;
            continue;
        }
        if (!ptag_param(c, c.params[i])) continue;
        move_to(std::format("{}(%rsp)", base + 8 * k), tag_src(in.args[i]));
        ++k;
    }
    return base;
}

}  // namespace pyadamas
