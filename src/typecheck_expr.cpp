// typecheck_expr.cpp —— 表达式类型检查：运算、容器、推导式、迭代
#include "typecheck.h"

#include "module.h"

namespace pyadamas {

TypePtr TypeChecker::expr(Expr& e) {
        TypePtr t = expr_impl(e);
        expr_types_[&e] = t;
        return t;
    }

    TypePtr TypeChecker::expr_impl(Expr& e) {
        ++n_expr;
        // 左深链（1+1+1+...）会让递归深度等于表达式长度，需要一个上限。
        // 800 是权衡出来的：ASan 构建下每层帧约 4KB，2000 层会直接打穿
        // 8MB 栈；而真实的代码里 800 层嵌套已经不存在了。
        if (expr_depth_ >= 800) {
            err(e.line, e.col, "expression nested too deeply");
            return t_unknown();
        }
        ++expr_depth_;
        struct DepthDec {
            int& d;
            ~DepthDec() { --d; }
        } depth_dec{expr_depth_};
        switch (e.kind) {
            case ExprKind::Constant: {
                auto& c = static_cast<Constant&>(e);
                switch (c.ck) {
                    case Constant::CK::None: return t_none();
                    case Constant::CK::Bool: return t_bool();
                    case Constant::CK::Int: return t_int();
                    case Constant::CK::Float: return t_float();
                    case Constant::CK::Complex: return t_complex();
                    case Constant::CK::Str: return t_str();
                    case Constant::CK::Bytes: return t_bytes();
                    case Constant::CK::Ellipsis: return t_unknown();
                }
                return t_unknown();
            }
            case ExprKind::Name: {
                auto& n = static_cast<Name&>(e);
                // 类体命名空间优先于模块全局：Python 的类体里，前面几行
                // 赋过的名字就是类命名空间的成员，会遮住同名全局。
                if (auto it = class_body_attrs_.find(n.id); it != class_body_attrs_.end())
                    return it->second;
                if (auto* sym = lookup(n.id)) {
                    // 类名在值位置是运行期类对象，不是实例。异常类仍按
                    // 类型名使用（raise/except/throw 的既有路径）。
                    if (sym->ty && sym->ty->kind == TyKind::Class && name_is_class(n) &&
                        (!sym->ty->cls || !sym->ty->cls->is_exception_class)) {
                        if (sym->ty->cls) sym->ty->cls->value_used = true;
                        return mk_class_obj(sym->ty);
                    }
                    return sym->ty;
                }
                if (lookup_module_alias(n.id)) {
                    err(n.line, n.col,
                        std::format("module '{}' cannot be used as a value", n.id));
                    return t_unknown();
                }
                if (cmodules_.count(n.id)) {
                    err(n.line, n.col,
                        std::format("cimport '{}' cannot be used as a value", n.id));
                    return t_unknown();
                }
                // 先看内置值（__name__ / __file__ / Ellipsis ...），再看可调用对象
                if (auto vt = builtin_value_type(n.id)) return vt;
                if (is_builtin_callable(n.id)) return builtin_type(n.id);
                if (is_exception_name(n.id)) return t_unknown();
                if (n.id.starts_with("__") && n.id.ends_with("__")) return t_unknown();
                // 有 from x import * 时无从知道导入了哪些名字，只能放行
                if (star_import_) return t_unknown();
                // 模块里出现过 globals()[k] = v：名字集合是动态的
                if (dynamic_globals_) return t_unknown();
                // 模块级通常是条件导入、C 扩展常量、__all__ 之外的名字，
                // 静态无从判断；函数体内才值得报
                err(n.line, n.col, std::format("undefined name '{}'", n.id));
                return t_unknown();
            }
            case ExprKind::JoinedStr: {
                // f-string 里嵌的表达式必须真的检查一遍：以前直接 return
                // t_str()，导致 `f"{undefined_name}"` 不报错，降级阶段也拿不到
                // 内嵌表达式的类型（str 被当成 int 格式化）。
                auto& j = static_cast<JoinedStr&>(e);
                for (auto& v : j.values) expr(*v);
                return t_str();
            }
            case ExprKind::FormattedValue: {
                auto& f = static_cast<FormattedValue&>(e);
                expr(*f.value);
                if (f.format_spec) expr(*f.format_spec);
                return t_str();
            }
            case ExprKind::List: {
                auto& l = static_cast<ListExpr&>(e);
                if (l.elts.empty()) return mk_list(fresh_hole());
                TypePtr acc = expr(*l.elts[0]);
                TypePtr hole = fresh_hole();
                bool all_same = true;
                for (size_t i = 1; i < l.elts.size(); ++i) {
                    auto t = expr(*l.elts[i]);
                    if (!type_eq(acc, t)) all_same = false;
                    acc = join(acc, t);
                }
                (void)all_same;
                if (is_unknown(acc)) return mk_list(hole);
                return mk_list(acc);
            }
            case ExprKind::Tuple: {
                auto& t = static_cast<TupleExpr&>(e);
                std::vector<TypePtr> elems;
                for (auto& el : t.elts) elems.push_back(expr(*el));
                return mk_tuple(std::move(elems));
            }
            case ExprKind::Set: {
                auto& s = static_cast<SetExpr&>(e);
                if (s.elts.empty()) return mk_set(fresh_hole());
                TypePtr acc = expr(*s.elts[0]);
                for (size_t i = 1; i < s.elts.size(); ++i) acc = join(acc, expr(*s.elts[i]));
                return mk_set(acc);
            }
            case ExprKind::Dict: {
                auto& d = static_cast<DictExpr&>(e);
                if (d.keys.empty()) return mk_dict(fresh_hole(), fresh_hole());
                TypePtr kt = nullptr, vt = nullptr;
                for (size_t i = 0; i < d.keys.size(); ++i) {
                    if (d.keys[i]) {
                        auto k = expr(*d.keys[i]);
                        kt = kt ? join(kt, k) : k;
                    }
                    auto v = expr(*d.values[i]);
                    vt = vt ? join(vt, v) : v;
                }
                return mk_dict(kt ? kt : t_unknown(), vt ? vt : t_unknown());
            }
            case ExprKind::BinOp: {
                auto& b = static_cast<BinOp&>(e);
                // `int | str` 这类运行时构造的联合类型：两个操作数都是类型对象，
                // 不是普通值，按值运算检查只会误报
                if (b.op == Tok::VBAR) {
                    auto type_operand = [](Expr* x) {
                        if (auto* n = dynamic_cast<Name*>(x)) return TypeChecker::is_type_name(n->id);
                        if (auto* c = dynamic_cast<Constant*>(x)) return c->ck == Constant::CK::None;
                        return false;
                    };
                    if (type_operand(b.left.get()) && type_operand(b.right.get())) return t_unknown();
                }
                auto l = expr(*b.left);
                auto r = expr(*b.right);
                return binop_type(b.op, l, r, b.line, b.col);
            }
            case ExprKind::UnaryOp: {
                auto& u = static_cast<UnaryOp&>(e);
                auto t = expr(*u.operand);
                switch (u.op) {
                    case Tok::KW_NOT: return t_bool();
                    case Tok::MINUS:
                    case Tok::PLUS:
                        // 类实例 / 绑定方法可能重载了一元运算符（@property
                        // 返回的数值最常见），静态上看不见
                        if (is_unknown(t) || is_num(t) || t->kind == TyKind::Complex ||
                            t->kind == TyKind::Class || t->kind == TyKind::BoundMethod ||
                            t->kind == TyKind::TimeDelta)
                            return t;
                        err(u.line, u.col,
                            std::format("bad operand type for unary operator: '{}'", type_name(t)));
                        return t_unknown();
                    case Tok::TILDE:
                        if (is_unknown(t) || t->kind == TyKind::Int) return t_int();
                        err(u.line, u.col, "bad operand type for '~': expected int");
                        return t_unknown();
                    default: return t_unknown();
                }
            }
            case ExprKind::BoolOp: {
                auto& b = static_cast<BoolOp&>(e);
                TypePtr acc = nullptr;
                for (auto& v : b.values) {
                    auto t = expr(*v);
                    acc = acc ? join(acc, t) : t;
                }
                return acc ? acc : t_unknown();
            }
            case ExprKind::Compare: {
                auto& c = static_cast<Compare&>(e);
                auto l = expr(*c.left);
                for (size_t i = 0; i < c.ops.size(); ++i) {
                    auto r = expr(*c.comparators[i]);
                    compare_type(c.ops[i], l, r, c.line, c.col);
                    l = r;
                }
                return t_bool();
            }
            case ExprKind::Call:
                return call(static_cast<Call&>(e));
            case ExprKind::Subscript: {
                auto& s = static_cast<Subscript&>(e);
                // list[int] / dict[str, int]：PEP 585 的泛型别名，是类型对象不是值
                if (auto* vn = dynamic_cast<Name*>(s.value.get());
                    vn && is_type_name(vn->id)) {
                    if (!s.slice) return t_unknown();
                    if (auto* tup = dynamic_cast<TupleExpr*>(s.slice.get()))
                        for (auto& el : tup->elts) expr(*el);
                    else
                        expr(*s.slice);
                    return t_unknown();
                }
                auto ot = expr(*s.value);
                if (s.slice->kind == ExprKind::Slice) {
                    auto& sl = static_cast<SliceExpr&>(*s.slice);
                    if (sl.lower) expr(*sl.lower);
                    if (sl.upper) expr(*sl.upper);
                    if (sl.step) expr(*sl.step);
                    if (ot->kind == TyKind::List || ot->kind == TyKind::Str ||
                        ot->kind == TyKind::Bytes)
                        return ot;
                    if (ot->kind == TyKind::Tuple) {
                        // 切片长度不可知，元素类型仍是原来的元素类型：
                        // tt[-2:] 不能还是 tuple[11]，否则解包会误报
                        if (ot->variadic) return ot;
                        if (ot->sub.empty()) return mk_tuple_rest(t_unknown());
                        TypePtr acc = ot->sub[0];
                        for (size_t i = 1; i < ot->sub.size(); ++i)
                            acc = join(acc, ot->sub[i]);
                        return mk_tuple_rest(acc);
                    }
                    if (is_unknown(ot)) return t_unknown();
                    err(s.line, s.col,
                        std::format("'{}' is not sliceable", type_name(ot)));
                    return t_unknown();
                }
                auto it = expr(*s.slice);
                // 定长 tuple 的常量下标可以精确到具体元素；不做这一步，
                // `d.popitem()[0]` 这种常见写法会退化成 unknown。
                if (ot && ot->kind == TyKind::Tuple && !ot->variadic &&
                    s.slice->kind == ExprKind::Constant) {
                    auto& c = static_cast<Constant&>(*s.slice);
                    if (c.ck == Constant::CK::Int || c.ck == Constant::CK::Bool) {
                        int64_t i = c.ck == Constant::CK::Bool ? (c.b ? 1 : 0) : c.i;
                        if (i < 0) i += static_cast<int64_t>(ot->sub.size());
                        if (i >= 0 && i < static_cast<int64_t>(ot->sub.size()))
                            return ot->sub[static_cast<size_t>(i)];
                    }
                }
                // ptr[T] 解引用：Optional[int] 上带元素种类时支持 p[i]。
                if (ot && ot->kind == TyKind::Optional && ot->celem != CKind::None &&
                    !ot->sub.empty() && ot->sub[0]->kind == TyKind::Int) {
                    if (!is_unknown(it) && it->kind != TyKind::Int && it->kind != TyKind::Bool)
                        err(s.line, s.col, "pointer index must be an integer");
                    return cptr_elem_type(ot->celem);
                }
                return subscript_type(ot, it, s.line, s.col);
            }
            case ExprKind::Attribute: {
                auto& a = static_cast<Attribute&>(e);
                if (auto* vn = dynamic_cast<Name*>(a.value.get())) {
                    if (const CImportDecl* cd = cmodule(vn->id)) {
                        for (auto& c : cd->consts)
                            if (c.name == a.attr) return ckind_type(c.kind);
                        for (auto& v : cd->vars) {
                            if (v.py_name != a.attr) continue;
                            if (v.kind == CKind::Ptr && !v.struct_name.empty()) {
                                if (StructInfo* st = struct_info(v.struct_name))
                                    return mk_optional(mk_struct_ptr(st));
                            }
                            return ckind_type(v.kind);
                        }
                        for (auto& x : cd->externs) {
                            if (x.py_name != a.attr) continue;
                            auto it = extern_fns_.find(&x);
                            if (it == extern_fns_.end()) break;
                            auto ft = new_ty(TyKind::Func);
                            ft->fn = it->second;
                            return ft;
                        }
                        err(a.line, a.col,
                            std::format("cimport '{}' has no member '{}'", vn->id, a.attr));
                        return t_unknown();
                    }
                }
                if (auto* vn = dynamic_cast<Name*>(a.value.get())) {
                    if (auto mod = lookup_module_alias(vn->id)) {
                        if (auto ci = mod->classes.find(a.attr); ci != mod->classes.end())
                            return mk_class_obj(mk_class(ci->second.cls.get()));
                        auto it = mod->globals.find(a.attr);
                        if (it == mod->globals.end()) {
                            err(a.line, a.col,
                                std::format("module '{}' has no attribute '{}'", vn->id, a.attr));
                            return t_unknown();
                        }
                        return it->second;
                    }
                }
                // str.capitalize / int.bit_length 这类「类型上的未绑定方法」
                if (auto* vn = dynamic_cast<Name*>(a.value.get()); vn && is_type_name(vn->id))
                    return t_unknown();
                auto ot = expr(*a.value);
                if (is_unknown(ot)) return t_unknown();
                if (ot->kind == TyKind::Optional) {
                    // 收窄做不到逐条路径成立，但内层是类/结构体指针时字段
                    // 类型是确定的；运行时有空指针检查兜底。
                    if (ot->sub.empty()) return t_unknown();
                    TyKind ik = ot->sub[0]->kind;
                    if (ik != TyKind::Class && ik != TyKind::Struct && ik != TyKind::StructPtr)
                        return t_unknown();
                    ot = ot->sub[0];
                }
                if (ot->kind == TyKind::ClassObj) {
                    if (a.attr == "__name__") return t_str();
                    if (!ot->sub.empty()) ot = ot->sub[0];
                }
                if (ot->kind == TyKind::Struct || ot->kind == TyKind::StructPtr) {
                    const StructInfo* s = ot->st;
                    if (!s) return t_unknown();
                    if (const auto* f = s->field(a.attr)) return f->ty;
                    err(a.line, a.col,
                        std::format("struct '{}' has no field '{}'", s->name, a.attr));
                    return t_unknown();
                }
                if (ot->kind == TyKind::Exception) {
                    if (a.attr == "args") return mk_tuple({t_str()});
                    if (a.attr == "__cause__")
                        return mk_optional(mk_exception("Exception"));
                    // 内置异常的其它属性（OSError.errno、UnicodeError.code……）
                    // 静态看不见，check 保守放过，build 再决定。
                    unsupported.push_back("attribute on a builtin exception");
                    return t_unknown();
                }
                if (ot->kind == TyKind::TypeObj) {
                    if (a.attr == "__name__") return t_str();
                    err(a.line, a.col,
                        std::format("'type' object has no attribute '{}'", a.attr));
                    return t_unknown();
                }
                if (ot->kind == TyKind::File) {
                    if (a.attr == "closed") return t_bool();
                    if (a.attr == "name" || a.attr == "mode" || a.attr == "encoding" ||
                        a.attr == "errors")
                        return t_str();
                    err(a.line, a.col,
                        std::format("'file' object has no attribute '{}'", a.attr));
                    return t_unknown();
                }
                if (ot->kind == TyKind::DateTime) {
                    if (a.attr == "year" || a.attr == "month" || a.attr == "day" ||
                        a.attr == "hour" || a.attr == "minute" || a.attr == "second")
                        return t_int();
                    err(a.line, a.col,
                        std::format("'datetime' object has no attribute '{}'", a.attr));
                    return t_unknown();
                }
                if (ot->kind == TyKind::TimeDelta) {
                    if (a.attr == "days" || a.attr == "seconds" ||
                        a.attr == "microseconds")
                        return t_int();
                    err(a.line, a.col,
                        std::format("'timedelta' object has no attribute '{}'", a.attr));
                    return t_unknown();
                }
                if (ot->kind == TyKind::Path) {
                    if (a.attr == "name" || a.attr == "stem" || a.attr == "suffix")
                        return t_str();
                    if (a.attr == "parent") return new_ty(TyKind::Path);
                    if (a.attr == "parts") return mk_list(t_str());
                    err(a.line, a.col,
                        std::format("'pathlib.Path' object has no attribute '{}'", a.attr));
                    return t_unknown();
                }
                if (ot->kind == TyKind::Deque) {
                    if (a.attr == "maxlen") return t_int();
                    err(a.line, a.col,
                        std::format("'collections.deque' object has no attribute '{}'",
                                    a.attr));
                    return t_unknown();
                }
                if (ot->kind == TyKind::CompletedProcess) {
                    if (a.attr == "returncode") return t_int();
                    if (a.attr == "stdout" || a.attr == "stderr") return t_str();
                    err(a.line, a.col,
                        std::format("'CompletedProcess' object has no attribute '{}'", a.attr));
                    return t_unknown();
                }
                if (ot->kind == TyKind::Popen) {
                    if (a.attr == "pid" || a.attr == "returncode") return t_int();
                    if (a.attr == "stdout" || a.attr == "stderr" || a.attr == "stdin")
                        return t_file();
                    err(a.line, a.col,
                        std::format("'Popen' object has no attribute '{}'", a.attr));
                    return t_unknown();
                }
                if (ot->kind == TyKind::Regex) {
                    if (a.attr == "groups" || a.attr == "flags") return t_int();
                    if (a.attr == "pattern") return t_str();
                    err(a.line, a.col,
                        std::format("re.Pattern has no attribute '{}'", a.attr));
                    return t_unknown();
                }
                if (ot->kind == TyKind::Match) {
                    if (a.attr == "lastindex") return t_int();
                    if (a.attr == "lastgroup") return t_str();
                    err(a.line, a.col, std::format("re.Match has no attribute '{}'", a.attr));
                    return t_unknown();
                }
                // 双下划线属性属于运行时的协议实现，静态上没有方法表
                if (a.attr.size() > 4 && a.attr.starts_with("__") && a.attr.ends_with("__"))
                    return t_unknown();
                // Python 里函数是普通对象，可以挂任意属性（functools.wraps 就这么做）
                if (ot->kind == TyKind::Func) return t_unknown();
                if (ot->kind == TyKind::Class) {
                    if (!ot->cls) return t_unknown();
                    // 骨架：类体还没检查到（类定义在引用点之后），成员表是空的。
                    // 正在检查本类方法时不算骨架——那时字段和方法都已登记完。
                    if (!ot->cls->complete && ot->cls != cur_class_) return t_unknown();
                    if (const PropertyInfo* p = ot->cls->property(a.attr)) {
                        if (!p->owner || p->getter < 0 ||
                            p->getter >= static_cast<int>(p->owner->methods.size()))
                            return t_unknown();
                        const MethodInfo& gm = p->owner->methods[p->getter];
                        return gm.call_sig && gm.call_sig->ret ? gm.call_sig->ret
                                                               : t_unknown();
                    }
                    if (is_class_receiver(a)) {
                        // 类接收者：`C.x` / `cls.x` 只可能是类属性或方法，
                        // 实例字段在类上不存在（同名时也不能被它挡住）。
                        if (const ClassAttrInfo* ca = ot->cls->class_attr(a.attr)) return ca->ty;
                        if (const MethodInfo* m = ot->cls->method(a.attr))
                            return mk_method_value(*m, true);
                        if (ot->cls->is_open()) return t_unknown();
                        err(a.line, a.col,
                            std::format("class '{}' has no attribute '{}'", ot->cls->name,
                                        a.attr));
                        return t_unknown();
                    }
                    if (auto* f = ot->cls->field(a.attr)) return f->ty;
                    // 装饰方法在类属性里也有包装函数，必须先按方法查，
                    // 否则会把含 self 的包装签名当成方法值的类型。
                    if (const MethodInfo* m = ot->cls->method(a.attr))
                        return mk_method_value(*m, false);
                    if (const ClassAttrInfo* ca = ot->cls->class_attr(a.attr)) return ca->ty;
                    // `self.x` 找不到：可能是 mixin 组合出来的成员，也可能
                    // 是类体里赋的类属性——两种都不是错误，静态上认输。
                    if (is_self_attr(a)) return t_unknown();
                    // 有外部基类时成员表本来就不全，查不到不等于不存在
                    if (ot->cls->is_open()) return t_unknown();
                    // 类已因 setattr/hasattr/getattr 或非字面量名字开启了动态属性：
                    // 读取落到运行期 dict。注意不能因为一次普通读取就 self-mark，
                    // 否则拼错的字段名会被当成动态属性静默放过。
                    if (ot->cls->needs_dict) return t_unknown();
                    err(a.line, a.col,
                        std::format("'{}' object has no attribute '{}'", ot->cls->name, a.attr));
                    return t_unknown();
                }
                // 属性（不是方法）不能当可调用对象用：int.bit_length 是方法，
                // int.numerator 是属性，`n.numerator` 取到的是数值本身
                if (is_property_name(a.attr) && has_method(ot, a.attr)) return t_unknown();
                if (has_method(ot, a.attr)) return mk_bound(ot, a.attr);
                err(a.line, a.col,
                    std::format("'{}' object has no attribute '{}'", type_name(ot), a.attr));
                return t_unknown();
            }
            case ExprKind::Slice:
                return t_unknown();
            case ExprKind::ListComp: {
                auto& c = static_cast<ListComp&>(e);
                return mk_list(comp_elem(e, c.generators, c.elt));
            }
            case ExprKind::SetComp: {
                auto& c = static_cast<SetComp&>(e);
                return mk_set(comp_elem(e, c.generators, c.elt));
            }
            case ExprKind::DictComp: {
                auto& c = static_cast<DictComp&>(e);
                enter_comp_scope(c.generators);
                auto k = expr(*c.key);
                auto v = expr(*c.value);
                exit_comp_scope();
                return mk_dict(k, v);
            }
            case ExprKind::GeneratorExp: {
                auto& c = static_cast<GeneratorExp&>(e);
                for (auto& g : c.generators)
                    if (static_cast<Comprehension&>(*g).is_async) {
                        err(e.line, e.col,
                            "async generator expressions are not supported yet");
                        break;
                    }
                return mk_list(comp_elem(e, c.generators, c.elt));
            }
            case ExprKind::Lambda: {
                auto& l = static_cast<Lambda&>(e);
                auto info = std::make_shared<FuncInfo>();
                info->name = "<lambda>";
                for (auto& p : l.args.posonly) {
                    info->pos.push_back(Param{p.name, t_unknown(), false, false, nullptr, std::nullopt});
                    ++info->n_posonly;
                }
                for (auto& p : l.args.args)
                    info->pos.push_back(Param{p.name, t_unknown(), false, false, nullptr, std::nullopt});
                for (auto& p : l.args.kwonly)
                    info->kwonly.push_back(Param{p.name, t_unknown(), false, true, nullptr, std::nullopt});
                if (l.args.vararg) {
                    info->varargs = true;
                    info->vararg_name = l.args.vararg->name;
                    info->vararg_ty = t_unknown();
                }
                if (l.args.kwarg) {
                    info->kwargs = true;
                    info->kwarg_name = l.args.kwarg->name;
                    info->kwarg_ty = t_unknown();
                }

                auto sc = std::make_unique<Scope>();
                sc->parent = cur_;
                cur_ = sc.get();
                pool_.push_back(std::move(sc));
                for (auto& p : info->pos)
                    cur_->vars[p.name] = Symbol{p.ty, l.line, false, false};
                for (auto& p : info->kwonly)
                    cur_->vars[p.name] = Symbol{p.ty, l.line, false, false};
                if (info->varargs)
                    cur_->vars[info->vararg_name] = Symbol{mk_tuple_rest(t_unknown()), l.line,
                                                           false, false};
                if (info->kwargs)
                    cur_->vars[info->kwarg_name] = Symbol{t_unknown(), l.line, false, false};
                auto body = expr(*l.body);
                cur_ = cur_->parent;
                info->ret = body;
                info->ret_known = true;
                auto t = new_ty(TyKind::Func);
                t->fn = info;
                return t;
            }
            case ExprKind::IfExp: {
                auto& f = static_cast<IfExp&>(e);
                expr(*f.test);
                auto a = expr(*f.body);
                auto b = expr(*f.orelse);
                return join(a, b);
            }
            case ExprKind::Starred:
                return expr(*static_cast<Starred&>(e).value);
            case ExprKind::NamedExpr: {
                auto& n = static_cast<NamedExpr&>(e);
                auto vt = expr(*n.value);
                assign(*n.target, vt, n.line, n.col);
                return vt;
            }
            case ExprKind::Await: {
                auto& aw = static_cast<Await&>(e);
                TypePtr vt = expr(*aw.value);
                if (!vt || vt->kind == TyKind::Unknown) return t_unknown();
                if (vt->kind == TyKind::Coroutine || vt->kind == TyKind::Task ||
                    vt->kind == TyKind::Future)
                    return vt->sub.empty() ? t_unknown() : vt->sub[0];
                err(e.line, e.col,
                    std::format("'{}' object is not awaitable", type_name(vt)));
                return t_unknown();
            }
            case ExprKind::Yield: {
                saw_yield_ = true;
                auto& y = static_cast<Yield&>(e);
                TypePtr t = y.value ? expr(*y.value) : t_none();
                if (yields_) yields_->push_back(t);
                // yield 表达式的值是 send() 传进来的：声明了
                // Generator[Y, S, R] 就按 S 收窄，否则静态不可知。
                if (cur_fn_ && cur_fn_->ret && cur_fn_->ret->kind == TyKind::Generator &&
                    cur_fn_->ret->sub.size() > 1 && cur_fn_->ret->sub[1])
                    return cur_fn_->ret->sub[1];
                return t_unknown();
            }
            case ExprKind::YieldFrom: {
                saw_yield_ = true;
                auto& yf = static_cast<YieldFrom&>(e);
                TypePtr vt = expr(*yf.value);
                if (cur_fn_ && cur_fn_->is_async) {
                    err(e.line, e.col, "'yield from' is not allowed in an async function");
                    return t_unknown();
                }
                TypePtr elem = t_unknown();
                if (vt) {
                    switch (vt->kind) {
                        case TyKind::Unknown:
                        case TyKind::Optional:
                            break;
                        case TyKind::Generator:
                            elem = vt->sub.empty() ? t_unknown() : vt->sub[0];
                            break;
                        case TyKind::List:
                        case TyKind::Tuple:
                        case TyKind::Dict:
                        case TyKind::Set:
                        case TyKind::Range:
                            elem = vt->sub.empty() ? t_unknown() : vt->sub[0];
                            break;
                        case TyKind::Str:
                            elem = t_str();
                            break;
                        case TyKind::AsyncGenerator:
                            err(e.line, e.col,
                                "cannot 'yield from' an async generator; use 'async for'");
                            break;
                        default:
                            err(e.line, e.col,
                                std::format("cannot 'yield from' '{}'", type_name(vt)));
                            break;
                    }
                }
                if (yields_) yields_->push_back(elem);
                return t_unknown();
            }
            case ExprKind::Comprehension:
                return t_unknown();
        }
        return t_unknown();
    }

TypePtr TypeChecker::comp_elem(Expr& self, std::vector<ExprPtr>& gens, ExprPtr& elt) {
        (void)self;
        enter_comp_scope(gens);
        auto t = expr(*elt);
        exit_comp_scope();
        return t;
    }

void TypeChecker::enter_comp_scope(std::vector<ExprPtr>& gens) {
        auto sc = std::make_unique<Scope>();
        sc->parent = cur_;
        cur_ = sc.get();
        pool_.push_back(std::move(sc));
        for (auto& g : gens) {
            auto& c = static_cast<Comprehension&>(*g);
            auto it = expr(*c.iter);
            TypePtr elem;
            if (c.is_async) {
                if (!cur_fn_ || !cur_fn_->is_async)
                    err(c.line, c.col, "'async for' outside async function");
                if (!it || it->kind == TyKind::Unknown || it->kind == TyKind::Optional ||
                    it->kind == TyKind::AsyncGenerator)
                    elem = (it && it->kind == TyKind::AsyncGenerator && !it->sub.empty())
                               ? it->sub[0]
                               : t_unknown();
                else {
                    err(c.line, c.col,
                        std::format("async for requires an async generator, got '{}'",
                                    type_name(it)));
                    elem = t_unknown();
                }
            } else {
                elem = iter_elem(it, c.line, c.col);
            }
            assign(*c.target, elem, c.line, c.col);
            for (auto& cond : c.ifs) expr(*cond);
        }
    }

TypePtr TypeChecker::dunder_binop_ret(Tok op, TypePtr l, TypePtr r) {
    struct Ref {
        Tok op;
        const char* fwd;
        const char* rev;
    };
    static const Ref tbl[] = {
        {Tok::PLUS, "__add__", "__radd__"},
        {Tok::MINUS, "__sub__", "__rsub__"},
        {Tok::STAR, "__mul__", "__rmul__"},
        {Tok::SLASH, "__truediv__", "__rtruediv__"},
        {Tok::DOUBLESLASH, "__floordiv__", "__rfloordiv__"},
        {Tok::PERCENT, "__mod__", "__rmod__"},
        {Tok::DOUBLESTAR, "__pow__", "__rpow__"},
    };
    const Ref* ref = nullptr;
    for (const auto& e : tbl)
        if (e.op == op) {
            ref = &e;
            break;
        }
    if (!ref) return nullptr;
    auto try_side = [&](TypePtr self, TypePtr other, const char* name) -> TypePtr {
        if (!self || self->kind != TyKind::Class || !self->cls) return nullptr;
        const MethodInfo* m = self->cls->method(name);
        if (!m || m->kind != MethodKind::Normal || !m->call_sig) return nullptr;
        if (m->call_sig->varargs || m->call_sig->kwargs || m->call_sig->pos.size() != 1)
            return nullptr;
        TypePtr pt = m->call_sig->pos[0].ty;
        if (pt && pt->kind != TyKind::Unknown && other && other->kind != TyKind::Unknown &&
            !assignable(pt, other))
            return nullptr;
        return m->call_sig->ret ? m->call_sig->ret : t_unknown();
    };
    if (TypePtr t = try_side(l, r, ref->fwd)) return t;
    return try_side(r, l, ref->rev);
}

TypePtr TypeChecker::dunder_aug_ret(Tok op, TypePtr l, TypePtr r) {
    struct Ref {
        Tok op;
        const char* iaug;
    };
    static const Ref tbl[] = {
        {Tok::PLUSEQUAL, "__iadd__"},      {Tok::MINUSEQUAL, "__isub__"},
        {Tok::STAREQUAL, "__imul__"},      {Tok::SLASHEQUAL, "__itruediv__"},
        {Tok::DOUBLESLASHEQUAL, "__ifloordiv__"}, {Tok::PERCENTEQUAL, "__imod__"},
        {Tok::DOUBLESTAREQUAL, "__ipow__"},
    };
    const char* name = nullptr;
    for (const auto& e : tbl)
        if (e.op == op) {
            name = e.iaug;
            break;
        }
    if (!name) return nullptr;
    if (!l || l->kind != TyKind::Class || !l->cls) return nullptr;
    const MethodInfo* m = l->cls->method(name);
    if (!m || m->kind != MethodKind::Normal || !m->call_sig) return nullptr;
    if (m->call_sig->varargs || m->call_sig->kwargs || m->call_sig->pos.size() != 1)
        return nullptr;
    TypePtr pt = m->call_sig->pos[0].ty;
    if (pt && pt->kind != TyKind::Unknown && r && r->kind != TyKind::Unknown &&
        !assignable(pt, r))
        return nullptr;
    return m->call_sig->ret ? m->call_sig->ret : t_unknown();
}

bool TypeChecker::dunder_cmp_ok(CmpOp op, TypePtr l, TypePtr r) {
    struct Ref {
        CmpOp op;
        const char* fwd;
        const char* rev;
    };
    static const Ref tbl[] = {
        {CmpOp::Lt, "__lt__", "__gt__"},
        {CmpOp::Le, "__le__", "__ge__"},
        {CmpOp::Gt, "__gt__", "__lt__"},
        {CmpOp::Ge, "__ge__", "__le__"},
    };
    const char* fwd = nullptr;
    const char* rev = nullptr;
    for (const auto& e : tbl)
        if (e.op == op) {
            fwd = e.fwd;
            rev = e.rev;
            break;
        }
    if (!fwd) return false;
    auto side_ok = [&](TypePtr self, TypePtr other, const char* name) {
        if (!self || self->kind != TyKind::Class || !self->cls) return false;
        const MethodInfo* m = self->cls->method(name);
        if (!m || m->kind != MethodKind::Normal || !m->call_sig) return false;
        if (m->call_sig->varargs || m->call_sig->kwargs || m->call_sig->pos.size() != 1)
            return false;
        TypePtr pt = m->call_sig->pos[0].ty;
        if (pt && pt->kind != TyKind::Unknown && other && other->kind != TyKind::Unknown &&
            !assignable(pt, other))
            return false;
        return true;
    };
    return side_ok(l, r, fwd) || side_ok(r, l, rev);
}

TypePtr TypeChecker::binop_type(Tok op, TypePtr l, TypePtr r, int line, int col) {
        if (is_unknown(l) || is_unknown(r)) {
            // 一侧 unknown（没标注的参数最常见）、另一侧是 float 时，结果
            // 仍然是 float：Python 先把另一侧提升成 double。以前这里一律
            // 返回 unknown，codegen 兜底按整数算，算出来是位模式参与整数
            // 乘除的野值，而且不报错。unknown 一侧按机器整数表示补 convert；
            // 浮点实参本来就不支持传给未标注参数，这里不改变那个限制。
            bool lf = l && l->kind == TyKind::Float;
            bool rf = r && r->kind == TyKind::Float;
            switch (op) {
                // / 永远产出 float，哪怕另一侧是 int
                case Tok::SLASH: return t_float();
                case Tok::PLUS:
                case Tok::MINUS:
                case Tok::STAR:
                case Tok::DOUBLESLASH:
                case Tok::PERCENT:
                case Tok::DOUBLESTAR:
                    if (lf != rf) return t_float();
                    break;
                default: break;
            }
            return t_unknown();
        }
        // 类实例的算术 dunder：有实现就按方法声明的返回类型走。
        if (l->kind == TyKind::Class || r->kind == TyKind::Class) {
            if (TypePtr t = dunder_binop_ret(op, l, r)) return t;
        }
        // BoundMethod 上做算术没有静态规则，放过；类实例一侧没有匹配的
        // dunder 时后端只能按整数算指针，宁可在这里报错。
        if (l->kind == TyKind::BoundMethod || r->kind == TyKind::BoundMethod)
            return t_unknown();
        // Optional / None 需要先判空才能运算，这里不做数据流分析，直接放行
        if (l->kind == TyKind::Optional || r->kind == TyKind::Optional ||
            l->kind == TyKind::None || r->kind == TyKind::None)
            return t_unknown();
        // Path / str：路径拼接（CPython 的 Path.__truediv__）
        if (l->kind == TyKind::Path && op == Tok::SLASH) {
            if (r->kind != TyKind::Str && !is_unknown(r)) {
                err(line, col,
                    std::format("unsupported operand type(s) for /: 'pathlib.Path' and '{}'",
                                type_name(r)));
                return t_unknown();
            }
            return new_ty(TyKind::Path);
        }
        auto bad = [&]() {
            err(line, col,
                std::format("unsupported operand type(s) for {}: '{}' and '{}'", tok_name(op),
                            type_name(l), type_name(r)));
            return t_unknown();
        };
        // 元组重复的结果长度运行期才知道，静态类型记成变长；异构元素按 unknown。
        auto tuple_rest_of = [](const TypePtr& t) {
            TypePtr et = t->sub.empty() ? t_unknown() : t->sub[0];
            for (size_t i = 1; i < t->sub.size(); ++i) {
                if (!type_eq(et, t->sub[i])) {
                    et = t_unknown();
                    break;
                }
            }
            return mk_tuple_rest(et);
        };

        // datetime / timedelta 的算术：只有下面这几种组合有定义。
        if (l->kind == TyKind::DateTime || r->kind == TyKind::DateTime ||
            l->kind == TyKind::TimeDelta || r->kind == TyKind::TimeDelta) {
            auto td = [](const TypePtr& t) { return t->kind == TyKind::TimeDelta; };
            auto dt = [](const TypePtr& t) { return t->kind == TyKind::DateTime; };
            switch (op) {
                case Tok::MINUS:
                    if (dt(l) && dt(r)) return new_ty(TyKind::TimeDelta);
                    if (dt(l) && td(r)) return new_ty(TyKind::DateTime);
                    if (td(l) && td(r)) return new_ty(TyKind::TimeDelta);
                    break;
                case Tok::PLUS:
                    if (dt(l) && td(r)) return new_ty(TyKind::DateTime);
                    if (td(l) && dt(r)) return new_ty(TyKind::DateTime);
                    if (td(l) && td(r)) return new_ty(TyKind::TimeDelta);
                    break;
                case Tok::STAR:
                    if (td(l) && r->kind == TyKind::Int) return new_ty(TyKind::TimeDelta);
                    if (l->kind == TyKind::Int && td(r)) return new_ty(TyKind::TimeDelta);
                    break;
                case Tok::SLASH:
                    if (td(l) && r->kind == TyKind::Int) return new_ty(TyKind::TimeDelta);
                    if (td(l) && td(r)) return t_float();
                    break;
                case Tok::DOUBLESLASH:
                    if (td(l) && r->kind == TyKind::Int) return new_ty(TyKind::TimeDelta);
                    if (td(l) && td(r)) return t_int();
                    break;
                default: break;
            }
            return bad();
        }

        if (l->kind == TyKind::Complex || r->kind == TyKind::Complex) {
            auto ok = [](const TypePtr& t) {
                return t && (t->kind == TyKind::Complex || is_num(t));
            };
            switch (op) {
                case Tok::PLUS:
                case Tok::MINUS:
                case Tok::STAR:
                case Tok::SLASH:
                    if (ok(l) && ok(r)) return t_complex();
                    return bad();
                default:
                    return bad();
            }
        }

        switch (op) {
            case Tok::PLUS:
                if (is_num(l) && is_num(r)) return promote(l, r);
                if (l->kind == TyKind::Str && r->kind == TyKind::Str) return t_str();
                if (l->kind == TyKind::Bytes && r->kind == TyKind::Bytes) return t_bytes();
                // 异常值参与字符串拼接时按消息处理，兼容旧代码 "msg:" + e
                if (l->kind == TyKind::Str && r->kind == TyKind::Exception) return t_str();
                if (l->kind == TyKind::Exception && r->kind == TyKind::Str) return t_str();
                if (l->kind == TyKind::List && r->kind == TyKind::List)
                    return mk_list(join(l->sub[0], r->sub[0]));
                if (l->kind == TyKind::Tuple && r->kind == TyKind::Tuple) {
                    std::vector<TypePtr> v = l->sub;
                    v.insert(v.end(), r->sub.begin(), r->sub.end());
                    return mk_tuple(std::move(v));
                }
                return bad();
            case Tok::MINUS:
                if (is_num(l) && is_num(r)) return promote(l, r);
                if (l->kind == TyKind::Set && r->kind == TyKind::Set) return l;
                return bad();
            case Tok::STAR:
                if (is_num(l) && is_num(r)) return promote(l, r);
                if (l->kind == TyKind::Str && r->kind == TyKind::Int) return t_str();
                if (l->kind == TyKind::Int && r->kind == TyKind::Str) return t_str();
                if (l->kind == TyKind::Bytes && r->kind == TyKind::Int) return t_bytes();
                if (l->kind == TyKind::Int && r->kind == TyKind::Bytes) return t_bytes();
                if (l->kind == TyKind::List && r->kind == TyKind::Int) return l;
                if (l->kind == TyKind::Int && r->kind == TyKind::List) return r;
                if (l->kind == TyKind::Tuple && r->kind == TyKind::Int)
                    return tuple_rest_of(l);
                if (l->kind == TyKind::Int && r->kind == TyKind::Tuple)
                    return tuple_rest_of(r);
                return bad();
            case Tok::SLASH:
                if (is_num(l) && is_num(r)) return t_float();
                return bad();
            case Tok::DOUBLESLASH:
            case Tok::PERCENT:
                if (is_num(l) && is_num(r)) return promote(l, r);
                if (op == Tok::PERCENT && l->kind == TyKind::Str) return t_str();
                if (op == Tok::PERCENT && l->kind == TyKind::Bytes) return t_bytes();
                return bad();
            case Tok::DOUBLESTAR:
                if (is_num(l) && is_num(r)) return promote(l, r);
                return bad();
            case Tok::AMPER:
            case Tok::VBAR:
            case Tok::CIRCUMFLEX:
                if (l->kind == TyKind::Int && r->kind == TyKind::Int) return t_int();
                if (l->kind == TyKind::Bool && r->kind == TyKind::Bool) return t_bool();
                if (l->kind == TyKind::Set && r->kind == TyKind::Set) return l;
                if (l->kind == TyKind::Dict && r->kind == TyKind::Dict && op == Tok::VBAR)
                    return l;
                return bad();
            case Tok::LSHIFT:
            case Tok::RSHIFT:
                if (l->kind == TyKind::Int && r->kind == TyKind::Int) return t_int();
                return bad();
            default:
                return bad();
        }
    }

void TypeChecker::compare_type(CmpOp op, TypePtr l, TypePtr r, int line, int col) {
        if (is_unknown(l) || is_unknown(r)) return;
        if (l->kind == TyKind::Optional || r->kind == TyKind::Optional) return;
        switch (op) {
            case CmpOp::Eq:
            case CmpOp::Ne:
            case CmpOp::Is:
            case CmpOp::IsNot:
                return;
            case CmpOp::Lt:
            case CmpOp::Le:
            case CmpOp::Gt:
            case CmpOp::Ge:
                if (is_num(l) && is_num(r)) return;
                if (l->kind == TyKind::Str && r->kind == TyKind::Str) return;
                if (l->kind == TyKind::Bytes && r->kind == TyKind::Bytes) return;
                // 元组按字典序比较（版本号、日期这类 (a, b) 比较很常见）
                if (l->kind == TyKind::Tuple && r->kind == TyKind::Tuple) return;
                if (l->kind == TyKind::List && r->kind == TyKind::List) return;
                if (l->kind == TyKind::Set && r->kind == TyKind::Set) return;
                if ((l->kind == TyKind::DateTime && r->kind == TyKind::DateTime) ||
                    (l->kind == TyKind::TimeDelta && r->kind == TyKind::TimeDelta))
                    return;
                // 类实例只有实现了 __lt__/__le__/__gt__/__ge__（含镜像）
                // 才可排序，否则后端会拿指针当整数比——必须在这里拦住。
                if (l->kind == TyKind::Class || r->kind == TyKind::Class) {
                    if (dunder_cmp_ok(op, l, r)) return;
                    err(line, col,
                        std::format("'{}' and '{}' are not orderable (no matching "
                                    "__lt__/__le__/__gt__/__ge__)",
                                    type_name(l), type_name(r)));
                    return;
                }
                err(line, col,
                    std::format("'{}' and '{}' are not orderable", type_name(l), type_name(r)));
                return;
            case CmpOp::In:
            case CmpOp::NotIn:
                if (r->kind == TyKind::List || r->kind == TyKind::Set || r->kind == TyKind::Str ||
                    r->kind == TyKind::Bytes || r->kind == TyKind::Dict ||
                    r->kind == TyKind::Tuple || r->kind == TyKind::Range)
                    return;
                // 类可能实现了 __iter__（或从外部基类继承），静态上看不见
                if (r->kind == TyKind::Class) return;
                err(line, col, std::format("argument of type '{}' is not iterable", type_name(r)));
                return;
        }
    }

TypePtr TypeChecker::subscript_type(TypePtr obj, TypePtr idx, int line, int col) {
        if (is_unknown(obj) || obj->kind == TyKind::Optional) return t_unknown();
        switch (obj->kind) {
            case TyKind::List:
                if (!is_unknown(idx) && idx->kind != TyKind::Int && idx->kind != TyKind::Bool)
                    err(line, col, std::format("list index must be int, got '{}'", type_name(idx)));
                return obj->sub[0];
            case TyKind::Range:
                if (!is_unknown(idx) && idx->kind != TyKind::Int && idx->kind != TyKind::Bool)
                    err(line, col, std::format("range index must be int, got '{}'", type_name(idx)));
                return obj->sub.empty() ? t_int() : obj->sub[0];
            case TyKind::Str:
                if (!is_unknown(idx) && idx->kind != TyKind::Int && idx->kind != TyKind::Bool)
                    err(line, col, std::format("string index must be int, got '{}'", type_name(idx)));
                return t_str();
            case TyKind::Bytes:
                // bytes 下标取的是那个字节的整数值，不是单字节切片
                if (!is_unknown(idx) && idx->kind != TyKind::Int && idx->kind != TyKind::Bool)
                    err(line, col, std::format("bytes index must be int, got '{}'", type_name(idx)));
                return t_int();
            case TyKind::Tuple:
                // 长度不定的元组，下标拿到的就是元素类型
                return obj->variadic && !obj->sub.empty() ? obj->sub[0] : t_unknown();
            case TyKind::Dict:
                if (!assignable(obj->sub[0], idx))
                    err(line, col,
                        std::format("dict key type is '{}', got '{}'", type_name(obj->sub[0]),
                                    type_name(idx)));
                return obj->sub[1];
            case TyKind::Deque:
                return obj->sub.empty() ? t_unknown() : obj->sub[0];
            default:
                // 类名下标是泛型参数（`MyList[int]`、`Generic[T]`），
                // 我们不做泛型，返回 unknown 比判死有用
                if (obj->kind == TyKind::Class) return t_unknown();
                err(line, col, std::format("'{}' object is not subscriptable", type_name(obj)));
                return t_unknown();
        }
    }

TypePtr TypeChecker::iter_elem(TypePtr it, int line, int col) {
        if (is_unknown(it) || it->kind == TyKind::Optional) return t_unknown();
        switch (it->kind) {
            case TyKind::List:
            case TyKind::Set:
                return it->sub[0];
            case TyKind::Range:
                return it->sub.empty() ? t_int() : it->sub[0];
            case TyKind::Str: return t_str();
            case TyKind::Bytes: return t_int();
            case TyKind::Dict: return it->sub[0];
            case TyKind::File: return t_str();  // for line in f
            case TyKind::Tuple: {
                // 同构元组取元素类型；异构元组取公共上界（合不上就是 unknown）。
                // 取第一个元素会让 `for x in (1, "a")` 得到 int，属于误判。
                if (it->sub.empty()) return t_unknown();
                TypePtr acc = it->sub[0];
                for (size_t i = 1; i < it->sub.size(); ++i) acc = join(acc, it->sub[i]);
                return acc;
            }
            case TyKind::Generator:
                return it->sub.empty() ? t_unknown() : it->sub[0];
            case TyKind::Deque:
                return it->sub.empty() ? t_unknown() : it->sub[0];
            default:
                // 类实现了 __iter__ 时，元素类型取它返回的容器/生成器的
                // 元素类型；没有实现就维持 unknown（生成阶段再报错）。
                if (it->kind == TyKind::Class) {
                    const MethodInfo* m = it->cls ? it->cls->method("__iter__") : nullptr;
                    if (m && m->kind == MethodKind::Normal && m->call_sig && m->call_sig->ret) {
                        TypePtr rt = m->call_sig->ret;
                        if ((rt->kind == TyKind::Generator || rt->kind == TyKind::List ||
                             rt->kind == TyKind::Tuple || rt->kind == TyKind::Str) &&
                            !rt->sub.empty())
                            return rt->sub[0];
                    }
                    return t_unknown();
                }
                err(line, col, std::format("'{}' object is not iterable", type_name(it)));
                return t_unknown();
        }
    }


}  // namespace pyadamas
