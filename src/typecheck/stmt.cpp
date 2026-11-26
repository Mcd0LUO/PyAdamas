// typecheck/stmt.cpp —— 语句类型检查：控制流、try/with、赋值与解包
#include "driver/build.h"
#include "typecheck/typecheck.h"

#include <filesystem>
#include <set>

#include "ir/module.h"

namespace pyadamas {

// time.struct_time 只读（CPython 里不可变）：赋值/下标写都在编译期拒绝。
static const char* kStructTimeImmutable =
    "'time.struct_time' object is immutable (assign to a variable first)";

static void pattern_capture_names(const Pattern& p, std::vector<std::string>& out);

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

void TypeChecker::stmts(std::vector<StmtPtr>& body) { stmts_impl(body, false); }

// 同一函数作用域里的嵌套块体：继承外层的 del 可见性（BUG-2）。
void TypeChecker::stmts_block(std::vector<StmtPtr>& body) { stmts_impl(body, true); }

void TypeChecker::stmts_impl(std::vector<StmtPtr>& body, bool inherit_del) {
    // 0.537：del 只在本 statements 序列内可见；嵌套块继承（见 DelGuard）。
    DelGuard del_guard(deleted_names_, inherit_del);
    // 提前退出的 None 守卫：`if x is None: return` 之后 x 一定不是 None；收窄保持到块末。
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
                    stmts_block(c.body);
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
            if (ot && ot->kind == TyKind::StructTime) { err(line, col, kStructTimeImmutable); return; }
            if (sub->slice->kind == ExprKind::Slice) {
                // 切片赋值：元素类型必须检查（否则 `l: list[int]; l[0:1] = ['hello']`
                // 会把字符串指针写进 int 列表，读出来是垃圾）；与 merge_elem 同口径。
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
                           ot->kind != TyKind::Optional && ot->kind != TyKind::Buffer)
                    err(line, col,
                        std::format("'{}' object does not support item assignment",
                                    bare_type_name(ot)));
                return;
            }
            if (is_cptr(ot)) {
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
            } else if (!is_unknown(ot) && ot->kind != TyKind::Optional &&
                       ot->kind != TyKind::Buffer) {  // 0.536 连续内存可写；措辞同 CPython
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
                        err_member(vn->id, at->attr, line, col);
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
            if (ot->kind == TyKind::StructTime) { err(line, col, kStructTimeImmutable); return; }
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
            // A8-3：接收者是不是「类对象」（mod.C / type 值）。类属性跨模块
            // 只读，写要给出明确诊断，而不是落到运行期 setattr。
            bool recv_class_obj = false;
            if (ot->kind == TyKind::ClassObj && !ot->sub.empty()) {
                ot = ot->sub[0];
                recv_class_obj = true;
            }
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
                // 跨模块类属性（A8-3）：值与「已拥有」标志都在定义模块里，
                // 导入方写它会破坏 COW 语义，明确报错（以前落到运行期
                // setattr() on an object without dynamic attributes）。
                if (recv_class_obj && !is_class_receiver(*at)) {
                    if (const ClassAttrInfo* ca = ot->cls->class_attr(at->attr)) {
                        if (!ca->writable) {
                            err(line, col,
                                std::format("class attribute '{}.{}' of an imported class is "
                                            "read-only across modules; assign it in the "
                                            "module that defines '{}'",
                                            ot->cls->name, at->attr, ot->cls->name));
                            return;
                        }
                    }
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
