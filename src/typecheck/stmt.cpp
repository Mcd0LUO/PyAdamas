// typecheck/stmt.cpp —— 语句类型检查：控制流、try/with、赋值与解包
#include "typecheck/typecheck.h"

#include <filesystem>
#include <set>

#include "ir/module.h"

namespace pyadamas {

static void pattern_capture_names(const Pattern& p, std::vector<std::string>& out);

void TypeChecker::build_struct(StructDef& s) {
        if (structs_.count(s.name)) {
            err(s.line, s.col, std::format("struct '{}' is already defined", s.name));
            return;
        }
        auto info = std::make_unique<StructInfo>();
        info->name = s.name;
        // v1 的字段映射固定：int → int64_t(8/8)、float → double(8/8)、
        // bool → _Bool(1/1)。布局算法与 C 一致：按字段对齐向上取整放置，
        // 总大小对齐到最大成员对齐。
        int64_t off = 0;
        int64_t max_align = 1;
        for (auto& f : s.fields) {
            if (info->field(f.name)) {
                err(f.line, f.col,
                    std::format("duplicate field '{}' in struct '{}'", f.name, s.name));
                continue;
            }
            auto* nm = dynamic_cast<Name*>(f.type.get());
            TypePtr ft;
            if (nm && nm->id == "int") ft = t_int();
            else if (nm && nm->id == "float") ft = t_float();
            else if (nm && nm->id == "bool") ft = t_bool();
            else {
                err(f.line, f.col, "struct field type must be 'int', 'float' or 'bool'");
                continue;
            }
            const int64_t sz = ft->kind == TyKind::Bool ? 1 : 8;
            const int64_t al = sz;
            off = (off + al - 1) / al * al;
            StructFieldInfo sfi;
            sfi.name = f.name;
            sfi.ty = ft;
            sfi.offset = off;
            sfi.size = sz;
            sfi.align = al;
            info->fields.push_back(std::move(sfi));
            off += sz;
            if (al > max_align) max_align = al;
        }
        info->align = max_align;
        info->size = (off + max_align - 1) / max_align * max_align;
        StructInfo* raw = info.get();
        all_structs_.push_back(std::move(info));
        structs_[s.name] = raw;
        module_->vars[s.name] = Symbol{mk_struct(raw), s.line, true, false};
    }

StructInfo* TypeChecker::build_cstruct(const CStruct& cs) {
        auto info = std::make_unique<StructInfo>();
        info->name = cs.name;
        info->size = cs.size;
        info->align = cs.align;
        info->c_struct = true;
        info->c_union = cs.is_union;
        for (auto& f : cs.fields) {
            TypePtr ft = t_int();
            bool unsig = false;
            if (!f.ptr_struct.empty()) {
                // 元素/字段是 .cpya 里声明的 struct/union（按值或 ptr[...]）。
                StructInfo* est = struct_info(f.ptr_struct);
                if (!est) continue;  // 依赖序保证不会发生
                ft = f.kind == CKind::Ptr ? mk_optional(mk_struct_ptr(est)) : mk_struct(est);
                StructFieldInfo sfi;
                sfi.name = f.name;
                sfi.ty = ft;
                sfi.offset = f.offset;
                sfi.size = f.size;
                sfi.align = f.align;
                sfi.dims = f.dims;
                sfi.c_elem = CKind::Struct;
                info->fields.push_back(std::move(sfi));
                continue;
            }
            switch (f.kind) {
                case CKind::Int:
                case CKind::Int32: ft = t_int(); break;
                case CKind::UInt32: ft = t_int(); unsig = true; break;
                case CKind::Int8: ft = t_i8(); break;
                case CKind::Int16: ft = t_i16(); break;
                case CKind::UInt8: ft = t_u8(); unsig = true; break;
                case CKind::UInt16: ft = t_u16(); unsig = true; break;
                case CKind::UInt64: ft = t_u64(); unsig = true; break;
                case CKind::Float: ft = t_float(); break;
                case CKind::Float32: ft = t_float(); break;
                case CKind::Bool: ft = t_bool(); break;
                case CKind::Ptr: ft = mk_optional(t_int()); break;
                default: break;
            }
            StructFieldInfo sfi;
            sfi.name = f.name;
            sfi.ty = ft;
            sfi.offset = f.offset;
            sfi.size = f.size;
            sfi.align = f.align;
            sfi.c_unsigned = unsig;
            sfi.dims = f.dims;
            sfi.c_elem = f.dims.empty() ? CKind::None : f.kind;
            if (f.kind == CKind::Ptr && f.ptr_elem != CKind::None) sfi.c_elem = f.ptr_elem;
            info->fields.push_back(std::move(sfi));
        }
        StructInfo* raw = info.get();
        all_structs_.push_back(std::move(info));
        return raw;
    }

bool TypeChecker::block_exits(const std::vector<StmtPtr>& body) {
    if (body.empty()) return false;
    auto& last = *body.back();
    switch (last.kind) {
        case StmtKind::Return:
        case StmtKind::Raise:
        case StmtKind::Break:
        case StmtKind::Continue:
            return true;
        case StmtKind::If: {
            auto& f = static_cast<If&>(last);
            // 没有 else 的 if 不算离开（可能整条都不执行）
            if (f.orelse.empty()) return false;
            for (auto& b : f.branches)
                if (!block_exits(b.body)) return false;
            return block_exits(f.orelse);
        }
        default: return false;
    }
}

void TypeChecker::stmts(std::vector<StmtPtr>& body) {
    // 提前退出的 None 守卫：`if x is None: return` 之后，x 一定不是 None。
    // 收窄要保持到块末，所以在块级别统一记录、退出时恢复。
    std::vector<Narrowing> active;
    for (auto& s : body) {
        if (s->kind == StmtKind::If) {
            auto& f = static_cast<If&>(*s);
            stmt(*s);
            // 单分支且分支必然离开（return/raise/continue/break），
            // 后面的代码只可能在条件为假时到达
            if (f.orelse.empty() && f.branches.size() == 1 &&
                block_exits(f.branches[0].body)) {
                // 提前退出后按「条件为假」窄化；`A or B` 为假即两者都假，
                // 所以这里要收全部（narrow_all），只用第一条会漏掉剩下的。
                for (auto& n : narrow_all(*f.branches[0].cond, false))
                    if (n.sym || !n.akey.empty()) active.push_back(n);
            }
            continue;
        }
        stmt(*s);
    }
    for (auto& n : active) restore_narrowing(n);
}

void TypeChecker::stmt(Stmt& s) {
        ++n_stmt;
        switch (s.kind) {
            case StmtKind::Expr:
                expr(*static_cast<ExprStmt&>(s).value);
                break;
            case StmtKind::Assign:
                stmt_assign(static_cast<Assign&>(s));
                break;
            case StmtKind::AugAssign:
                stmt_aug_assign(static_cast<AugAssign&>(s));
                break;
            case StmtKind::AnnAssign:
                stmt_ann_assign(static_cast<AnnAssign&>(s));
                break;
            case StmtKind::Return:
                stmt_return(static_cast<Return&>(s));
                break;
            case StmtKind::If:
                stmt_if(static_cast<If&>(s));
                break;
            case StmtKind::While:
                stmt_while(static_cast<While&>(s));
                break;
            case StmtKind::For:
                stmt_for(static_cast<For&>(s));
                break;
            case StmtKind::FunctionDef:
                funcdef(static_cast<FunctionDef&>(s));
                break;
            case StmtKind::ClassDef:
                classdef(static_cast<ClassDef&>(s));
                break;
            case StmtKind::StructDef:
                // 布局与登记已在 collect_toplevel 完成。
                break;
            case StmtKind::Try:
                stmt_try(static_cast<Try&>(s));
                break;
            case StmtKind::With:
                stmt_with(static_cast<With&>(s));
                break;
            case StmtKind::Raise:
                stmt_raise(static_cast<Raise&>(s));
                break;
            case StmtKind::Assert:
                stmt_assert(static_cast<Assert&>(s));
                break;
            case StmtKind::Import:
                stmt_import(static_cast<Import&>(s));
                break;
            case StmtKind::CImport:
                stmt_cimport(static_cast<CImport&>(s));
                break;
            case StmtKind::ImportFrom:
                stmt_import_from(static_cast<ImportFrom&>(s));
                break;
            case StmtKind::Global: {
                for (auto& n : static_cast<Global&>(s).names) global_decl_.insert(n);
                break;
            }
            case StmtKind::Nonlocal: {
                auto& nl = static_cast<Nonlocal&>(s);
                for (auto& n : nl.names) {
                    nonlocal_decl_.insert(n);
                    if (cur_fn_def_) free_refs_[cur_fn_def_].insert(n);
                    bool found = false;
                    if (cur_fn_scope_) {
                        for (Scope* sc = cur_fn_scope_->parent; sc && !sc->is_module; sc = sc->parent)
                            if (sc->vars.count(n)) { found = true; break; }
                    }
                    if (!found) err(s.line, s.col, std::format("no binding for nonlocal '{}' found", n));
                }
                break;
            }
            case StmtKind::Delete:
                stmt_delete(static_cast<Delete&>(s));
                break;
            case StmtKind::Match: {
                auto& m = static_cast<Match&>(s);
                TypePtr st = expr(*m.subject);
                std::unordered_map<std::string, TypePtr> merged;
                for (auto& c : m.cases) {
                    auto sc = std::make_unique<Scope>();
                    sc->parent = cur_;
                    cur_ = sc.get();
                    pool_.push_back(std::move(sc));
                    match_pattern(*c.pattern, st);
                    if (c.guard) expr(*c.guard);
                    stmts(c.body);
                    std::vector<std::string> names;
                    pattern_capture_names(*c.pattern, names);
                    for (auto& n : names) {
                        auto vit = cur_->vars.find(n);
                        if (vit == cur_->vars.end()) continue;
                        TypePtr t = vit->second.ty;
                        auto mit = merged.find(n);
                        if (mit == merged.end()) merged.emplace(n, t);
                        else mit->second = join(mit->second, t);
                    }
                    cur_ = cur_->parent;
                }
                // Python 的 match 捕获就是普通赋值：case 结束后仍在当前
                // 作用域可见。这里把各分支的类型 join 起来再声明一次。
                for (auto& [name, ty] : merged) declare(name, ty, m.line, m.col);
                match_capture_types_[&m] = std::move(merged);
                break;
            }
            case StmtKind::Break:
            case StmtKind::Continue:
            case StmtKind::Pass:
                break;
                }
    }

Tok TypeChecker::aug_to_bin(Tok t) {
        switch (t) {
            case Tok::PLUSEQUAL: return Tok::PLUS;
            case Tok::MINUSEQUAL: return Tok::MINUS;
            case Tok::STAREQUAL: return Tok::STAR;
            case Tok::SLASHEQUAL: return Tok::SLASH;
            case Tok::DOUBLESLASHEQUAL: return Tok::DOUBLESLASH;
            case Tok::PERCENTEQUAL: return Tok::PERCENT;
            case Tok::DOUBLESTAREQUAL: return Tok::DOUBLESTAR;
            case Tok::AMPEREQUAL: return Tok::AMPER;
            case Tok::VBAREQUAL: return Tok::VBAR;
            case Tok::CIRCUMFLEXEQUAL: return Tok::CIRCUMFLEX;
            case Tok::LSHIFTEQUAL: return Tok::LSHIFT;
            case Tok::RSHIFTEQUAL: return Tok::RSHIFT;
            default: return Tok::PLUS;
        }
    }


bool TypeChecker::maybe_none(const Expr* e, const TypePtr& t, int line, int col) {
    if (!t || t->kind != TyKind::Optional) return false;
    if (auto* n = e ? dynamic_cast<const Name*>(e) : nullptr)
        err(line, col, std::format("'{}' may be None here; check it first "
                                   "(if {} is not None:) or use assert",
                                   n->id, n->id));
    else
        err(line, col, "this value may be None here; check it first "
                        "(if x is not None:) or use assert");
    return true;
}

// 从条件表达式里取「可窄化的名字」：裸名字，或 walrus 的被赋值目标
// （`if (n := d.get(k)) is not None:` 之后要窄化的是 n）。
Name* TypeChecker::narrow_name_of(Expr* e) {
    if (!e) return nullptr;
    if (auto* n = dynamic_cast<Name*>(e)) return n;
    if (auto* w = dynamic_cast<NamedExpr*>(e))
        return w->target ? dynamic_cast<Name*>(w->target.get()) : nullptr;
    return nullptr;
}

std::vector<TypeChecker::Narrowing> TypeChecker::narrow_all(Expr& cond, bool truthy) {
    std::vector<Narrowing> out;
    // `A and B` 为真 ⇒ A、B 都为真（短路窄化要叠加）；`A or B` 为假 ⇒ 两者都假。
    if (cond.kind == ExprKind::BoolOp) {
        auto& b = static_cast<BoolOp&>(cond);
        const bool conj = b.op == Tok::KW_AND;
        if (conj == truthy) {
            for (auto& v : b.values) {
                auto sub = narrow_all(*v, truthy);
                for (auto& n : sub) out.push_back(n);
            }
            return out;
        }
        return out;  // 混合极性（and 为假 / or 为真）不窄化，保守
    }
    auto one = narrow_on(cond, truthy);
    if (one.sym) out.push_back(one);
    return out;
}

// isinstance 窄化：真值分支取 cls（联合里必须至少有一个与之兼容的类成员），
// 假值分支把「cls 或其子类」的成员去掉。判不准返回 nullptr（保持原类型）。
TypePtr TypeChecker::narrow_isinstance(const TypePtr& t, const ClassInfo* cls, bool truthy) {
    if (!t || !cls) return nullptr;
    std::vector<TypePtr> members;
    bool optional = false;
    if (t->kind == TyKind::Optional) {
        optional = true;
        if (t->sub.empty() || !t->sub[0]) return nullptr;
        union_flatten(t->sub[0], members);
    } else if (is_union_ty(t)) {
        union_flatten(t, members);
    } else {
        members.push_back(t);
    }
    if (members.empty()) return nullptr;
    auto exactly = [&](const TypePtr& m) {
        if (!m || m->kind != TyKind::Class || !m->cls) return false;
        return m->cls == cls || m->cls->is_subclass_of(cls);
    };
    auto compat = [&](const TypePtr& m) {
        if (!m || m->kind != TyKind::Class || !m->cls) return false;
        return m->cls == cls || m->cls->is_subclass_of(cls) || cls->is_subclass_of(m->cls);
    };
    if (truthy) {
        bool any = false;
        for (auto& m : members)
            if (compat(m)) any = true;
        if (!any) return nullptr;  // 联合里没有这个类：判不准，不动
        return mk_class(const_cast<ClassInfo*>(cls));
    }
    std::vector<TypePtr> keep;
    bool removed = false;
    for (auto& m : members) {
        if (exactly(m)) {
            removed = true;
            continue;
        }
        keep.push_back(m);
    }
    if (!removed || keep.empty()) return nullptr;  // 去掉就空了：判不准
    TypePtr acc = nullptr;
    for (auto& m : keep) acc = acc ? mk_union(acc, m) : m;
    if (!acc) return nullptr;
    return optional ? mk_optional(acc) : acc;
}

TypeChecker::Narrowing TypeChecker::narrow_on(Expr& cond, bool truthy) {
        auto non_none = [](const TypePtr& t) -> TypePtr {
            if (!t) return t;
            if (t->kind == TyKind::Optional)
                return t->sub.empty() ? t_unknown() : t->sub[0];
            if (t->kind == TyKind::None) return t_unknown();  // 分支实际不可达
            return t;
        };
        auto is_none_const = [](Expr* e) {
            auto* c = dynamic_cast<Constant*>(e);
            return c && c->ck == Constant::CK::None;
        };

        if (cond.kind == ExprKind::UnaryOp) {
            auto& u = static_cast<UnaryOp&>(cond);
            if (u.op == Tok::KW_NOT) return narrow_on(*u.operand, !truthy);
        }
        if (cond.kind == ExprKind::Compare) {
            auto& c = static_cast<Compare&>(cond);
            if (c.ops.size() == 1) {
                // `(n := expr) is not None`：walrus 也应该被窄化 —— 参与判断的
                // 是**被赋名的那个变量**（graphlib 的 nodeinfo 就是这种写法）。
                Name* name = narrow_name_of(c.left.get());
                bool none_right = is_none_const(c.comparators[0].get());
                if (!name && is_none_const(c.left.get()))
                    name = narrow_name_of(c.comparators[0].get());
                if (!name) {
                    // 属性收窄：`if self.x is not None:` → 之后 `self.x.foo()`。
                    // 只认单层「名字.属性」，键 action 存 attr_refine_。
                    Expr* oe = none_right ? c.left.get() : c.comparators[0].get();
                    auto* at = dynamic_cast<Attribute*>(oe);
                    auto* bn = at ? dynamic_cast<Name*>(at->value.get()) : nullptr;
                    if (at && bn) {
                        bool is_not2 = c.ops[0] == CmpOp::IsNot;
                        if (is_not2 != truthy) return {};
                        TypePtr cur = expr(*at);
                        Narrowing n;
                        n.akey = bn->id + "." + at->attr;
                        auto it2 = attr_refine_.find(n.akey);
                        n.had_old = it2 != attr_refine_.end();
                        n.old = n.had_old ? it2->second : nullptr;
                        n.now = non_none(cur);
                        attr_refine_[n.akey] = n.now;
                        return n;
                    }
                }
                if (name && (none_right || is_none_const(c.left.get()))) {
                    // 只做 "is not None" 方向的收窄。
                    // "is None" 分支里若把变量改成 None，`if x is None: x = {}`
                    // 这种合法写法反而会报错，收窄必须尊重原声明。
                    bool is_not = c.ops[0] == CmpOp::IsNot;
                    if (is_not != truthy) return {};
                    Symbol* sym = lookup(name->id);
                    if (!sym) return {};
                    Narrowing n{};
                    n.sym = sym;
                    n.old = sym->ty;
                    n.now = nullptr;
                    sym->ty = non_none(sym->ty);
                    n.now = sym->ty;
                    return n;
                }
            }
        }
        // isinstance(x, C)：真值分支把联合收窄成 C，假值分支把 C 去掉。
        // 只认「用户类对象 + 名字/walrus 目标」；内置类型、元组形式、属性
        // 目标一律不窄化（保守：判不准就不动，避免误杀）。
        if (cond.kind == ExprKind::Call) {
            auto& call = static_cast<Call&>(cond);
            auto* cfn = dynamic_cast<Name*>(call.func.get());
            if (cfn && cfn->id == "isinstance" && !lookup("isinstance") &&
                call.keywords.empty() && call.args.size() == 2) {
                Name* tgt = narrow_name_of(call.args[0].get());
                TypePtr ct = expr(*call.args[1]);
                const ClassInfo* cls = nullptr;
                if (ct && ct->kind == TyKind::ClassObj && !ct->sub.empty() && ct->sub[0] &&
                    ct->sub[0]->kind == TyKind::Class)
                    cls = ct->sub[0]->cls;
                Symbol* sym = tgt ? lookup(tgt->id) : nullptr;
                if (cls && sym && sym->ty) {
                    TypePtr narrowed = narrow_isinstance(sym->ty, cls, truthy);
                    if (narrowed && !type_eq(narrowed, sym->ty)) {
                        Narrowing n{};
                        n.sym = sym;
                        n.old = sym->ty;
                        sym->ty = narrowed;
                        n.now = narrowed;
                        return n;
                    }
                }
            }
        }
        if ((cond.kind == ExprKind::Name || cond.kind == ExprKind::NamedExpr) && truthy) {
            Name* nn = narrow_name_of(&cond);
            if (!nn) return {};
            auto& n = *nn;
            if (Symbol* sym = lookup(n.id)) {
                Narrowing r{};
                r.sym = sym;
                r.old = sym->ty;
                r.now = nullptr;
                sym->ty = non_none(sym->ty);
                r.now = sym->ty;
                return r;
            }
        }
        return {};
    }

    void TypeChecker::restore_narrowing(const Narrowing& n) {
        if (!n.akey.empty()) {
            if (n.had_old) attr_refine_[n.akey] = n.old;
            else attr_refine_.erase(n.akey);
            return;
        }
        // 块内可能已经重新赋值（类型变了），这时不该把类型恢复成收窄前的
        if (!n.sym || n.sym->ty != n.now) return;
        n.sym->ty = n.old;
    }

bool TypeChecker::target_touches_globals(Expr& target) const {
    const Expr* p = &target;
    while (p) {
        switch (p->kind) {
            case ExprKind::Subscript: p = static_cast<const Subscript*>(p)->value.get(); break;
            case ExprKind::Attribute: p = static_cast<const Attribute*>(p)->value.get(); break;
            case ExprKind::Call: {
                auto& c = *static_cast<const Call*>(p);
                auto* n = dynamic_cast<const Name*>(c.func.get());
                return n && n->id == "globals";
            }
            case ExprKind::Name: {
                auto* n = static_cast<const Name*>(p);
                return globals_aliases_.count(n->id) > 0;
            }
            default: return false;
        }
    }
    return false;
}

void TypeChecker::assign(Expr& target, TypePtr value, int line, int col) {
        // globals()[name] = ... 之后模块级名字集合就不可知了
        if ((target.kind == ExprKind::Subscript || target.kind == ExprKind::Attribute) &&
            target_touches_globals(target))
            dynamic_globals_ = true;
        if (auto* n = dynamic_cast<Name*>(&target)) {
            declare(n->id, value, line, col);
            return;
        }
        if (auto* tup = dynamic_cast<TupleExpr*>(&target)) {
            unpack_assign(tup->elts, value, line, col);
            return;
        }
        if (auto* lst = dynamic_cast<ListExpr*>(&target)) {
            unpack_assign(lst->elts, value, line, col);
            return;
        }
        if (auto* st = dynamic_cast<Starred*>(&target)) {
            assign(*st->value, value, line, col);
            return;
        }
        if (auto* sub = dynamic_cast<Subscript*>(&target)) {
            auto ot = expr(*sub->value);
            auto st = expr(*sub->slice);
            if (sub->slice->kind == ExprKind::Slice) {
                // 切片赋值：运行时展开，但**元素类型必须检查**。
                //
                // 原实现只确认「对象可变」就放过，于是
                //     l: list[int] = [1, 2]
                //     l[0:1] = ['hello']
                // 编译通过，运行时 pa_list_slice_assign 把字符串指针写进
                // 一个声明为 int 的列表；后续 l[0] 按 int 读出来就是垃圾
                // （实测输出 96042627193968 这种值，每次运行都不同）。
                //
                // 检查方式与单元素赋值（下面的 merge_elem）保持一致：
                // 值是序列就逐个元素对；不是序列就整体对。
                auto& sl = static_cast<SliceExpr&>(*sub->slice);
                if (sl.lower) expr(*sl.lower);
                if (sl.upper) expr(*sl.upper);
                if (sl.step) expr(*sl.step);
                if (ot->kind == TyKind::List && ot->sub.size() >= 1 &&
                    !is_unknown(value) && value->kind == TyKind::List &&
                    value->sub.size() >= 1 && !is_unknown(value->sub[0])) {
                    // list 切片的右值也是 list：比较元素类型。
                    if (!merge_elem(ot->sub[0], ot, value->sub[0], ot->locked))
                        err(line, col,
                            std::format("cannot assign '{}' to slice of '{}'",
                                        type_name(value), type_name(ot)));
                }
                if (ot->kind == TyKind::Class) {
                    if (ot->cls && !ot->cls->is_open() &&
                        !ot->cls->method("__setitem__"))
                        err(line, col,
                            std::format("'{}' object does not support item assignment",
                                        bare_type_name(ot)));
                } else if (!is_unknown(ot) && ot->kind != TyKind::List &&
                           ot->kind != TyKind::Optional)
                    err(line, col,
                        std::format("'{}' object does not support item assignment",
                                    bare_type_name(ot)));
                return;
            }
            if (ot->kind == TyKind::Optional && ot->celem != CKind::None && !ot->sub.empty() &&
                ot->sub[0]->kind == TyKind::Int) {
                if (!is_unknown(st) && !is_int_like(st))
                    err(line, col, "pointer index must be an integer");
                TypePtr want = cptr_elem_type(ot->celem);
                if (!assignable(want, value))
                    err(line, col,
                        std::format("pointer element has type '{}', cannot assign '{}'",
                                    type_name(want), type_name(value)));
                return;
            }
            if (ot->kind == TyKind::List) {
                if (!merge_elem(ot->sub[0], ot, value, ot->locked))
                    err(line, col,
                        std::format("cannot store '{}' into '{}'", type_name(value),
                                    type_name(ot)));
            } else if (ot->kind == TyKind::Dict) {
                // 定宽键 + 字面量下标（写路径）：与读路径/建表同口径 coerce。
                if (is_fixed_int(ot->sub[0]) && accept_literal_into(ot->sub[0], sub->slice.get()))
                    st = ot->sub[0];
                if (!merge_elem(ot->sub[0], ot, st, ot->locked))
                    err(line, col,
                        std::format("dict key type is '{}', got '{}'", type_name(ot->sub[0]),
                                    type_name(st)));
                if (!merge_elem(ot->sub[1], ot, value, ot->locked))
                    err(line, col,
                        std::format("cannot store '{}' into '{}'", type_name(value),
                                    type_name(ot)));
            } else if (ot->kind == TyKind::Class) {
                // 类可能实现了 __setitem__（自己的或继承来的）。实现了就
                // 放过；基类来自未知来源（is_open）时静态无从判断，也放过。
                // 只有能确定「这个类及其祖先都没有 __setitem__」时才报错——
                // 否则降级阶段会把对象当 list 解引用，运行期直接段错误。
                if (ot->cls && !ot->cls->is_open() &&
                    !ot->cls->method("__setitem__"))
                    err(line, col,
                        std::format("'{}' object does not support item assignment",
                                    bare_type_name(ot)));
            } else if (!is_unknown(ot) && ot->kind != TyKind::Optional) {
                // 措辞跟 CPython 一致：'range' object does not support item assignment
                err(line, col, std::format("'{}' object does not support item assignment",
                                           bare_type_name(ot)));
            }
            return;
        }
        if (auto* at = dynamic_cast<Attribute*>(&target)) {
            if (auto* vn = dynamic_cast<Name*>(at->value.get())) {
                if (auto mod = lookup_module_alias(vn->id)) {
                    if (mod->cpy_proxy) {
                        err(line, col,
                            std::format("cannot assign to attribute of CPython module proxy '{}'",
                                        vn->id));
                        return;
                    }
                    auto it = mod->globals.find(at->attr);
                    if (it == mod->globals.end()) {
                        err(line, col,
                            std::format("module '{}' has no attribute '{}'", vn->id, at->attr));
                        return;
                    }
                    if (!assignable(it->second, value))
                        err(line, col,
                            std::format("module attribute '{}.{}' has type '{}', cannot assign '{}'",
                                        vn->id, at->attr, type_name(it->second),
                                        type_name(value)));
                    return;
                }
            }
            if (auto* vn = dynamic_cast<Name*>(at->value.get())) {
                if (const CImportDecl* cd = cmodule(vn->id)) {
                    for (auto& cv : cd->vars) {
                        if (cv.py_name != at->attr) continue;
                        if (cv.kind == CKind::Str || cv.kind == CKind::OptStr) {
                            err(line, col,
                                std::format("cannot assign to C string variable '{}.{}'",
                                            vn->id, at->attr));
                            return;
                        }
                        TypePtr want = ckind_type(cv.kind);
                        if (cv.kind == CKind::Ptr && !cv.struct_name.empty()) {
                            if (StructInfo* st = struct_info(cv.struct_name))
                                want = mk_optional(mk_struct_ptr(st));
                        }
                        if (!assignable(want, value))
                            err(line, col,
                                std::format("C variable '{}.{}' has type '{}', cannot assign '{}'",
                                            vn->id, at->attr, type_name(want),
                                            type_name(value)));
                        return;
                    }
                    err(line, col,
                        std::format("cannot assign to cimport attribute '{}.{}'", vn->id,
                                    at->attr));
                    return;
                }
            }
            auto ot = expr(*at->value);
            if (ot->kind == TyKind::Func) return;
            if (ot->kind == TyKind::Optional && !ot->sub.empty()) {
                TyKind ik = ot->sub[0]->kind;
                if (ik == TyKind::Class || ik == TyKind::Struct || ik == TyKind::StructPtr)
                    ot = ot->sub[0];
            }
            if (ot->kind == TyKind::Struct || ot->kind == TyKind::StructPtr) {
                if (!ot->st) return;
                const StructFieldInfo* f = ot->st->field(at->attr);
                if (!f) {
                    err(line, col, std::format("struct '{}' has no field '{}'", ot->st->name,
                                               at->attr));
                    return;
                }
                if (f->is_array()) {
                    err(line, col,
                        std::format("cannot assign to array field '{}' as a whole; assign its "
                                    "elements instead", at->attr));
                    return;
                }
                if (!assignable(f->ty, value))
                    err(line, col,
                        std::format("field '{}' has type '{}', cannot assign '{}'", at->attr,
                                    type_name(f->ty), type_name(value)));
                return;
            }
            if (ot->kind == TyKind::ClassObj && !ot->sub.empty()) ot = ot->sub[0];
            if (ot->kind == TyKind::Class) {
                if (!ot->cls) return;
                if (!ot->cls->complete && ot->cls != cur_class_) return;  // 骨架，无从判断
                if (const PropertyInfo* p = ot->cls->property(at->attr)) {
                    if (!p->owner || p->setter < 0 ||
                        p->setter >= static_cast<int>(p->owner->methods.size())) {
                        err(line, col,
                            std::format("property '{}' has no setter", at->attr));
                        return;
                    }
                    const MethodInfo& sm = p->owner->methods[p->setter];
                    TypePtr vt = sm.call_sig && !sm.call_sig->pos.empty()
                                     ? sm.call_sig->pos[0].ty
                                     : t_unknown();
                    if (!is_unknown(vt) && !assignable(vt, value))
                        err(line, col,
                            std::format("property '{}' setter cannot accept '{}'", at->attr,
                                        type_name(value)));
                    return;
                }
                // `C.x = v`：改的是这个类自己的那份类属性存储
                if (is_class_receiver(*at)) {
                    if (const ClassAttrInfo* ca = ot->cls->class_attr(at->attr)) {
                        if (ca->ty && ca->ty->kind != TyKind::None &&
                            !is_unknown(ca->ty) && !assignable(ca->ty, value))
                            err(line, col,
                                std::format("class attribute '{}.{}' has type '{}', cannot "
                                            "assign '{}'",
                                            ot->cls->name, at->attr, type_name(ca->ty),
                                            type_name(value)));
                        // 继承来的：本类从此有自己的一份，基类那份不动
                        if (ca->owner != ot->cls) ot->cls->cowed_attrs.insert(at->attr);
                        return;
                    }
                }
                // `self.x = ...` 且 x 没声明过：就地补成字段，类型不再苛求
                if (add_implicit_field(*at, value)) return;
                if (auto* f = ot->cls->field(at->attr)) { mark_self_field(*at);  // 3.9
                    // self.d = {}：空字面量的洞按字段标注填（否则运行时
                    // 按 unknown 种类建表，str 键变指针哈希）。
                    if (value && ty_contains_hole(value, 0))
                        fill_holes_recursive(value, f->ty, 0);
                    if (!assignable(f->ty, value))
                        err(line, col,
                            std::format("field '{}' has type '{}', cannot assign '{}'", at->attr,
                                        type_name(f->ty), type_name(value)));
                    return;
                }
                if (ot->cls->method(at->attr)) {
                    // `c.__annotations__ = d` 这类：dunder 是协议，静态不管
                    if (at->attr.size() > 4 && at->attr.starts_with("__") &&
                        at->attr.ends_with("__"))
                        return;
                    // `self.x = v` 可能走 @property 的 setter，静态分不清
                    if (is_self_attr(*at)) {
                        unsupported.push_back("assignment to a property setter");
                        return;
                    }
                    // `obj.method = fn` 动态覆盖方法（monkey-patch）合法；
                    // 但赋非函数值（`b.f = 1`）是明确错误，保留报错。
                    if (value && value->kind == TyKind::Func) {
                        unsupported.push_back("assignment to a method name");
                        return;
                    }
                    err(line, col, std::format("cannot assign to method '{}'", at->attr));
                    return;
                }
                // 外部基类可能带来任意属性
                if (ot->cls->is_open()) return;
                // 实例上给类属性赋值要新建实例属性，而 PyAdamas 的对象布局是
                // 编译期定死的；明确报错，别静默写到类属性上影响所有实例。
                if (ot->cls->class_attr(at->attr)) {
                    // CPython 给实例赋值同名会新建实例属性覆盖类属性；布局固定
                    // 但这是合法 Python，check 保守放过，build 决定。
                    unsupported.push_back("assignment to a class attribute through an instance");
                    return;
                }
                // 动态属性已开启：obj.x = v 落运行期 dict（AttrSet）。
                if (ot->cls->needs_dict) return;
                err(line, col,
                    std::format("'{}' object has no attribute '{}'", ot->cls->name, at->attr));
                return;
            }
            // 异常值的内置属性（__context__/__cause__/__traceback__/filename/
            // errno 等）静态看得到一部分，但赋值是常见写法；conservative 放过。
            if (ot && ot->kind == TyKind::Exception) {
                if (at->attr.size() > 4 && at->attr.starts_with("__") &&
                    at->attr.ends_with("__"))
                    return;
                unsupported.push_back("assignment to an exception attribute");
                return;
            }
            if (ot && ot->kind == TyKind::TypeObj) {
                unsupported.push_back("assignment to a type object attribute");
                return;
            }
            if (!is_unknown(ot))
                err(line, col,
                    std::format("'{}' has no assignable attribute '{}'", type_name(ot), at->attr));
            return;
        }
        err(line, col, "cannot assign to this expression");
    }

bool TypeChecker::unpack_types(const std::vector<ExprPtr>& targets, TypePtr value, int line,
                               int col, std::vector<TypePtr>& out, bool quiet) {
        size_t n = targets.size();
        out.assign(n, t_unknown());
        size_t star_at = n;  // n 表示没有星号目标
        for (size_t i = 0; i < n; ++i)
            if (targets[i]->kind == ExprKind::Starred) { star_at = i; break; }

        if (value->kind == TyKind::Tuple) {
            if (value->variadic) {
                // 长度不定：每个目标拿元素类型，星号目标拿 list[元素]
                TypePtr e = value->sub.empty() ? t_unknown() : value->sub[0];
                for (size_t i = 0; i < n; ++i)
                    out[i] = targets[i]->kind == ExprKind::Starred ? mk_list(e) : e;
                return true;
            }
            auto& elems = value->sub;
            if (star_at == n) {
                if (!elems.empty() && elems.size() != n) {
                    if (!quiet)
                        err(line, col,
                            std::format("cannot unpack {} values into {}", elems.size(), n));
                    return false;
                }
                for (size_t i = 0; i < n; ++i)
                    out[i] = i < elems.size() ? elems[i] : t_unknown();
                return true;
            }
            // a, *b, c = (...)：星号吃中间，剩余目标与末尾元素对齐
            size_t tail = n - star_at - 1;
            if (elems.size() + 1 < n) {
                if (!quiet)
                    err(line, col,
                        std::format("cannot unpack {} values into {}", elems.size(), n));
                return false;
            }
            for (size_t i = 0; i < star_at; ++i)
                out[i] = i < elems.size() ? elems[i] : t_unknown();
            TypePtr rest = nullptr;
            for (size_t i = star_at; i + tail < elems.size(); ++i)
                rest = rest ? join(rest, elems[i]) : elems[i];
            out[star_at] = mk_list(rest ? rest : t_unknown());
            for (size_t j = 0; j < tail; ++j)
                out[star_at + 1 + j] = elems[elems.size() - tail + j];
            return true;
        }

        if (value->kind == TyKind::List || value->kind == TyKind::Set ||
            value->kind == TyKind::Generator || value->kind == TyKind::Range ||
            value->kind == TyKind::Str || value->kind == TyKind::Exception ||
            value->kind == TyKind::Dict) {
            // 只记录元素类型、不记录长度，逐个赋元素类型。
            // generator/range 是惰性可迭代；str 解包到单字符。
            TypePtr elem = value->kind == TyKind::Str ? t_str()
                          : value->sub.empty()        ? t_unknown()
                                                      : value->sub[0];
            for (size_t i = 0; i < n; ++i)
                out[i] = targets[i]->kind == ExprKind::Starred ? mk_list(elem) : elem;
            return true;
        }

        // 类可能实现了 __iter__（静态看不到），None/unknown 无从判断：
        // 都按 unknown 逐个赋，不报错。
        if (is_unknown(value) || value->kind == TyKind::None ||
            value->kind == TyKind::Class) {
            return true;  // out 已全是 unknown
        }
        if (value->kind == TyKind::Optional && !value->sub.empty()) {
            // a, b = maybe_tuple：拆 Optional 内层，None 走 unknown。
            TypePtr inner = value->sub[0];
            if (inner->kind == TyKind::Tuple || inner->kind == TyKind::List ||
                inner->kind == TyKind::Str || inner->kind == TyKind::Generator) {
                return true;
            }
        }
        if (!quiet)
            err(line, col, std::format("cannot unpack non-sequence '{}'", type_name(value)));
        return false;
}

void TypeChecker::unpack_assign(std::vector<ExprPtr>& targets, TypePtr value, int line, int col) {
        std::vector<TypePtr> tys;
        if (!unpack_types(targets, value, line, col, tys, false)) return;
        for (size_t i = 0; i < targets.size(); ++i) {
            if (targets[i]->kind == ExprKind::Starred) {
                assign(*static_cast<Starred&>(*targets[i]).value, tys[i], line, col);
            } else {
                assign(*targets[i], tys[i], line, col);
            }
        }
}

void TypeChecker::collect_target_types(Expr& target, TypePtr value,
                                       std::vector<std::pair<std::string, TypePtr>>& out) {
        if (target.kind == ExprKind::Name) {
            out.emplace_back(static_cast<Name&>(target).id, value);
            return;
        }
        if (target.kind == ExprKind::Starred) {
            collect_target_types(*static_cast<Starred&>(target).value, value, out);
            return;
        }
        if (target.kind == ExprKind::Tuple || target.kind == ExprKind::List) {
            auto& elts = target.kind == ExprKind::Tuple
                             ? static_cast<TupleExpr&>(target).elts
                             : static_cast<ListExpr&>(target).elts;
            std::vector<TypePtr> tys;
            if (!unpack_types(elts, value, target.line, target.col, tys, true)) return;
            for (size_t i = 0; i < elts.size(); ++i)
                if (elts[i]) collect_target_types(*elts[i], tys[i], out);
        }
        // 下标/属性目标不绑定新名字，跳过
}



static void pattern_capture_names(const Pattern& p, std::vector<std::string>& out) {
    switch (p.kind) {
        case PatternKind::Capture:
            if (!p.name.empty() && p.name != "_") out.push_back(p.name);
            return;
        case PatternKind::As:
            if (!p.name.empty() && p.name != "_") out.push_back(p.name);
            for (auto& it : p.items) pattern_capture_names(*it, out);
            return;
        case PatternKind::Sequence:
        case PatternKind::Or:
            for (auto& it : p.items) pattern_capture_names(*it, out);
            return;
        case PatternKind::Mapping:
            for (auto& kv : p.pairs) pattern_capture_names(*kv.second, out);
            if (!p.rest.empty() && p.rest != "_") out.push_back(p.rest);
            return;
        case PatternKind::Class:
            for (auto& it : p.positional) pattern_capture_names(*it, out);
            for (auto& kv : p.attrs) pattern_capture_names(*kv.second, out);
            return;
        default:
            return;
    }
}

void TypeChecker::match_pattern(Pattern& p, TypePtr subject,
                                const std::unordered_set<std::string>* or_names) {
    auto declare_capture = [&](const std::string& name, TypePtr ty) {
        if (name.empty() || name == "_") return;
        if (or_names && or_names->count(name)) return;  // 已由 OR 统一声明
        if (cur_->vars.count(name)) return;             // 同一模式重复捕获
        declare(name, std::move(ty), p.line, p.col);
    };
    switch (p.kind) {
        case PatternKind::Literal:
            expr(*p.lit);
            return;
        case PatternKind::Value:
            expr(*p.lit);
            return;
        case PatternKind::Capture:
            declare_capture(p.name, subject);
            return;
        case PatternKind::Wildcard:
            return;
        case PatternKind::As: {
            for (auto& it : p.items) match_pattern(*it, subject, or_names);
            declare_capture(p.name, subject);
            return;
        }
        case PatternKind::Or: {
            std::vector<std::set<std::string>> sets;
            for (auto& it : p.items) {
                std::vector<std::string> names;
                pattern_capture_names(*it, names);
                sets.emplace_back(names.begin(), names.end());
            }
            for (size_t i = 1; i < sets.size(); ++i) {
                if (sets[i] != sets[0]) {
                    err(p.line, p.col,
                        "alternative patterns bind different names");
                    break;
                }
            }
            std::unordered_set<std::string> names(sets[0].begin(), sets[0].end());
            for (auto& n : names)
                if (!cur_->vars.count(n)) declare(n, t_unknown(), p.line, p.col);
            for (auto& it : p.items) match_pattern(*it, subject, &names);
            return;
        }
        case PatternKind::Sequence: {
            TypePtr t = subject;
            if (t && t->kind == TyKind::Optional && !t->sub.empty()) t = t->sub[0];
            if (t && t->kind == TyKind::Optional) {
                err(p.line, p.col, "sequence pattern cannot match Optional");
                return;
            }
            TypePtr elem = t_unknown();
            bool str_seq = t && t->kind == TyKind::Str;
            if (t && t->kind == TyKind::List) elem = t->sub.empty() ? t_unknown() : t->sub[0];
            else if (t && t->kind == TyKind::Str) {
                err(p.line, p.col, "sequence pattern cannot match str");
                return;
            } else if (t && t->kind == TyKind::Tuple) elem = t_unknown();
            else if (t && t->kind != TyKind::Unknown) {
                err(p.line, p.col,
                    std::format("sequence pattern cannot match '{}'", type_name(t)));
                return;
            }
            for (size_t i = 0; i < p.items.size(); ++i) {
                if (static_cast<int>(i) == p.star) {
                    if (p.items[i]->kind == PatternKind::Capture)
                        declare_capture(p.items[i]->name, mk_list(elem));
                    continue;
                }
                TypePtr et = elem;
                if (t && t->kind == TyKind::Tuple && !str_seq) {
                    et = i < t->sub.size() ? t->sub[i] : t_unknown();
                }
                match_pattern(*p.items[i], et, or_names);
            }
            return;
        }
        case PatternKind::Mapping: {
            TypePtr t = subject;
            if (t && t->kind == TyKind::Optional && !t->sub.empty()) t = t->sub[0];
            TypePtr kt = t_unknown();
            TypePtr val = t_unknown();
            if (t && t->kind == TyKind::Dict) {
                if (!t->sub.empty()) kt = t->sub[0];
                if (t->sub.size() > 1) val = t->sub[1];
            } else if (t && t->kind != TyKind::Unknown) {
                err(p.line, p.col,
                    std::format("mapping pattern cannot match '{}'", type_name(t)));
                return;
            }
            for (auto& kv : p.pairs) {
                expr(*kv.first);
                match_pattern(*kv.second, val, or_names);
            }
            if (!p.rest.empty() && p.rest != "_")
                declare_capture(p.rest, mk_dict(kt, val));
            return;
        }
        case PatternKind::Class: {
            if (p.cls.find('.') != std::string::npos) {
                size_t dot = p.cls.rfind('.');
                std::string mod_name = p.cls.substr(0, dot);
                std::string cls_name = p.cls.substr(dot + 1);
                auto mod = lookup_module_alias(mod_name);
                if (!mod) {
                    err(p.line, p.col,
                        std::format("unknown module '{}' in class pattern", mod_name));
                    return;
                }
                auto ci = mod->classes.find(cls_name);
                if (ci == mod->classes.end()) {
                    err(p.line, p.col,
                        std::format("module '{}' has no class '{}'", mod_name, cls_name));
                    return;
                }
                p.qualname = mod->name + "." + cls_name;
                ClassInfo* pc = ci->second.cls.get();
                if (pc->is_exception_class) {
                    if (!p.positional.empty() || !p.attrs.empty())
                        err(p.line, p.col,
                            "exception class patterns cannot have sub-patterns");
                    return;
                }
                if (!pc->complete) {
                    err(p.line, p.col,
                        std::format("class '{}' must be fully defined before a class pattern "
                                    "uses its fields", p.cls));
                    return;
                }
                if (!p.positional.empty()) {
                    const auto* margs = pc->match_args_of();
                    if (!margs) {
                        err(p.line, p.col,
                            std::format("'{}' has no __match_args__ for positional patterns",
                                        pc->name));
                    } else {
                        if (p.positional.size() > margs->size())
                            err(p.line, p.col,
                                std::format("'{}' accepts {} positional sub-pattern{}, got {}",
                                            pc->name, margs->size(),
                                            margs->size() == 1 ? "" : "s",
                                            p.positional.size()));
                        size_t n = std::min(p.positional.size(), margs->size());
                        for (size_t i = 0; i < n; ++i) {
                            const FieldInfo* f = pc->field((*margs)[i]);
                            if (!f) {
                                err(p.line, p.col,
                                    std::format(
                                        "__match_args__ of '{}' names unknown field '{}'",
                                        pc->name, (*margs)[i]));
                                continue;
                            }
                            match_pattern(*p.positional[i], f->ty, or_names);
                        }
                    }
                }
                for (auto& kv : p.attrs) {
                    const FieldInfo* f = pc->field(kv.first);
                    if (!f) {
                        err(p.line, p.col,
                            std::format("'{}' object has no field '{}'", pc->name, kv.first));
                        continue;
                    }
                    match_pattern(*kv.second, f->ty, or_names);
                }
                return;
            }
            auto it = classes_.find(p.cls);
            if (it == classes_.end()) {
                if (is_exception_name(p.cls)) {
                    if (!p.positional.empty() || !p.attrs.empty())
                        err(p.line, p.col,
                            "exception class patterns cannot have sub-patterns");
                    return;
                }
                if (is_type_name(p.cls)) {
                    if (!p.positional.empty() || !p.attrs.empty())
                        err(p.line, p.col,
                            "builtin type patterns cannot have sub-patterns");
                    return;
                }
                err(p.line, p.col,
                    std::format("unknown class '{}' in class pattern", p.cls));
                return;
            }
            ClassInfo* pc = it->second.get();
            if (pc->is_exception_class) {
                if (!p.positional.empty() || !p.attrs.empty())
                    err(p.line, p.col,
                        "exception class patterns cannot have sub-patterns");
                return;
            }
            if (!pc->complete && pc != cur_class_ &&
                (!p.positional.empty() || !p.attrs.empty())) {
                err(p.line, p.col,
                    std::format("class '{}' must be fully defined before a class pattern "
                                "uses its fields", p.cls));
                return;
            }
            if (!p.positional.empty()) {
                const auto* margs = pc->match_args_of();
                if (!margs) {
                    err(p.line, p.col,
                        std::format("'{}' has no __match_args__ for positional patterns",
                                    pc->name));
                } else {
                    if (p.positional.size() > margs->size())
                        err(p.line, p.col,
                            std::format("'{}' accepts {} positional sub-pattern{}, got {}",
                                        pc->name, margs->size(),
                                        margs->size() == 1 ? "" : "s",
                                        p.positional.size()));
                    size_t n = std::min(p.positional.size(), margs->size());
                    for (size_t i = 0; i < n; ++i) {
                        const FieldInfo* f = pc->field((*margs)[i]);
                        if (!f) {
                            err(p.line, p.col,
                                std::format("__match_args__ of '{}' names unknown field '{}'",
                                            pc->name, (*margs)[i]));
                            continue;
                        }
                        match_pattern(*p.positional[i], f->ty, or_names);
                    }
                }
            }
            for (auto& kv : p.attrs) {
                const FieldInfo* f = pc->field(kv.first);
                if (!f) {
                    err(p.line, p.col,
                        std::format("'{}' object has no field '{}'", pc->name, kv.first));
                    continue;
                }
                match_pattern(*kv.second, f->ty, or_names);
            }
            return;
        }
    }
}

}  // namespace pyadamas
