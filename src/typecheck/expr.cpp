// typecheck/expr.cpp —— 表达式类型检查：运算、容器、推导式、迭代
#include "typecheck/typecheck.h"

#include "ir/module.h"

namespace pyadamas {

// 类型名在值位置（argparse 的 type=int、[int, str]、t is int）：
// 类型对象携带它命名的类型；机器表示是类型名字符串（与 type(x) 一致）。
// object/type/bytearray 没有对应内部类型，sub 留空，名字取字面量。
static TypePtr type_name_value(const std::string& id) {
    if (id == "int") return mk_typeobj(t_int());
    if (id == "float") return mk_typeobj(t_float());
    if (id == "str") return mk_typeobj(t_str());
    if (id == "bool") return mk_typeobj(t_bool());
    if (id == "bytes") return mk_typeobj(t_bytes());
    if (id == "complex") return mk_typeobj(t_complex());
    if (id == "list") return mk_typeobj(mk_list(t_unknown()));
    if (id == "dict") return mk_typeobj(mk_dict(t_unknown(), t_unknown()));
    if (id == "tuple") return mk_typeobj(mk_tuple_rest(t_unknown()));
    if (id == "set" || id == "frozenset") return mk_typeobj(mk_set(t_unknown()));
    return mk_typeobj(nullptr);
}

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
                    case Constant::CK::Int:
                        // u64 大字面量：静态类型仍按 int 登记（提升规则不变），
                        // 收尾检查未被 u64 上下文消费的照旧报 too large。
                        if (c.u64_lit) note_u64_literal(&e);
                        return t_int();
                    case Constant::CK::Big: return t_big();   // P3：超 u64 字面量
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
                // 0.537：同一块内 `del y` 之后再读 y → 报错（CPython 是 NameError）。
                if (deleted_names_.count(n.id)) {
                    err(n.line, n.col,
                        std::format("name '{}' is not defined (deleted above)", n.id));
                    return t_unknown();
                }
                // 类体命名空间优先于模块全局：Python 的类体里，前面几行
                // 赋过的名字就是类命名空间的成员，会遮住同名全局。
                if (auto it = class_body_attrs_.find(n.id); it != class_body_attrs_.end())
                    return it->second;
                if (auto* sym = lookup(n.id)) {
                    // 类名在值位置是运行期类对象，不是实例。异常类仍按
                    // 类型名使用（raise/except/throw 的既有路径）。
                    // P4（lang_gaps）：异常类名在值位置也是 ClassObj（与
                    // 内建异常类一致）——raise/except 的名字机制按名字查
                    // classes_，不受表达式类型影响。门控关时异常类保持
                    // Class 类型（旧语义，asm 不变）。
                    if (sym->ty && sym->ty->kind == TyKind::Class && name_is_class(n) &&
                        (!sym->ty->cls || !sym->ty->cls->is_exception_class || lang_gaps_)) {
                        if (sym->ty->cls) sym->ty->cls->value_used = true;
                        return mk_class_obj(sym->ty);
                    }
                    return sym->ty;
                }
                if (lookup_module_alias(n.id)) {
                    // 0.484（pya-lsp 缺口 2 的配套）：**模块名当值用不再报错**。
                    // 反射语义（hasattr(os, "fork") / getattr(os, name, d)）是第一
                    // 等用例，静态能证明的只有「它是个模块」；具体成员是否存在由
                    // build 的反射路径决定。check 比 build 严格会造成误杀
                    // （tests/11_reflect.py），这里按「宁可漏报不可误杀」降级成
                    // unknown 并记为未建模构造。
                    unsupported.push_back("module value '" + n.id + "'");
                    return t_unknown();
                }
                if (cmodules_.count(n.id)) {
                    err(n.line, n.col,
                        std::format("cimport '{}' cannot be used as a value", n.id));
                    return t_unknown();
                }
                // 先看内置值（__name__ / __file__ / Ellipsis ...），再看可调用对象
                if (auto vt = builtin_value_type(n.id)) return vt;
                // 类型名当值用：类型对象（不是函数签名）。直接调用 int(x)
                // 仍按名字分派（call_name），不受影响。
                if (is_type_name(n.id)) return type_name_value(n.id);
                if (is_builtin_callable(n.id)) return builtin_type(n.id);
                // P4（lang_gaps）：内建异常类当值 → 运行期类对象（ClassObj，
                // CLASS tag=11）。内层用 Exception[name] 记类型名（没有
                // ClassInfo：内建异常无编译期布局）。门控关时仍是 unknown/0。
                if (is_exception_name(n.id))
                    return lang_gaps_ ? mk_class_obj(mk_exception(n.id)) : t_unknown();
                if (n.id.starts_with("__") && n.id.ends_with("__")) return t_unknown();
                // 有 from x import * 时无从知道导入了哪些名字，只能放行
                if (star_import_) return t_unknown();
                // 模块里出现过 globals()[k] = v：名字集合是动态的
                if (dynamic_globals_) return t_unknown();
                // 模块级通常是条件导入、C 扩展常量、__all__ 之外的名字，
                // 静态无从判断；只在函数体内确定性报错。类体/模块体
                // 的名字可能在后文或其它模块定义，conservative 放过。
                if (cur_fn_scope_ || strict_module_names_)
                    err(n.line, n.col, std::format("undefined name '{}'", n.id));
                else
                    unsupported.push_back("undefined module-scope name '" + n.id + "'");
                return t_unknown();
            }
            case ExprKind::JoinedStr: {
                // f-string 里嵌的表达式必须真的检查一遍：以前直接 return
                // t_str()，导致 `f"{undefined_name}"` 不报错，降级阶段也拿不到
                // 内嵌表达式的类型（str 被当成 int 格式化）。
                auto& j = static_cast<JoinedStr&>(e);
                for (auto& v : j.values) expr(*v);
                // 0.567：t"..." 是 Template 对象，不是 str。
                return j.is_template ? t_template() : t_str();
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
                // 0.565：`[*a, x]` 里的 `*a` 贡献的是 a 的**元素类型**，不是 list 本身。
                auto elem_of = [&](Expr* el) -> TypePtr {
                    if (el->kind != ExprKind::Starred) return expr(*el);
                    TypePtr it = expr(*static_cast<Starred*>(el)->value);
                    if (it && (it->kind == TyKind::List || it->kind == TyKind::Tuple) &&
                        !it->sub.empty())
                        return it->sub[0];
                    return t_unknown();
                };
                TypePtr acc = elem_of(l.elts[0].get());
                bool all_same = true;
                for (size_t i = 1; i < l.elts.size(); ++i) {
                    auto t = elem_of(l.elts[i].get());
                    if (!type_eq(acc, t)) all_same = false;
                    acc = join(acc, t);
                }
                (void)all_same;
                // 异构列表用 unknown 单例（不可填），不用 fresh_hole：
                // 洞会被目标类型单态化（形参回填/下标写入），把 [1, "x"]
                // 变成 list[str] 后字面量按 kind1 建表，int 元素当堆指针
                // dec → 段错误（206_list_store_kind 实测）。混合内容本来
                // 就不该被单态化。
                if (is_unknown(acc)) return mk_list(t_unknown());
                // 全 None 的列表字面量是「占位容器」惯用法（`[None] * 256` 之后在
                // import 期填满、`[None, None]` 之后再赋值）。按 list[None] 推断会
                // 让每个元素读取都变成「可能为 None」，而真实数据从别处写进来。
                // 这类字面量按 list[unknown] 处理：读出来是 unknown（不误报），
                // 标注成 list[int] 等的**不影响**（那是标注路径，不走这里）。
                if (acc && acc->kind == TyKind::None) return mk_list(t_unknown());
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
                TypePtr acc;
                for (auto& el : s.elts) {
                    TypePtr et = expr(*el);
                    // big 键已支持：哈希键种类 9（raw-or-big，按值哈希+比较）。
                    acc = acc ? join(acc, et) : et;
                }
                return mk_set(acc);
            }
            case ExprKind::Dict: {
                auto& d = static_cast<DictExpr&>(e);
                if (d.keys.empty()) return mk_dict(fresh_hole(), fresh_hole());
                TypePtr kt = nullptr, vt = nullptr;
                for (size_t i = 0; i < d.keys.size(); ++i) {
                    if (d.keys[i]) {
                        auto k = expr(*d.keys[i]);
                        // big 键已支持（哈希键种类 9）。
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
                TypePtr bt = binop_type(b.op, l, r, b.line, b.col, b.left.get(), b.right.get());
                // 提升到 U64 的运算：u64 大字面量操作数按 U64 定型消费。
                consume_u64_literal(bt, b.left.get());
                consume_u64_literal(bt, b.right.get());
                return bt;
            }
            case ExprKind::UnaryOp: {
                auto& u = static_cast<UnaryOp&>(e);
                auto t = expr(*u.operand);
                switch (u.op) {
                    case Tok::KW_NOT: return t_bool();
                    case Tok::MINUS:
                    case Tok::PLUS:
                    case Tok::TILDE:
                        // 用户类的一元运算走 __neg__/__pos__/__invert__，结果
                        // 类型就是方法声明的返回类型。以前这里一律返回操作数
                        // 自己的类型（Class），降级层于是把对象指针当整数取负，
                        // 再按堆类型做引用计数——野指针，运行期 SIGSEGV。
                        if (t && t->kind == TyKind::Class) {
                            const char* dn = u.op == Tok::MINUS    ? "__neg__"
                                             : u.op == Tok::PLUS   ? "__pos__"
                                                                   : "__invert__";
                            if (TypePtr r = dunder_unary_ret(dn, t)) return r;
                            // 基类来源未知的开放类：静态看不见 dunder，交给
                            // 降级层（那里同样会报错，不会静默算错）。
                            if (t->cls && t->cls->is_open()) return t_unknown();
                            err(u.line, u.col,
                                std::format("bad operand type for unary {}: '{}'",
                                            tok_name(u.op), type_name(t)));
                            return t_unknown();
                        }
                        if (u.op == Tok::TILDE) {
                            if (is_fixed_int(t)) return t;  // 定宽保持宽度/符号
                            if (t && t->kind == TyKind::Big) return t;   // P3：~big
                            if (is_unknown(t) || is_int_like(t)) return t_int();
                            err(u.line, u.col, "bad operand type for '~': expected int");
                            return t_unknown();
                        }
                        // bool 是 Python 的 int 子类：+True / -True 的结果类型是 int
                        // （CPython 打印 1 / -1）。以前这里返回操作数自身的 Bool 类型：
                        // 值算对了（-1/1），但按 bool 打印成 True/False，类型也错。
                        if (t && t->kind == TyKind::Bool) return t_int();
                        // 绑定方法可能重载了一元运算符（@property 返回的数值最
                        // 常见），静态上看不见
                        if (is_unknown(t) || is_num(t) || t->kind == TyKind::Complex ||
                            t->kind == TyKind::BoundMethod || t->kind == TyKind::TimeDelta)
                            return t;
                        err(u.line, u.col,
                            std::format("bad operand type for unary operator: '{}'", type_name(t)));
                        return t_unknown();
                    default: return t_unknown();
                }
            }
            case ExprKind::BoolOp: {
                auto& b = static_cast<BoolOp&>(e);
                TypePtr acc = nullptr;
                bool has_big = false, has_other = false;
                bool has_float = false, has_int = false;
                // 短路内的窄化（0.498）：`a and a.foo()` 的右操作数只在左侧为真时
                // 求值，所以左操作数的真值窄化要作用到后面所有元素；`or` 用假值
                // 方向（`x is None or x.foo()`）。求值完恢复原类型。
                std::vector<Narrowing> active;
                for (size_t vi = 0; vi < b.values.size(); ++vi) {
                    auto& v = b.values[vi];
                    auto t = expr(*v);
                    has_big |= t->kind == TyKind::Big;
                    has_other |= t->kind != TyKind::Big;
                    has_float |= t->kind == TyKind::Float;
                    has_int |= t->kind == TyKind::Int || t->kind == TyKind::Bool;
                    acc = acc ? join(acc, t) : t;
                    if (vi + 1 < b.values.size()) {
                        auto n = narrow_on(*v, b.op == Tok::KW_AND);
                        if (n.sym) active.push_back(n);
                    }
                }
                for (auto& n : active) restore_narrowing(n);
                // and/or 保留被选中的值；不能把标量当成 big 指针。
                if (has_big && has_other) return t_unknown();
                // 0.672：int 与 float 混在一起时，结果类型是**动态**的——
                // `1 or 2.5` 在 CPython 里返回 int 1，`0 or 2.5` 返回 float 2.5。
                // 若把 join 后的 Float 当成静态类型，降级会把选中的 int 也 coerce 成
                // float（实测 `print(1 or 2.5)` 打印 1.0，属静默错值）。
                // 交给 unknown 域：int 原样、float 装箱，运行期按实际值分派。
                if (has_float && has_int) return t_unknown();
                return acc ? acc : t_unknown();
            }
            case ExprKind::Compare: {
                auto& c = static_cast<Compare&>(e);
                auto l = expr(*c.left);
                for (size_t i = 0; i < c.ops.size(); ++i) {
                    auto r = expr(*c.comparators[i]);
                    compare_type(c.ops[i], l, r, c.line, c.col);
                    // 链式比较逐对提升：提升到 U64 时消费两侧的 u64 大字面量。
                    TypePtr pt = promote(l, r);
                    consume_u64_literal(pt, i == 0 ? c.left.get() : c.comparators[i - 1].get());
                    consume_u64_literal(pt, c.comparators[i].get());
                    l = r;
                }
                return t_bool();
            }
            case ExprKind::Call: {
                TypePtr cty = call(static_cast<Call&>(e));
                if (type_stats_on_ && is_unknown(cty)) {
                    auto& cc = static_cast<Call&>(e);
                    std::string nm = "<expr>";
                    std::string kind = "other";
                    if (auto* n = dynamic_cast<Name*>(cc.func.get())) {
                        nm = n->id;
                        if (auto* sym = lookup(n->id); sym && sym->ty &&
                            sym->ty->kind == TyKind::Func) {
                            kind = (nm.find("__pyadamas$lambda") != std::string::npos ||
                                    nm.find("lambda") != std::string::npos)
                                       ? "lambda" : "user";
                        } else {
                            kind = "other";
                        }
                    } else if (auto* a = dynamic_cast<Attribute*>(cc.func.get())) {
                        nm = a->attr;
                        TypePtr bt = type_of(a->value.get());
                        if (bt && bt->kind == TyKind::Class) kind = "method";
                        else if (auto* bn = dynamic_cast<Name*>(a->value.get());
                                 bn && lookup_module_alias(bn->id)) {
                            kind = "module";
                            nm = bn->id + "." + a->attr;
                        } else if (bt && bt->kind == TyKind::DYLIB) {
                            kind = "dylib";
                        } else if (bt && is_unknown(bt)) {
                            kind = "unknown_recv";
                        }
                    }
                    type_stat("call_return_" + kind, nm);
                    std::string ckey = nm;
                    for (char& ch : ckey)
                        if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
                    type_stat("callname_" + kind + "_" + ckey, "");
                    type_stat("call_callee", nm);
                    char loc[96];
                    snprintf(loc, sizeof(loc), "%s:%d", nm.c_str(), cc.line);
                    type_stat("call_site_" + kind, loc);
                }
                return cty;
            }
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
                    if (ot->kind == TyKind::StructTime) return mk_tuple_rest(t_int());
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
                // .cpya 定长数组的地址视图（多维 / struct 元素）：
                // 中间层下标得到下一层地址，最后一层得到元素地址（struct 元素）。
                if (ot && ot->kind == TyKind::Optional && !ot->cdims.empty() &&
                    ot->sub.size() == 1 && ot->sub[0]->kind == TyKind::Int) {
                    if (!is_unknown(it) && !is_int_like(it))
                        err(s.line, s.col, "pointer index must be an integer");
                    if (ot->cdims.size() > 1) {
                        TypePtr nt = mk_optional(t_int());
                        nt->cdims.assign(ot->cdims.begin() + 1, ot->cdims.end());
                        nt->st = ot->st;
                        nt->celem = ot->celem;
                        return nt;
                    }
                    if (ot->st) return mk_optional(mk_struct_ptr(ot->st));
                }
                // ptr[T] 解引用：Optional[int] 上带元素种类时支持 p[i]。
                if (is_cptr(ot)) {
                    if (!is_unknown(it) && !is_int_like(it))
                        err(s.line, s.col, "pointer index must be an integer");
                    return cptr_elem_type(ot->celem);
                }
                // 定宽 dict 键 + 字面量下标（读路径）：范围内隐式 coerce，
                // 与键的写入/字面量建表同口径。写路径在 assign() 里同法处理。
                if (ot && ot->kind == TyKind::Dict && !ot->sub.empty() &&
                    is_fixed_int(ot->sub[0]) && accept_literal_into(ot->sub[0], s.slice.get()))
                    it = ot->sub[0];
                return subscript_type(ot, it, s.line, s.col);
            }
            case ExprKind::Attribute:
                return expr_attribute(static_cast<Attribute&>(e));
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
            case ExprKind::Go: {
                auto& g = static_cast<GoExpr&>(e);
                (void)expr(*g.call);
                auto* call = dynamic_cast<Call*>(g.call.get());
                if (!call) {
                    err(e.line, e.col, "go expects a function call, e.g. 'go f(args)'");
                    return mk_vthread();
                }
                TypePtr ft = expr(*call->func);
                if (!ft || ft->kind != TyKind::Func || !ft->fn) {
                    err(e.line, e.col,
                        "go expects a function value with a known signature");
                    return mk_vthread();
                }
                FuncInfoPtr fn = ft->fn;
                if (fn->unchecked)
                    err(e.line, e.col, "go expects a user-defined function");
                if (fn->is_generator || fn->is_async || fn->is_async_generator)
                    err(e.line, e.col,
                        "go cannot spawn a generator or coroutine (use plain functions)");
                // 实参绑定与普通调用一致（lower_go 走 bind_args）：默认参数、关键字参数、
                // *args/**kwargs 都支持，这里不再单独设限。
                return mk_vthread();
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
                        // 0.615：bytes / deque / 用户类（__iter__）也允许 yield from，
                        // 降级层先物化成 list（或调 __iter__）再按下标迭代。
                        case TyKind::Bytes:
                            elem = t_int();
                            break;
                        case TyKind::Deque:
                            elem = vt->sub.empty() ? t_unknown() : vt->sub[0];
                            break;
                        case TyKind::Class:
                            elem = t_unknown();
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

void TypeChecker::compare_type(CmpOp op, TypePtr l, TypePtr r, int line, int col) {
        auto container = [](const TypePtr& t) {
            return t && (t->kind == TyKind::List || t->kind == TyKind::Tuple ||
                         t->kind == TyKind::Dict || t->kind == TyKind::Set);
        };
        // big 元素的比较已经支持（list/tuple 走按值比较，运行时 kind 4 / 链 'b'）。
        // 仍然不支持的两类：
        //   - dict/set 参与比较或作为 in 的右操作数：键哈希还没按值做；
        //   - 有序比较（< <= > >=）的容器：元素级序比较还没实现。
        auto big_ok_container = [](const TypePtr& t) {
            return t && (t->kind == TyKind::List || t->kind == TyKind::Tuple);
        };
        auto bad_for_big = [&](const TypePtr& t) {
            if (!container(t) || !has_big_value(t)) return false;
            // dict/set：只支持「作为 in 的右操作数」（键走 kind 9 按值哈希）；
            // 两个容器之间的相等比较还没有按值实现。
            if (t->kind == TyKind::Dict || t->kind == TyKind::Set)
                return op != CmpOp::In && op != CmpOp::NotIn;
            if (!big_ok_container(t)) return true;
            return op != CmpOp::Eq && op != CmpOp::Ne && op != CmpOp::In &&
                   op != CmpOp::NotIn;                          // 有序比较
        };
        if (op != CmpOp::Is && op != CmpOp::IsNot &&
            (bad_for_big(l) || bad_for_big(r) ||
             ((op == CmpOp::In || op == CmpOp::NotIn) && container(r) &&
              has_big_value(l) && !big_ok_container(r) && r->kind != TyKind::Dict &&
              r->kind != TyKind::Set))) {
            err(line, col, "container comparisons involving big are not supported");
            return;
        }
        if (is_unknown(l) || is_unknown(r)) return;
        if (l->kind == TyKind::Optional || r->kind == TyKind::Optional) {
            // 与 None 的 is/is not/==/!= 是判空本身，不报；排序比较要先判空（0.498）。
            const bool ordering = op == CmpOp::Lt || op == CmpOp::Le || op == CmpOp::Gt ||
                                  op == CmpOp::Ge;
            if (ordering) {
                err(line, col, "this value may be None here; check it first "
                               "(if x is not None:) or use assert");
            }
            return;
        }
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
                // None 与任何有序类型比较：常见于 optional 守卫没窄化；
                // 保守放行（运行时才真正决定合法性）。
                if (l->kind == TyKind::None || r->kind == TyKind::None) return;
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
                    r->kind == TyKind::Tuple || r->kind == TyKind::Range ||
                    r->kind == TyKind::None)
                    return;
                // 类可能实现了 __iter__（或从外部基类继承），静态上看不见
                if (r->kind == TyKind::Class) return;
                err(line, col, std::format("argument of type '{}' is not iterable", type_name(r)));
                return;
        }
    }

TypePtr TypeChecker::subscript_type(TypePtr obj, TypePtr idx, int line, int col) {
        if (is_unknown(obj)) return t_unknown();
        if (obj->kind == TyKind::Optional) {
            // 没有表达式可指，消息用泛化主语（0.498）。
            err(line, col, "this value may be None here; check it first "
                           "(if x is not None:) or use assert");
            return t_unknown();
        }
        switch (obj->kind) {
            case TyKind::List:
                if (!is_unknown(idx) && !is_int_like(idx))
                    err(line, col, std::format("list index must be int, got '{}'", type_name(idx)));
                return obj->sub[0];
            case TyKind::Buffer:  // 0.536：连续内存按下标读写
                if (!is_unknown(idx) && !is_int_like(idx))
                    err(line, col, std::format("buffer index must be int, got '{}'", type_name(idx)));
                return obj->sub.empty() ? t_unknown() : obj->sub[0];
            case TyKind::Range:
                if (!is_unknown(idx) && !is_int_like(idx))
                    err(line, col, std::format("range index must be int, got '{}'", type_name(idx)));
                return obj->sub.empty() ? t_int() : obj->sub[0];
            case TyKind::Str:
                if (!is_unknown(idx) && !is_int_like(idx))
                    err(line, col, std::format("string index must be int, got '{}'", type_name(idx)));
                return t_str();
            case TyKind::Bytes:
                // bytes 下标取的是那个字节的整数值，不是单字节切片
                if (!is_unknown(idx) && !is_int_like(idx))
                    err(line, col, std::format("bytes index must be int, got '{}'", type_name(idx)));
                return t_int();
            case TyKind::Tuple:
                // 长度不定的元组，下标拿到的就是元素类型
                return obj->variadic && !obj->sub.empty() ? obj->sub[0] : t_unknown();
            case TyKind::Dict:
                // big 键已支持（哈希键种类 9，按值哈希 + pa_big_eq）。
                if (!assignable(obj->sub[0], idx))
                    err(line, col,
                        std::format("dict key type is '{}', got '{}'", type_name(obj->sub[0]),
                                    type_name(idx)));
                return obj->sub[1];
            case TyKind::Deque:
                return obj->sub.empty() ? t_unknown() : obj->sub[0];
            case TyKind::StructTime:
                // time.struct_time 的下标形态：9 个 int（旧 tuple 语义，0.518 保留）。
                return t_int();
            default:
                // 0.565：类名下标是泛型参数（`Box[int]`）—— 我们不做泛型，擦除即可，
                // 让它能继续用于构造（`Box[int](3)`）。
                if (obj->kind == TyKind::ClassObj) return obj;
                // 类名下标是泛型参数（`MyList[int]`、`Generic[T]`），
                // 我们不做泛型，返回 unknown 比判死有用
                if (obj->kind == TyKind::Class) {
                    // A1：类实例下标 `b[i]` 的类型取 `__getitem__` 的返回标注。
                    // 以前一律 unknown，导致 `print(b[1])` / `str(b[1])` / 容器字面量
                    // 把 float 结果按机器字（double 位模式）处理 —— 静默错值。
                    if (obj->cls) {
                        if (const MethodInfo* gi = obj->cls->method("__getitem__"))
                            if (gi->call_sig && gi->call_sig->ret) return gi->call_sig->ret;
                    }
                    return t_unknown();
                }
                err(line, col, std::format("'{}' object is not subscriptable", type_name(obj)));
                return t_unknown();
        }
    }

TypePtr TypeChecker::iter_elem(TypePtr it, int line, int col) {
        if (is_unknown(it) || it->kind == TyKind::Optional) return t_unknown();
        // enum 类对象：迭代产出成员对象（for c in Color / list(Color)）。
        if (it->kind == TyKind::ClassObj && !it->sub.empty() && it->sub[0] &&
            it->sub[0]->cls && it->sub[0]->cls->is_enum)
            return mk_class(it->sub[0]->cls);
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
            case TyKind::None:
                // None 被迭代：多是 optional 守卫没窄化，conservative。
                return t_unknown();
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
