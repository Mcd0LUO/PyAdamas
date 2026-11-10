// typecheck_builtin.cpp —— 内置函数调用的类型分派
#include "typecheck.h"

namespace py23 {

TypePtr TypeChecker::builtin_call(const std::string& n, Call& c) {
        std::vector<TypePtr> a;
        for (auto& arg : c.args) a.push_back(expr(*arg));
        // 关键字的值在这里求一次就够了，下面按名字分派时只查类型。
        // 再求一次会把同一个未定义名报两遍。
        std::vector<TypePtr> kwt;
        for (auto& kw : c.keywords) kwt.push_back(expr(*kw.value));

        auto need = [&](size_t lo, size_t hi) {
            if (a.size() < lo || a.size() > hi) {
                err(c.line, c.col,
                    std::format("'{}' expects {} argument{}, got {}", n,
                                lo == hi ? std::to_string(lo)
                                         : std::format("{}..{}", lo, hi),
                                hi == 1 ? "" : "s", a.size()));
                return false;
            }
            return true;
        };

        if (n == "print") {
            for (size_t i = 0; i < c.keywords.size(); ++i) {
                auto& kw = c.keywords[i];
                if (kw.arg.empty()) {
                    // print(*args, **kwargs) 是转发包装的常见写法，键名静态不可知。
                    // 放行——后端展开时自然会拒。
                    continue;
                }
                if (kw.arg != "sep" && kw.arg != "end" && kw.arg != "file" && kw.arg != "flush") {
                    err(kw.value->line, kw.value->col,
                        std::format("print() got an unexpected keyword argument '{}'", kw.arg));
                    continue;
                }
                TypePtr kt = kwt[i];
                if (kw.arg == "file") {
                    if (!is_unknown(kt) && (!kt || kt->kind != TyKind::File))
                        err(kw.value->line, kw.value->col,
                            std::format("print() file must be an open file object, got '{}'",
                                        type_name(kt)));
                    continue;
                }
                if (kw.arg == "flush") {
                    if (!is_unknown(kt) && kt && kt->kind != TyKind::Bool &&
                        kt->kind != TyKind::Int)
                        err(kw.value->line, kw.value->col,
                            "print() flush must be a bool");
                    continue;
                }
                if (!is_unknown(kt) && (!kt || kt->kind != TyKind::Str))
                    err(kw.value->line, kw.value->col,
                        std::format("print() '{}' must be a string, got '{}'", kw.arg,
                                    type_name(kt)));
            }
            return t_none();
        }
        if (n == "len") {
            if (a.size() != 1) return t_int();
            if (!is_unknown(a[0]) && a[0]->kind != TyKind::List && a[0]->kind != TyKind::Str &&
                a[0]->kind != TyKind::Dict && a[0]->kind != TyKind::Tuple &&
                a[0]->kind != TyKind::Set && a[0]->kind != TyKind::Class)
                err(c.line, c.col,
                    std::format("object of type '{}' has no len()", type_name(a[0])));
            return t_int();
        }
        if (n == "range") {
            for (auto& t : a)
                if (!is_unknown(t) && t->kind != TyKind::Int)
                    err(c.line, c.col,
                        std::format("'range' argument must be int, got '{}'", type_name(t)));
            return mk_list(t_int());
        }
        if (n == "abs") {
            if (a.empty()) return t_unknown();
            if (!is_unknown(a[0]) && !is_num(a[0]) &&
                (!a[0] || a[0]->kind != TyKind::Complex))
                err(c.line, c.col,
                    std::format("bad operand type for abs(): '{}'", type_name(a[0])));
            if (a[0] && a[0]->kind == TyKind::Complex) return t_float();
            return a[0];
        }
        if (n == "min" || n == "max") {
            if (a.empty()) return t_unknown();
            if (a.size() == 1) return iter_elem(a[0], c.line, c.col);
            TypePtr acc = a[0];
            for (size_t i = 1; i < a.size(); ++i) acc = join(acc, a[i]);
            return acc;
        }
        if (n == "sum") {
            if (!a.empty()) {
                auto e = iter_elem(a[0], c.line, c.col);
                if (!is_unknown(e) && !is_num(e))
                    err(c.line, c.col,
                        std::format("sum() over non-numeric '{}'", type_name(e)));
                TypePtr res = is_num(e) ? e : t_int();
                // start 参数参与数值域提升：sum([], 0.5) 是 float。
                if (a.size() > 1 && is_num(a[1])) res = promote(res, a[1]);
                return res;
            }
            return t_int();
        }
        if (n == "sorted" || n == "reversed") {
            if (a.empty()) return mk_list(t_unknown());
            auto e = iter_elem(a[0], c.line, c.col);
            return mk_list(e);
        }
        if (n == "enumerate") {
            if (a.empty()) return mk_list(mk_tuple({t_int(), t_unknown()}));
            auto e = iter_elem(a[0], c.line, c.col);
            return mk_list(mk_tuple({t_int(), e}));
        }
        if (n == "zip") {
            std::vector<TypePtr> elems;
            for (auto& t : a) elems.push_back(iter_elem(t, c.line, c.col));
            return mk_list(mk_tuple(std::move(elems)));
        }
        if (n == "int") {
            if (a.size() > 2)
                err(c.line, c.col, std::format("int() takes at most 2 arguments, got {}", a.size()));
            else if (a.size() == 2 && !is_unknown(a[0]) && (!a[0] || a[0]->kind != TyKind::Str))
                err(c.line, c.col,
                    std::format("int() can't convert non-string with explicit base: '{}'",
                                type_name(a[0])));
            return t_int();
        }
        if (n == "float") return t_float();
        if (n == "str") return t_str();
        if (n == "bool") return t_bool();
        if (n == "list") {
            if (a.empty()) return mk_list(fresh_hole());
            return mk_list(iter_elem(a[0], c.line, c.col));
        }
        if (n == "tuple") {
            if (a.empty()) return mk_tuple({});
            return mk_tuple_rest(iter_elem(a[0], c.line, c.col));
        }
        if (n == "set") {
            if (a.empty()) return mk_set(fresh_hole());
            return mk_set(iter_elem(a[0], c.line, c.col));
        }
        if (n == "dict") return mk_dict(t_unknown(), t_unknown());
        if (n == "ord") return t_int();
        if (n == "chr") return t_str();
        if (n == "repr") return t_str();
        if (n == "hex" || n == "bin" || n == "oct") return t_str();
        if (n == "isinstance") {
            if (!need(2, 2)) return t_bool();
            TypePtr tt = a[1];
            bool ok = tt && tt->kind == TyKind::Class;
            if (!ok && tt && tt->kind == TyKind::Func && tt->fn) {
                static const std::unordered_set<std::string> types = {
                    "int", "float", "str", "bool", "list",
                    "tuple", "dict", "set", "complex", "object",
                };
                ok = types.count(tt->fn->name) > 0;
            }
            if (!ok && !is_unknown(tt))
                err(c.args[1]->line, c.args[1]->col,
                    "isinstance() second argument must be a class or builtin type");
            return t_bool();
        }
        if (n == "callable") {
            if (!need(1, 1)) return t_bool();
            return t_bool();
        }
        if (n == "hash") {
            if (!need(1, 1)) return t_int();
            return t_int();
        }
        if (n == "type") {
            if (!need(1, 1)) return t_unknown();
            return mk_typeobj(a.empty() ? t_unknown() : a[0]);
        }
        if (n == "frozenset") {
            if (a.size() > 1)
                err(c.line, c.col, "frozenset() takes at most one argument");
            return a.empty() ? mk_set(t_unknown()) : mk_set(iter_elem(a[0], c.line, c.col));
        }
        if (n == "id") {
            if (!need(1, 1)) return t_int();
            return t_int();
        }
        if (n == "issubclass") {
            if (!need(2, 2)) return t_bool();
            return t_bool();
        }
        if (n == "hasattr" || n == "getattr" || n == "setattr") {
            size_t want = n == "setattr" ? 3 : 2;
            if (!need(want, want))
                return n == "hasattr" ? t_bool() : n == "getattr" ? t_unknown() : t_none();
            auto* ne = c.args[1].get();
            const std::string* an = nullptr;
            if (ne->kind == ExprKind::Constant) {
                auto& cc = static_cast<Constant&>(*ne);
                if (cc.ck == Constant::CK::Str) an = &cc.s;
            }
            if (!an) {
                err(ne->line, ne->col,
                    std::format("{}() attribute name must be a string literal", n));
                return n == "hasattr" ? t_bool() : n == "getattr" ? t_unknown() : t_none();
            }
            TypePtr ot = a[0];
            if (ot && ot->kind == TyKind::Optional && !ot->sub.empty()) ot = ot->sub[0];
            if (!ot || ot->kind != TyKind::Class || !ot->cls) {
                err(c.args[0]->line, c.args[0]->col,
                    std::format("{}() is only supported on class instances", n));
                return n == "hasattr" ? t_bool() : n == "getattr" ? t_unknown() : t_none();
            }
            if (n == "hasattr") return t_bool();
            if (!ot->cls->complete && ot->cls != cur_class_) return t_unknown();
            if (const PropertyInfo* p = ot->cls->property(*an)) {
                if (n == "setattr") {
                    err(c.line, c.col,
                        std::format("setattr() of property '{}' is not supported yet", *an));
                    return t_none();
                }
                if (!p->owner || p->getter < 0 ||
                    p->getter >= static_cast<int>(p->owner->methods.size()))
                    return t_unknown();
                const MethodInfo& gm = p->owner->methods[p->getter];
                return gm.call_sig && gm.call_sig->ret ? gm.call_sig->ret : t_unknown();
            }
            if (const FieldInfo* f = ot->cls->field(*an)) {
                if (n == "getattr") return f->ty;
                return t_none();
            }
            if (ot->cls->method(*an) && n == "getattr") {
                err(c.line, c.col,
                    std::format("getattr() of method '{}' must be called with ()", *an));
                return t_unknown();
            }
            if (ot->cls->is_open()) return t_unknown();
            err(c.line, c.col,
                std::format("'{}' object has no field '{}'", ot->cls->name, *an));
            return n == "getattr" ? t_unknown() : t_none();
        }
        if (n == "any" || n == "all") return t_bool();
        if (n == "round") {
            if (a.empty()) return t_unknown();
            if (a[0] && a[0]->kind == TyKind::Float)
                return a.size() >= 2 ? t_float() : t_int();
            return a[0];
        }
        if (n == "divmod") {
            if (a.size() >= 2 && is_num(a[0]) && is_num(a[1])) {
                auto q = promote(a[0], a[1]);
                return mk_tuple({q, q});
            }
            return mk_tuple({t_unknown(), t_unknown()});
        }
        if (n == "pow") return a.size() >= 2 ? promote(a[0], a[1]) : t_unknown();
        if (n == "input") return t_str();
        if (n == "type") return t_unknown();
        if (n == "next") {
            return a.empty() ? t_unknown() : iter_elem(a[0], c.line, c.col);
        }
        if (n == "iter") {
            return a.empty() ? t_unknown() : a[0];
        }
        if (n == "complex") {
            if (a.size() > 2)
                err(c.line, c.col, std::format("complex() takes at most 2 arguments, got {}", a.size()));
            return t_complex();
        }
        if (n == "open") {
            static const char* kParams[] = {"file",      "mode",    "buffering", "encoding",
                                            "errors",    "newline", "closefd",   "opener"};
            constexpr size_t kNP = 8;
            std::vector<TypePtr> bound(kNP, nullptr);
            std::vector<Expr*> bexpr(kNP, nullptr);
            if (a.size() > kNP) {
                err(c.line, c.col, "open() takes at most 8 arguments");
                return t_file();
            }
            for (size_t i = 0; i < a.size(); ++i) {
                bound[i] = a[i];
                bexpr[i] = c.args[i].get();
            }
            for (size_t i = 0; i < c.keywords.size(); ++i) {
                const std::string& k = c.keywords[i].arg;
                Expr* ve = c.keywords[i].value.get();
                if (k.empty()) {
                    err(ve->line, ve->col, "open() does not take '**kwargs'");
                    continue;
                }
                int idx = -1;
                for (int p = 0; p < static_cast<int>(kNP); ++p)
                    if (k == kParams[p]) idx = p;
                if (idx < 0) {
                    err(ve->line, ve->col,
                        std::format("open() got an unexpected keyword argument '{}'", k));
                    continue;
                }
                if (bound[static_cast<size_t>(idx)]) {
                    err(ve->line, ve->col,
                        std::format("open() got multiple values for argument '{}'", k));
                    continue;
                }
                bound[static_cast<size_t>(idx)] = kwt[i];
                bexpr[static_cast<size_t>(idx)] = ve;
            }
            if (!bound[0]) err(c.line, c.col, "open() missing required argument 'file'");
            auto str_or_none = [](const TypePtr& t) {
                return !t || is_unknown(t) || t->kind == TyKind::Str || t->kind == TyKind::None;
            };
            auto int_like = [](const TypePtr& t) {
                return !t || is_unknown(t) || t->kind == TyKind::Int || t->kind == TyKind::Bool;
            };
            if (bound[0] && !is_unknown(bound[0]) && bound[0]->kind != TyKind::Str &&
                bound[0]->kind != TyKind::Int && bound[0]->kind != TyKind::Bool)
                err(c.line, c.col,
                    std::format("open() file must be a path string or file descriptor, got '{}'",
                                type_name(bound[0])));
            if (!str_or_none(bound[1])) err(c.line, c.col, "open() mode must be a string");
            if (!int_like(bound[2]))
                err(c.line, c.col, "open() buffering must be an integer");
            if (!str_or_none(bound[3]))
                err(c.line, c.col, "open() encoding must be a string or None");
            if (!str_or_none(bound[4]))
                err(c.line, c.col, "open() errors must be a string or None");
            if (!str_or_none(bound[5]))
                err(c.line, c.col, "open() newline must be a string or None");
            if (!int_like(bound[6])) err(c.line, c.col, "open() closefd must be a bool");
            if (bound[7]) {
                err(c.line, c.col, "open() opener= is not supported by the code generator");
                return t_file();
            }
            return t_file();
        }
        if (n == "super" || n == "vars" || n == "dir" ||
            n == "slice" ||
            n == "bytearray" || n == "object" || n == "complex" || n == "memoryview") {
            unsupported.push_back(std::format("runtime builtin '{}'", n));
            if (n == "bytearray") return a.empty() ? t_unknown() : mk_list(t_unknown());
            if (n == "format") return t_str();
            return t_unknown();
        }
        if (n == "exec" || n == "eval" || n == "globals" || n == "locals" || n == "compile" ||
            n == "__import__") {
            // 只登记，不报错：能否支持由代码生成阶段决定，类型层面只能返回 unknown
            unsupported.push_back(std::format("dynamic builtin '{}'", n));
            return t_unknown();
        }
        if (n == "map" || n == "filter") {
            if (a.size() >= 2) return mk_list(iter_elem(a[1], c.line, c.col));
            return mk_list(t_unknown());
        }
        (void)need;
        return t_unknown();
    }

}  // namespace py23
