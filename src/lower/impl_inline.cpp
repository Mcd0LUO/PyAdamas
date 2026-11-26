// 0.607：Lowerer 的内联成员函数体（从 impl.h 的类声明里搬出来）。
// 声明留在 impl.h；这里只是定义，纯搬移。
#include "lower/impl.h"

namespace pyadamas {

    void Lowerer::err(int line, int col, std::string msg) {
        errors_.push_back({line, col, std::move(msg)});
    }

    void Lowerer::unsupported(const Stmt& s, const char* what) {
        err(s.line, s.col, std::format("{} is not supported by the code generator yet", what));
    }

    int Lowerer::new_block(std::string name) {
        // 别用聚合初始化：Block 加了 id 字段后会错位（-Werror 会抓，
        // 但更稳妥的是显式赋值）。
        Block nb;
        nb.name = std::move(name);
        st_->fn->blocks.push_back(std::move(nb));
        return static_cast<int>(st_->fn->blocks.size()) - 1;
    }

    void Lowerer::end_unreachable() {
        int dead = new_block("unreachable");
        Inst j;
        j.op = IrOp::Jump;
        j.target = dead;
        emit(std::move(j));
    }

    VReg Lowerer::alloc(TypePtr ty) {
        VReg r = static_cast<VReg>(st_->fn->vreg_ty.size());
        st_->fn->vreg_ty.push_back(ty ? std::move(ty) : t_unknown());
        st_->fn->vreg_nonneg.push_back(0);
        return r;
    }

    VReg Lowerer::def(TypePtr ty, Inst in) {
        VReg r = alloc(ty);
        // 先算右值再取左值引用：nonneg_of 虽当前只读，但把 RHS 独立成临时量
        // 杜绝「LHS vector 引用先于 RHS 求值、RHS 若 realloc 则悬空」的求值顺序 UB
        // （ASan 编译器构建 118_suspend_expr 暴露）；并显式守 size 防越界。
        const uint8_t nn = nonneg_of(in);
        auto& vn = st_->fn->vreg_nonneg;
        if (static_cast<size_t>(r) >= vn.size()) vn.resize(static_cast<size_t>(r) + 1, 0);
        vn[static_cast<size_t>(r)] = nn;
        in.dst = r;
        in.ty = ty;
        if (in.line == 0) in.line = cur_line_;
        st_->fn->ensure_inst_id(in);
        if (!cur().terminated()) cur().insts.push_back(std::move(in));
        return r;
    }

    void Lowerer::emit(Inst in) {
        in.dst = kNoReg;
        if (in.line == 0) in.line = cur_line_;
        st_->fn->ensure_inst_id(in);
        if (!cur().terminated()) cur().insts.push_back(std::move(in));
    }

    VReg Lowerer::heap_arg(TypePtr t) {
        if (!t) return const_int(2);
        if (t->kind == TyKind::Unknown) return const_int(2);
        return const_int(ir_is_heap(t) ? 1 : 0);
    }

    VReg Lowerer::const_int(int64_t v) {
        Inst in;
        in.op = IrOp::ConstInt;
        in.imm = v;
        return def(t_int(), std::move(in));
    }

    VReg Lowerer::const_int_ty(int64_t v, TypePtr ty) {
        Inst in;
        in.op = IrOp::ConstInt;
        in.imm = v;
        return def(ty ? std::move(ty) : t_int(), std::move(in));
    }

    VReg Lowerer::const_bool(bool v) {
        Inst in;
        in.op = IrOp::ConstInt;
        in.imm = v ? 1 : 0;
        return def(t_bool(), std::move(in));
    }

    VReg Lowerer::const_float(double v) {
        Inst in;
        in.op = IrOp::ConstFloat;
        in.fimm = v;
        return def(t_float(), std::move(in));
    }

    VReg Lowerer::const_none() {
        Inst in;
        in.op = IrOp::ConstNone;
        return def(t_none(), std::move(in));
    }

    VReg Lowerer::const_str(const std::string& s) {
        Inst in;
        in.op = IrOp::ConstStr;
        in.idx = mod_.intern_string(s);
        return def(t_str(), std::move(in));
    }

    VReg Lowerer::const_bytes(const std::string& s) {
        Inst in;
        in.op = IrOp::ConstStr;
        in.idx = mod_.intern_bytes(s);
        return def(t_bytes(), std::move(in));
    }

    VReg Lowerer::const_big(const std::string& s) {
        Inst in;
        in.op = IrOp::ConstBig;
        in.idx = Operand::big(mod_.intern_big(s));
        return def(t_big(), std::move(in));
    }

    int Lowerer::new_borrow_slot(TypePtr ty) {
        int s = new_temp_slot(std::move(ty));
        st_->borrow_slots.insert(s);
        st_->fn->slot_owned[static_cast<size_t>(s)] = false;
        st_->fn->slot_unknown[static_cast<size_t>(s)] = false;
        return s;
    }

    void Lowerer::store_slot_raw(int slot, VReg v) {
        for (auto* h : st_->bounds_hints)
            if (h->seq_slot == slot || h->idx_slot == slot) h->valid = false;
        // 写入不可证非负的值 → 作废该槽的非负提示（后续 load 不再打标）。
        if (st_->nonneg_slots.count(slot) && !nn(v)) st_->nonneg_slots.erase(slot);
        Inst in;
        in.op = IrOp::StoreSlot;
        in.idx = slot;
        in.a = v;
        emit(std::move(in));
    }

    void Lowerer::store_global_raw(int gi, VReg v) {
        Inst in;
        in.op = IrOp::StoreGlobal;
        in.idx = gi;
        in.a = v;
        emit(std::move(in));
    }

    bool Lowerer::is_cell(const std::string& name) const {
        return st_ && st_->cell_slots.count(name) > 0;
    }

    int Lowerer::comp_slot(const std::string& name) const {
        for (auto it = comp_slots_.rbegin(); it != comp_slots_.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return f->second;
        }
        return -1;
    }

    bool Lowerer::is_env(const std::string& name) const {
        auto* m = closure();
        return m && st_->env_slot >= 0 && m->env_index.count(name) > 0;
    }

    TypePtr Lowerer::captured_ty(const std::string& name) const {
        auto* m = closure();
        if (!m) return nullptr;
        if (auto it = m->cell_index.find(name); it != m->cell_index.end())
            return m->cells[static_cast<size_t>(it->second)].ty;
        if (auto it = m->env_index.find(name); it != m->env_index.end())
            return m->env[static_cast<size_t>(it->second)].ty;
        return nullptr;
    }

    VReg Lowerer::load_cell(const std::string& name) {
        TypePtr T = captured_ty(name);
        int slot = st_->cell_slots.at(name);
        return load_slot(slot, mk_list(T ? T : t_unknown()));
    }

    VReg Lowerer::read_cell(const std::string& name) {
        VReg c = load_cell(name);
        TypePtr T = captured_ty(name);
        Inst in;
        in.op = IrOp::IndexLoad;
        in.a = c;
        in.b = const_int(0);
        return def(T ? T : t_unknown(), std::move(in));
    }

    void Lowerer::bind_name_value(const std::string& name, VReg v, TypePtr vty) {
        if (is_cell(name)) {
            write_cell(name, v, vty);
        } else if (is_local(name)) {
            store_slot(st_->slots[name], v);
        } else {
            store_global(global_idx(name), v);
        }
    }

    bool Lowerer::is_local(const std::string& name) const {
        if (!st_) return false;
        if (comp_slot(name) >= 0) return true;
        return st_->slots.count(name) > 0 || is_env(name);
    }

    TypePtr Lowerer::storage_ty(const std::string& name) {
        if (auto t = captured_ty(name)) return t;
        if (st_->globals_decl.count(name) || !is_local(name)) return mod_.globals[global_idx(name)].ty;
        return st_->fn->slot_ty[st_->slots[name]];
    }

    bool Lowerer::is_bound_here(const std::string& name) const {
        if (!st_) return false;
        if (st_->globals_decl.count(name)) return true;
        return is_local(name);
    }

    bool Lowerer::is_builtin(const std::string& name) const {
        if (is_bound_here(name)) return false;
        if (auto* ms = tc_.module_scope())
            if (ms->vars.count(name)) return false;
        return true;
    }

    RtFn Lowerer::raise_rt(RtFn f) const {
        if (!st_ || st_->zc_raise_depth <= 0) return f;
        switch (f) {
            case RtFn::Raise: return RtFn::EhRaise;
            case RtFn::RaiseValueFrom: return RtFn::EhRaiseValueFrom;
            case RtFn::Reraise: return RtFn::EhReraise;
            default: return f;
        }
    }

    int Lowerer::new_str_cursor() {
        int cp = new_temp_slot(t_int());
        int off = new_temp_slot(t_int());
        VReg a1 = slot_addr_of(cp);
        VReg a2 = slot_addr_of(off);
        (void)a1;
        (void)a2;
        store_slot(cp, const_int(-1));
        store_slot(off, const_int(0));
        return cp;
    }

    VReg Lowerer::str_char_at(VReg seq, VReg idx, int cur_slot) {
        if (cur_slot < 0) {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::StrCharAt);
            in.args = {seq, idx};
            VReg v = def(t_str(), std::move(in));
            push_temp(v);
            return v;
        }
        VReg curp = slot_addr_of(cur_slot);
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::StrCharAtCursor);
        in.args = {seq, idx, curp};
        VReg v = def(t_str(), std::move(in));
        push_temp(v);
        return v;
    }

    int Lowerer::extern_var_idx(const CVar& v) {
        for (size_t i = 0; i < mod_.extern_vars.size(); ++i)
            if (mod_.extern_vars[i].symbol == v.symbol) return static_cast<int>(i);
        IRExternVar ev;
        ev.symbol = v.symbol;
        ev.kind = v.kind;
        mod_.extern_vars.push_back(std::move(ev));
        return static_cast<int>(mod_.extern_vars.size()) - 1;
    }

    TypePtr Lowerer::cvar_type(const CVar& v) {
        TypePtr t = ckind_type(v.kind);
        if (v.kind == CKind::Ptr && !v.struct_name.empty()) {
            if (StructInfo* st = tc_.struct_info(v.struct_name))
                t = mk_optional(mk_struct_ptr(st));
        } else if (v.kind == CKind::Ptr && v.elem != CKind::None && t) {
            // 0.564：数组变量的符号地址是编译期常量，不可能是 None —— 给非空 ptr[T]，
            // 省掉无意义的 `if p is not None:` 仪式。
            if (v.addr_of) t = t_int();
            t->celem = v.elem;
        }
        return t;
    }

    VReg Lowerer::field_addr(VReg obj, int64_t off) {
        Inst in;
        in.op = IrOp::Bin;
        in.bop = BinKind::Add;
        in.a = obj;
        in.b = const_int(off);
        return def(t_int(), std::move(in));
    }

    void Lowerer::field_store(VReg obj, int off, VReg v, uint8_t width, bool unsig) {
        Inst in;
        in.op = IrOp::FieldStore;
        in.a = obj;
        in.imm = off;
        in.b = v;
        in.ty = vreg_ty(v);  // 宽度 4 的 float32 字段要靠值类型决定 cvtsd2ss
        in.fwidth = width;
        in.funsigned = unsig;
        emit(std::move(in));
    }

    VReg Lowerer::field_load(VReg obj, int off, TypePtr ty, uint8_t width, bool unsig) {
        Inst in;
        in.op = IrOp::FieldLoad;
        in.a = obj;
        in.imm = off;
        in.fwidth = width;
        in.funsigned = unsig;
        return def(ty, std::move(in));
    }

    VReg Lowerer::struct_new(StructInfo* s) {
        Inst in;
        in.op = IrOp::StructNew;
        in.imm = s->size;
        return def_heap(mk_struct(s), std::move(in));
    }

    VReg Lowerer::struct_copy(VReg v, StructInfo* s) {
        Inst in;
        in.op = IrOp::StructCopy;
        in.a = v;
        in.imm = s->size;
        return def_heap(mk_struct(s), std::move(in));
    }

    TypePtr Lowerer::strip_optional(const TypePtr& t) {
        if (t && t->kind == TyKind::Optional && !t->sub.empty()) return t->sub[0];
        return t;
    }

    TypePtr Lowerer::vreg_ty(VReg v) const {
        return v < st_->fn->vreg_ty.size() ? st_->fn->vreg_ty[v] : t_unknown();
    }

    VReg Lowerer::call_runtime(RtFn f, TypePtr ret, std::vector<VReg> args) {
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(f);
        in.args = std::move(args);
        return def(std::move(ret), std::move(in));
    }

    void Lowerer::call_void(RtFn f, std::vector<VReg> args) {
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(f);
        in.args = std::move(args);
        emit(std::move(in));
    }

    void Lowerer::push_frame(IRFunction* fn, const Scope* scope) {
        auto ctx = std::make_unique<FuncCtx>();
        ctx->fn = fn;
        ctx->scope = scope;
        frames_.push_back(std::move(ctx));
        st_ = frames_.back().get();
    }

    void Lowerer::pop_frame() {
        frames_.pop_back();
        st_ = frames_.empty() ? nullptr : frames_.back().get();
    }

}  // namespace pyadamas
