// lower_match.cpp —— match/case 与模式降级
#include "lower_impl.h"

namespace pyadamas {

void Lowerer::lower_match(Match& m) {
    TypePtr sty = tc_.type_of(m.subject.get());
    VReg sv = expr(*m.subject);
    int subj = new_temp_slot(sty ? sty : t_unknown());
    store_slot(subj, sv);
    int end = new_block("match.end");

    // case 里的捕获同时写一份到 case 外的作用域：Python 里 match 捕获
    // 就是普通赋值，分支结束后仍然可见。
    auto saved_escape = std::move(st_->match_escape);
    st_->match_escape.clear();
    if (auto* caps = tc_.match_captures(&m)) {
        bool module_level = st_->scope == tc_.module_scope();
        for (auto& [name, ty] : *caps) {
            FuncCtx::MatchEscape e;
            e.ty = ty;
            if (is_cell(name) || is_env(name)) {
                e.kind = 1;
                e.idx = 0;
            } else if (st_->globals_decl.count(name) || module_level) {
                int gi = global_idx_as(name, ty);
                e.kind = 2;
                e.idx = gi;
            } else {
                auto it = st_->slots.find(name);
                if (it == st_->slots.end()) {
                    int slot = st_->fn->new_slot(ty ? ty : t_unknown());
                    st_->slots[name] = slot;
                    it = st_->slots.find(name);
                }
                st_->extra_locals[name] = ty;
                e.kind = 0;
                e.idx = it->second;
            }
            st_->match_escape[name] = e;
        }
    }

    for (auto& c : m.cases) {
        int fail = new_block("match.fail");
        comp_slots_.emplace_back();
        lower_pattern(*c.pattern, load_slot(subj, sty ? sty : t_unknown()), sty, fail);
        if (c.guard) {
            VReg g = expr(*c.guard);
            g = truth_bool(g, tc_.type_of(c.guard.get()));
            int gok = new_block("match.guard");
            Inst br;
            br.op = IrOp::Branch;
            br.a = g;
            br.target = gok;
            br.target2 = fail;
            emit(std::move(br));
            switch_to(gok);
        }
        stmts(c.body);
        comp_slots_.pop_back();
        if (!terminated()) {
            Inst j;
            j.op = IrOp::Jump;
            j.target = end;
            emit(std::move(j));
        }
        switch_to(fail);
    }
    Inst j;
    j.op = IrOp::Jump;
    j.target = end;
    emit(std::move(j));
    switch_to(end);
    st_->match_escape = std::move(saved_escape);
}

static void lower_pattern_names(const Pattern& p, std::vector<std::string>& out) {
    switch (p.kind) {
        case PatternKind::Capture:
            if (!p.name.empty() && p.name != "_") out.push_back(p.name);
            return;
        case PatternKind::As:
            if (!p.name.empty() && p.name != "_") out.push_back(p.name);
            for (auto& it : p.items) lower_pattern_names(*it, out);
            return;
        case PatternKind::Sequence:
        case PatternKind::Or:
            for (auto& it : p.items) lower_pattern_names(*it, out);
            return;
        case PatternKind::Mapping:
            for (auto& kv : p.pairs) lower_pattern_names(*kv.second, out);
            if (!p.rest.empty() && p.rest != "_") out.push_back(p.rest);
            return;
        case PatternKind::Class:
            for (auto& it : p.positional) lower_pattern_names(*it, out);
            for (auto& kv : p.attrs) lower_pattern_names(*kv.second, out);
            return;
        default:
            return;
    }
}

static bool builtin_pattern_matches(const std::string& name, TyKind k) {
    if (name == "object") return true;
    if (name == "int") return k == TyKind::Int || k == TyKind::Bool;
    if (name == "bool") return k == TyKind::Bool;
    if (name == "float") return k == TyKind::Float;
    if (name == "str") return k == TyKind::Str;
    if (name == "list") return k == TyKind::List;
    if (name == "dict") return k == TyKind::Dict;
    if (name == "tuple") return k == TyKind::Tuple;
    if (name == "set") return k == TyKind::Set;
    if (name == "complex") return k == TyKind::Complex;
    return false;
}

void Lowerer::lower_pattern(Pattern& p, VReg value, TypePtr ty, int fail, bool in_or,
                            std::unordered_set<std::string>* or_local) {
    auto jump_fail = [&]() {
        Inst j;
        j.op = IrOp::Jump;
        j.target = fail;
        emit(std::move(j));
    };
    auto test = [&](VReg cond) {
        int ok = new_block("pat.ok");
        Inst br;
        br.op = IrOp::Branch;
        br.a = cond;
        br.target = ok;
        br.target2 = fail;
        emit(std::move(br));
        switch_to(ok);
    };
    TypePtr st = ty;
    bool optional = ty && ty->kind == TyKind::Optional;
    if (st && st->kind == TyKind::Optional && !st->sub.empty()) st = st->sub[0];

    auto bind_name = [&](const std::string& name, VReg v, TypePtr vty) {
        if (name.empty() || name == "_") return;
        auto bind_escape = [&]() {
            auto eit = st_->match_escape.find(name);
            if (eit == st_->match_escape.end()) return;
            auto& e = eit->second;
            if (e.kind == 1) {
                write_cell(name, v, vty);
                return;
            }
            VReg ev = coerce(v, vty, e.ty);
            if (e.kind == 2) store_global(e.idx, ev);
            else store_slot(e.idx, ev);
        };
        auto& scope = comp_slots_.back();
        auto it = scope.find(name);
        bool duplicate =
            it != scope.end() && (!in_or || (or_local && or_local->count(name)));
        if (duplicate) {
            // 同一模式里重复捕获：Python 要求值相等。
            TypePtr oldty = st_->fn->slot_ty[it->second];
            VReg old = load_slot(it->second, oldty ? oldty : t_unknown());
            Inst cmp;
            cmp.op = IrOp::Cmp;
            cmp.cop = CmpKind::Eq;
            cmp.a = v;
            cmp.b = old;
            test(def(t_bool(), std::move(cmp)));
            return;
        }
        if (it != scope.end()) {
            // OR 预声明的共享槽：哪个分支成功就写哪个。
            store_slot(it->second, v);
            if (or_local) or_local->insert(name);
            bind_escape();
            return;
        }
        int slot = st_->fn->new_slot(vty ? vty : t_unknown());
        scope[name] = slot;
        store_slot(slot, v);
        if (or_local) or_local->insert(name);
        bind_escape();
    };

    auto match_equal = [&](ExprPtr& lit) {
        TypePtr lt = tc_.type_of(lit.get());
        if (lt && lt->kind == TyKind::None) {
            // 主体静态类型是 Optional[T] 时，None 是一个真实可能的值
            // （上面已经把 T 剥进 st，Optional 标记留在 optional 里）。此时
            // 必须真的比较 value == 0（pa 的 None 就是 0）；原来只看剥完的
            // st（int），一律 jump_fail，于是 int|None 主体传 None 永远落到
            // case _（实测 f(None) 返回 "other"）。非 Optional 的 int 主体
            // 仍然不匹配 None（与 CPython 一致）。
            if (st && st->kind != TyKind::None && st->kind != TyKind::Unknown &&
                !optional) {
                jump_fail();
                return;
            }
            Inst cmp;
            cmp.op = IrOp::Cmp;
            cmp.cop = CmpKind::Eq;
            cmp.a = value;
            cmp.b = const_int(0);
            test(def(t_bool(), std::move(cmp)));
            return;
        }
        if (lt && lt->kind == TyKind::Complex) {
            VReg lv = expr(*lit);
            if (st && st->kind == TyKind::Unknown) {
                test(call_runtime(RtFn::ComplexEqUnknown, t_bool(), {value, lv}));
                return;
            }
            if (st && st->kind != TyKind::Complex) {
                jump_fail();
                return;
            }
            if (st && st->kind != TyKind::Complex) lv = convert_to(lv, lt, t_complex());
            test(call_runtime(RtFn::ComplexEq, t_bool(), {value, lv}));
            return;
        }
        bool ln = lt && (lt->kind == TyKind::Int || lt->kind == TyKind::Bool ||
                         lt->kind == TyKind::Float);
        bool sn = st && (st->kind == TyKind::Int || st->kind == TyKind::Bool ||
                         st->kind == TyKind::Float);
        if (ln && st && !sn && st->kind != TyKind::Unknown) {
            jump_fail();
            return;
        }
        if (lt && lt->kind == TyKind::Str) {
            if (st && st->kind == TyKind::Unknown) {
                VReg lv = expr(*lit);
                test(call_runtime(RtFn::StrEqUnknown, t_bool(), {value, lv}));
                return;
            }
            if (st && st->kind != TyKind::Str) {
                jump_fail();
                return;
            }
        }
        VReg lv = expr(*lit);
        Inst cmp;
        cmp.op = IrOp::Cmp;
        cmp.cop = CmpKind::Eq;
        if (ln && sn) {
            TypePtr cty = (lt->kind == TyKind::Float || st->kind == TyKind::Float)
                              ? t_float()
                              : t_int();
            cmp.a = convert_to(value, st, cty);
            cmp.b = convert_to(lv, lt, cty);
        } else {
            cmp.a = value;
            cmp.b = lv;
        }
        test(def(t_bool(), std::move(cmp)));
    };

    switch (p.kind) {
        case PatternKind::Wildcard: return;
        case PatternKind::Capture:
            bind_name(p.name, value, st ? st : t_unknown());
            return;
        case PatternKind::As:
            for (auto& it : p.items) lower_pattern(*it, value, ty, fail, in_or, or_local);
            bind_name(p.name, value, st ? st : t_unknown());
            return;
        case PatternKind::Literal:
            match_equal(p.lit);
            return;
        case PatternKind::Value:
            match_equal(p.lit);
            return;
        case PatternKind::Or: {
            std::vector<std::string> names;
            lower_pattern_names(p, names);
            auto& scope = comp_slots_.back();
            for (auto& n : names) {
                if (!scope.count(n)) {
                    int slot = st_->fn->new_slot(t_unknown());
                    scope[n] = slot;
                }
            }
            int done = new_block("pat.or.ok");
            for (size_t i = 0; i < p.items.size(); ++i) {
                bool last = i + 1 == p.items.size();
                int nf = new_block("pat.or.next");
                std::unordered_set<std::string> local;
                lower_pattern(*p.items[i], value, ty, last ? fail : nf, true, &local);
                Inst j;
                j.op = IrOp::Jump;
                j.target = done;
                emit(std::move(j));
                if (!last) switch_to(nf);
            }
            switch_to(done);
            return;
        }
        case PatternKind::Sequence: {
            if (!st || st->kind == TyKind::Str ||
                (st->kind != TyKind::List && st->kind != TyKind::Tuple &&
                 st->kind != TyKind::Unknown)) {
                jump_fail();
                return;
            }
            if (st->kind == TyKind::Unknown)
                test(call_runtime(RtFn::IsSeq, t_bool(), {value}));
            int fixed = static_cast<int>(p.items.size()) - (p.star >= 0 ? 1 : 0);
            Inst ln;
            ln.op = IrOp::Len;
            ln.a = value;
            VReg n = def(t_int(), std::move(ln));
            if (p.star < 0) {
                Inst cmp;
                cmp.op = IrOp::Cmp;
                cmp.cop = CmpKind::Eq;
                cmp.a = n;
                cmp.b = const_int(fixed);
                test(def(t_bool(), std::move(cmp)));
            } else {
                Inst cmp;
                cmp.op = IrOp::Cmp;
                cmp.cop = CmpKind::Lt;
                cmp.a = n;
                cmp.b = const_int(fixed);
                Inst br;
                br.op = IrOp::Branch;
                br.a = def(t_bool(), std::move(cmp));
                int ok = new_block("pat.seq.ok");
                br.target = fail;
                br.target2 = ok;
                emit(std::move(br));
                switch_to(ok);
            }
            TypePtr elem = t_unknown();
            if (st->kind == TyKind::List) elem = st->sub.empty() ? t_unknown() : st->sub[0];
            int seen = 0;
            for (size_t i = 0; i < p.items.size(); ++i) {
                if (static_cast<int>(i) == p.star) {
                    if (p.items[i]->kind == PatternKind::Capture && !p.items[i]->name.empty()) {
                        int tail = static_cast<int>(p.items.size()) - p.star - 1;
                        VReg stop = n;
                        if (tail > 0) {
                            Inst sub;
                            sub.op = IrOp::Bin;
                            sub.bop = BinKind::Sub;
                            sub.a = n;
                            sub.b = const_int(tail);
                            stop = def(t_int(), std::move(sub));
                        }
                        VReg rest = call_runtime(RtFn::ListSlice, mk_list(elem),
                                                 {value, const_int(fixed), stop, const_int(1)});
                        push_temp(rest);
                        bind_name(p.items[i]->name, rest, mk_list(elem));
                    }
                    continue;
                }
                TypePtr et = elem;
                if (st->kind == TyKind::Tuple && i < st->sub.size()) et = st->sub[i];
                Inst get;
                get.op = IrOp::IndexLoad;
                get.a = value;
                get.b = const_int(seen);
                VReg ev = def(et, std::move(get));
                lower_pattern(*p.items[i], ev, et, fail, in_or, or_local);
                ++seen;
            }
            return;
        }
        case PatternKind::Mapping: {
            if (!st || st->kind == TyKind::Unknown) {
                test(call_runtime(RtFn::IsDict, t_bool(), {value}));
            } else if (st->kind != TyKind::Dict) {
                jump_fail();
                return;
            }
            TypePtr kt = t_unknown();
            TypePtr vt = t_unknown();
            if (st && st->kind == TyKind::Dict) {
                if (!st->sub.empty()) kt = st->sub[0];
                if (st->sub.size() > 1) vt = st->sub[1];
            }
            std::vector<VReg> keys;
            for (auto& kv : p.pairs) {
                VReg key = expr(*kv.first);
                key = coerce(key, tc_.type_of(kv.first.get()), kt);
                keys.push_back(key);
                test(call_runtime(RtFn::DictContains, t_bool(), {value, key}));
                Inst get;
                get.op = IrOp::CallRt;
                get.imm = static_cast<int64_t>(RtFn::DictGet);
                get.args = {value, key};
                VReg ev = def(vt, std::move(get));
                lower_pattern(*kv.second, ev, vt, fail, in_or, or_local);
            }
            if (!p.rest.empty() && p.rest != "_") {
                TypePtr dicty = mk_dict(kt, vt);
                VReg copy = call_runtime(RtFn::DictCopy, dicty, {value});
                push_temp(copy);
                for (VReg k : keys)
                    call_void(RtFn::DictRemove, {copy, k, const_int(1)});
                bind_name(p.rest, copy, dicty);
            }
            return;
        }
        case PatternKind::Class: {
            if (!p.qualname.empty()) {
                test(call_runtime(RtFn::IsNamedClass, t_bool(),
                                  {value, const_str(p.qualname)}));
                if (p.positional.empty() && p.attrs.empty()) return;
                // 字段偏移在定义模块里已经算好，接口里的 ClassInfo 直接可用。
                size_t dot = p.qualname.rfind('.');
                std::string mod_name = p.qualname.substr(0, dot);
                std::string cls_name = p.qualname.substr(dot + 1);
                ModuleInfoPtr mod;
                auto mit = st_->modules.find(mod_name);
                if (mit != st_->modules.end()) mod = mit->second;
                else {
                    auto ait = module_aliases_.find(mod_name);
                    if (ait != module_aliases_.end()) mod = ait->second;
                }
                ClassInfoPtr pc;
                if (mod) {
                    auto ci = mod->classes.find(cls_name);
                    if (ci != mod->classes.end()) pc = ci->second.cls;
                }
                if (!pc) {
                    err(p.line, p.col,
                        std::format("class '{}' is not available for pattern matching",
                                    p.qualname));
                    return;
                }
                auto match_field = [&](const std::string& fname, PatternPtr& sub) {
                    const FieldInfo* f = pc->field(fname);
                    if (!f) {
                        err(p.line, p.col,
                            std::format("'{}' object has no field '{}'", pc->name, fname));
                        return;
                    }
                    int off = 8 * (1 + pc->field_index(fname));
                    VReg fv = field_load(value, off, f->ty);
                    lower_pattern(*sub, fv, f->ty, fail, in_or, or_local);
                };
                if (!p.positional.empty()) {
                    const auto* margs = pc->match_args_of();
                    if (!margs) {
                        err(p.line, p.col,
                            std::format("'{}' has no __match_args__ for positional patterns",
                                        pc->name));
                    } else {
                        size_t n = std::min(p.positional.size(), margs->size());
                        for (size_t i = 0; i < n; ++i)
                            match_field((*margs)[i], p.positional[i]);
                    }
                }
                for (auto& kv : p.attrs) match_field(kv.first, kv.second);
                return;
            }
            if (p.cls.find('.') != std::string::npos) {
                err(p.line, p.col,
                    std::format("class pattern '{}' could not be resolved", p.cls));
                return;
            }
            auto cit = classes_.find(p.cls);
            if (cit != classes_.end() && cit->second) {
                const ClassInfoPtr& pc = cit->second;
                if (pc->is_exception_class) {
                    if (!p.positional.empty() || !p.attrs.empty()) {
                        err(p.line, p.col,
                            "exception class patterns cannot have sub-patterns");
                        return;
                    }
                    test(call_runtime(RtFn::IsException, t_bool(),
                                      {value, const_str(pc->name)}));
                    return;
                }
                auto cid = class_ids_.find(pc.get());
                if (cid == class_ids_.end()) {
                    err(p.line, p.col,
                        std::format("class '{}' is not available for pattern matching",
                                    pc->name));
                    return;
                }
                bool need_check = true;
                if (st && st->kind == TyKind::Class && st->cls) {
                    if (st->cls->is_subclass_of(pc.get())) {
                        // 静态类型就是模式类（或它的子类），必然匹配；
                        // 只有 Optional 还要排除 None。
                        need_check = optional;
                    } else if (pc->is_subclass_of(st->cls)) {
                        // 静态类型是父类、模式是子类：实际值可能是子类。
                        need_check = true;
                    } else {
                        jump_fail();
                        return;
                    }
                } else if (st && st->kind != TyKind::Unknown) {
                    jump_fail();
                    return;
                }
                if (need_check)
                    test(call_runtime(
                        RtFn::IsNamedClass, t_bool(),
                        {value, const_str(mod_.runtime_name + "." + pc->name)}));
                auto match_field = [&](const std::string& fname, PatternPtr& sub) {
                    const FieldInfo* f = pc->field(fname);
                    if (!f) {
                        err(p.line, p.col,
                            std::format("'{}' object has no field '{}'", pc->name, fname));
                        return;
                    }
                    int off = 8 * (1 + pc->field_index(fname));
                    VReg fv = field_load(value, off, f->ty);
                    lower_pattern(*sub, fv, f->ty, fail, in_or, or_local);
                };
                if (!p.positional.empty()) {
                    const auto* margs = pc->match_args_of();
                    if (!margs) {
                        err(p.line, p.col,
                            std::format("'{}' has no __match_args__ for positional patterns",
                                        pc->name));
                    } else {
                        size_t n = std::min(p.positional.size(), margs->size());
                        for (size_t i = 0; i < n; ++i)
                            match_field((*margs)[i], p.positional[i]);
                    }
                }
                for (auto& kv : p.attrs) match_field(kv.first, kv.second);
                return;
            }
            if (TypeChecker::is_exception_type(p.cls)) {
                test(call_runtime(RtFn::IsException, t_bool(),
                                  {value, const_str(p.cls)}));
                return;
            }
            static const std::unordered_set<std::string> builtin_types = {
                "int", "float", "str", "bool", "list", "dict", "tuple",
                "set", "complex", "object",
            };
            if (builtin_types.count(p.cls)) {
                if (!st || st->kind == TyKind::Unknown) {
                    // 原始 int/float 没有运行期标签，无法区分；堆类型
                    // 可以做安全检查。
                    if (p.cls == "object") return;
                    if (p.cls == "str") {
                        test(call_runtime(RtFn::IsStr, t_bool(), {value}));
                        return;
                    }
                    if (p.cls == "complex") {
                        test(call_runtime(RtFn::IsComplex, t_bool(), {value}));
                        return;
                    }
                    err(p.line, p.col,
                        std::format("cannot match builtin type pattern '{}()' against a "
                                    "value of unknown type", p.cls));
                    return;
                }
                if (!builtin_pattern_matches(p.cls, st->kind)) jump_fail();
                return;
            }
            err(p.line, p.col,
                std::format("unknown class '{}' in class pattern", p.cls));
            return;
        }
    }
}

}  // namespace pyadamas
