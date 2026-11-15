// typecheck_stmt.cpp —— 语句类型检查：控制流、try/with、赋值与解包
#include "typecheck.h"

#include <filesystem>
#include <set>

#include "module.h"

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
            info->fields.push_back(StructFieldInfo{f.name, ft, off, sz, al});
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
        for (auto& f : cs.fields) {
            TypePtr ft = t_int();
            bool unsig = false;
            switch (f.kind) {
                case CKind::Int:
                case CKind::Int32: ft = t_int(); break;
                case CKind::UInt32: ft = t_int(); unsig = true; break;
                case CKind::Float: ft = t_float(); break;
                case CKind::Bool: ft = t_bool(); break;
                case CKind::Ptr: ft = mk_optional(t_int()); break;
                default: break;
            }
            info->fields.push_back(StructFieldInfo{f.name, ft, f.offset, f.size, f.align, unsig});
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
                auto n = narrow_on(*f.branches[0].cond, false);
                if (n.sym) active.push_back(n);
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
                Name* name = dynamic_cast<Name*>(c.left.get());
                bool none_right = is_none_const(c.comparators[0].get());
                if (!name && is_none_const(c.left.get()))
                    name = dynamic_cast<Name*>(c.comparators[0].get());
                if (name && (none_right || is_none_const(c.left.get()))) {
                    // 只做 "is not None" 方向的收窄。
                    // "is None" 分支里若把变量改成 None，`if x is None: x = {}`
                    // 这种合法写法反而会报错，收窄必须尊重原声明。
                    bool is_not = c.ops[0] == CmpOp::IsNot;
                    if (is_not != truthy) return {};
                    Symbol* sym = lookup(name->id);
                    if (!sym) return {};
                    Narrowing n{sym, sym->ty, nullptr};
                    sym->ty = non_none(sym->ty);
                    n.now = sym->ty;
                    return n;
                }
            }
        }
        if (cond.kind == ExprKind::Name && truthy) {
            auto& n = static_cast<Name&>(cond);
            if (Symbol* sym = lookup(n.id)) {
                Narrowing r{sym, sym->ty, nullptr};
                sym->ty = non_none(sym->ty);
                r.now = sym->ty;
                return r;
            }
        }
        return {};
    }

    void TypeChecker::restore_narrowing(const Narrowing& n) {
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
                // 切片赋值在运行时才展开，类型层面只确认对象是可变的
                auto& sl = static_cast<SliceExpr&>(*sub->slice);
                if (sl.lower) expr(*sl.lower);
                if (sl.upper) expr(*sl.upper);
                if (sl.step) expr(*sl.step);
                if (!is_unknown(ot) && ot->kind != TyKind::List &&
                    ot->kind != TyKind::Optional && ot->kind != TyKind::Class &&
                    ot->kind != TyKind::None)
                    err(line, col,
                        std::format("'{}' object does not support item assignment",
                                    bare_type_name(ot)));
                return;
            }
            if (ot->kind == TyKind::Optional && ot->celem != CKind::None && !ot->sub.empty() &&
                ot->sub[0]->kind == TyKind::Int) {
                if (!is_unknown(st) && st->kind != TyKind::Int && st->kind != TyKind::Bool)
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
                if (!merge_elem(ot->sub[0], ot, st, ot->locked))
                    err(line, col,
                        std::format("dict key type is '{}', got '{}'", type_name(ot->sub[0]),
                                    type_name(st)));
                if (!merge_elem(ot->sub[1], ot, value, ot->locked))
                    err(line, col,
                        std::format("cannot store '{}' into '{}'", type_name(value),
                                    type_name(ot)));
            } else if (!is_unknown(ot) && ot->kind != TyKind::Optional &&
                       ot->kind != TyKind::Class && ot->kind != TyKind::None) {
                // 措辞跟 CPython 一致：'range' object does not support item assignment
                err(line, col, std::format("'{}' object does not support item assignment",
                                           bare_type_name(ot)));
            }
            return;
        }
        if (auto* at = dynamic_cast<Attribute*>(&target)) {
            if (auto* vn = dynamic_cast<Name*>(at->value.get())) {
                if (auto mod = lookup_module_alias(vn->id)) {
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
                        if (!assignable(ca->ty, value))
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
                if (auto* f = ot->cls->field(at->attr)) {
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

void TypeChecker::unpack_assign(std::vector<ExprPtr>& targets, TypePtr value, int line, int col) {
        size_t n = targets.size();
        size_t star_at = n;  // n 表示没有星号目标
        for (size_t i = 0; i < n; ++i)
            if (targets[i]->kind == ExprKind::Starred) { star_at = i; break; }

        auto assign_one = [&](Expr& t, TypePtr ty) {
            if (t.kind == ExprKind::Starred) {
                assign(*static_cast<Starred&>(t).value, ty, line, col);
            } else {
                assign(t, ty, line, col);
            }
        };

        if (value->kind == TyKind::Tuple) {
            if (value->variadic) {
                // 长度不定：每个目标拿元素类型，星号目标拿 list[元素]
                TypePtr e = value->sub.empty() ? t_unknown() : value->sub[0];
                for (auto& t : targets) {
                    if (t->kind == ExprKind::Starred)
                        assign_one(*t, mk_list(e));
                    else
                        assign_one(*t, e);
                }
                return;
            }
            auto& elems = value->sub;
            if (star_at == n) {
                if (!elems.empty() && elems.size() != n) {
                    err(line, col, std::format("cannot unpack {} values into {}", elems.size(), n));
                    return;
                }
                for (size_t i = 0; i < n; ++i)
                    assign_one(*targets[i], i < elems.size() ? elems[i] : t_unknown());
                return;
            }
            // a, *b, c = (...)：星号吃中间，剩余目标与末尾元素对齐
            size_t tail = n - star_at - 1;
            if (elems.size() + 1 < n) {
                err(line, col, std::format("cannot unpack {} values into {}", elems.size(), n));
                return;
            }
            for (size_t i = 0; i < star_at; ++i)
                assign_one(*targets[i], i < elems.size() ? elems[i] : t_unknown());
            TypePtr rest = nullptr;
            for (size_t i = star_at; i + tail < elems.size(); ++i)
                rest = rest ? join(rest, elems[i]) : elems[i];
            assign_one(*targets[star_at], mk_list(rest ? rest : t_unknown()));
            for (size_t j = 0; j < tail; ++j)
                assign_one(*targets[star_at + 1 + j], elems[elems.size() - tail + j]);
            return;
        }

        if (value->kind == TyKind::List || value->kind == TyKind::Set ||
            value->kind == TyKind::Generator || value->kind == TyKind::Range ||
            value->kind == TyKind::Str) {
            // 只记录元素类型、不记录长度，逐个赋元素类型。
            // generator/range 是惰性可迭代；str 解包到单字符。
            TypePtr elem = value->kind == TyKind::Str ? t_str()
                          : value->sub.empty()        ? t_unknown()
                                                      : value->sub[0];
            for (auto& t : targets) {
                if (t->kind == ExprKind::Starred)
                    assign_one(*t, mk_list(elem));
                else
                    assign_one(*t, elem);
            }
            return;
        }

        if (is_unknown(value) || value->kind == TyKind::None) {
            for (auto& t : targets) assign_one(*t, t_unknown());
            return;
        }
        if (value->kind == TyKind::Class) return;  // 可能实现了 __iter__
        if (value->kind == TyKind::Optional && !value->sub.empty()) {
            // a, b = maybe_tuple：拆 Optional 内层，None 走 unknown。
            TypePtr inner = value->sub[0];
            if (inner->kind == TyKind::Tuple || inner->kind == TyKind::List ||
                inner->kind == TyKind::Str || inner->kind == TyKind::Generator) {
                for (auto& t : targets) assign_one(*t, t_unknown());
                return;
            }
        }
        err(line, col, std::format("cannot unpack non-sequence '{}'", type_name(value)));
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
