// typecheck_call_modules.cpp —— 标准库模块调用的类型规则
#include "typecheck.h"

#include "module.h"

namespace pyadamas {

std::optional<TypePtr> TypeChecker::asyncio_call(const std::string& member, Call& c) {
        if (member != "run" && member != "sleep" && member != "gather" &&
            member != "create_task" && member != "Future" &&
            member != "wait_readable" && member != "wait_writable" &&
            member != "Event" && member != "Lock" && member != "Queue" &&
            member != "wait_for" && member != "timeout" && member != "TaskGroup" &&
            member != "Semaphore")
            return std::nullopt;

        std::vector<TypePtr> a;
        for (auto& arg : c.args) a.push_back(expr(*arg));
        for (auto& kw : c.keywords) expr(*kw.value);
        if (!c.keywords.empty())
            err(c.line, c.col, std::format("asyncio.{}() does not take keyword arguments", member));

        if (member == "run") {
            if (a.size() != 1) {
                err(c.line, c.col, std::format("asyncio.run() expects 1 argument, got {}", a.size()));
                return t_unknown();
            }
            if (!a[0] || a[0]->kind == TyKind::Unknown) return t_unknown();
            if (a[0]->kind == TyKind::Coroutine)
                return a[0]->sub.empty() ? t_unknown() : a[0]->sub[0];
            err(c.line, c.col,
                std::format("asyncio.run() expects a coroutine, got '{}'", type_name(a[0])));
            return t_unknown();
        }

        if (member == "sleep") {
            if (a.size() != 1) {
                err(c.line, c.col,
                    std::format("asyncio.sleep() expects 1 argument, got {}", a.size()));
            } else if (!is_unknown(a[0]) && !is_num(a[0])) {
                err(c.line, c.col,
                    std::format("asyncio.sleep() delay must be a number, got '{}'",
                                type_name(a[0])));
            }
            return mk_coroutine(t_none());
        }

        if (member == "Future") {
            if (!a.empty() || !c.keywords.empty())
                err(c.line, c.col, "asyncio.Future() takes no arguments");
            return mk_future(t_unknown());
        }

        if (member == "Event") {
            if (!a.empty()) err(c.line, c.col, "asyncio.Event() takes no arguments");
            return mk_async_event();
        }
        if (member == "Lock") {
            if (!a.empty()) err(c.line, c.col, "asyncio.Lock() takes no arguments");
            return mk_async_lock();
        }
        if (member == "Queue") {
            if (a.size() > 1)
                err(c.line, c.col, "asyncio.Queue() takes at most one argument");
            if (a.size() == 1 && !is_unknown(a[0]) && a[0]->kind != TyKind::Int)
                err(c.line, c.col,
                    std::format("asyncio.Queue() maxsize must be an int, got '{}'",
                                type_name(a[0])));
            return mk_async_queue(t_unknown());
        }

        if (member == "Semaphore") {
            if (a.size() > 1)
                err(c.line, c.col, "asyncio.Semaphore() takes at most one argument");
            if (a.size() == 1 && !is_unknown(a[0]) && a[0]->kind != TyKind::Int)
                err(c.line, c.col,
                    std::format("asyncio.Semaphore() value must be an int, got '{}'",
                                type_name(a[0])));
            return mk_async_semaphore();
        }

        if (member == "TaskGroup") {
            if (!a.empty()) err(c.line, c.col, "asyncio.TaskGroup() takes no arguments");
            return mk_async_taskgroup();
        }

        if (member == "timeout") {
            if (a.size() != 1) {
                err(c.line, c.col,
                    std::format("asyncio.timeout() expects 1 argument, got {}", a.size()));
            } else if (!is_unknown(a[0]) && !is_num(a[0])) {
                err(c.line, c.col,
                    std::format("asyncio.timeout() delay must be a number, got '{}'",
                                type_name(a[0])));
            }
            return mk_async_timeout();
        }

        if (member == "wait_for") {
            if (a.size() != 2) {
                err(c.line, c.col,
                    std::format("asyncio.wait_for() expects 2 arguments, got {}", a.size()));
                return t_unknown();
            }
            if (!is_unknown(a[1]) && !is_num(a[1]))
                err(c.line, c.col,
                    std::format("asyncio.wait_for() timeout must be a number, got '{}'",
                                type_name(a[1])));
            TypePtr res = t_unknown();
            if (a[0] && (a[0]->kind == TyKind::Coroutine || a[0]->kind == TyKind::Task ||
                         a[0]->kind == TyKind::Future) &&
                !a[0]->sub.empty())
                res = a[0]->sub[0];
            return mk_coroutine(res);
        }

        if (member == "wait_readable" || member == "wait_writable") {
            if (a.size() != 1) {
                err(c.line, c.col,
                    std::format("asyncio.{}() expects 1 argument, got {}", member, a.size()));
            } else if (!is_unknown(a[0]) && a[0]->kind != TyKind::Int) {
                err(c.line, c.col,
                    std::format("asyncio.{}() expects an int fd, got '{}'", member,
                                type_name(a[0])));
            }
            return mk_coroutine(t_none());
        }

        if (member == "create_task") {
            if (a.size() != 1) {
                err(c.line, c.col,
                    std::format("asyncio.create_task() expects 1 argument, got {}", a.size()));
                return mk_task(t_unknown());
            }
            if (!a[0] || a[0]->kind == TyKind::Unknown) return mk_task(t_unknown());
            if (a[0]->kind == TyKind::Coroutine)
                return mk_task(a[0]->sub.empty() ? t_unknown() : a[0]->sub[0]);
            err(c.line, c.col,
                std::format("asyncio.create_task() expects a coroutine, got '{}'",
                            type_name(a[0])));
            return mk_task(t_unknown());
        }

        // gather(*awaitables) -> Coroutine[list[公共结果类型]]
        TypePtr elem = nullptr;
        for (size_t i = 0; i < a.size(); ++i) {
            TypePtr it = a[i];
            TypePtr r = t_unknown();
            if (it && (it->kind == TyKind::Coroutine || it->kind == TyKind::Task ||
                       it->kind == TyKind::Future))
                r = it->sub.empty() ? t_unknown() : it->sub[0];
            else if (!is_unknown(it)) {
                err(c.args[i]->line, c.args[i]->col,
                    std::format("asyncio.gather() argument is not awaitable: '{}'",
                                type_name(it)));
            }
            elem = elem ? join(elem, r) : r;
        }
        return mk_coroutine(mk_list(elem ? elem : t_unknown()));
    }


std::optional<TypePtr> TypeChecker::random_call(const std::string& member, Call& c) {
        if (member == "sample" || member == "choices") {
            std::vector<TypePtr> a;
            for (auto& arg : c.args) a.push_back(expr(*arg));
            TypePtr kt = nullptr;
            for (auto& kw : c.keywords) {
                if (kw.arg == "k") {
                    kt = expr(*kw.value);
                } else {
                    err(kw.value->line, kw.value->col,
                        std::format("random.{}() got an unexpected keyword argument '{}'",
                                    member, kw.arg));
                }
            }
            // sample 用位置参数 (seq, k)；choices 跟 CPython 一样用 k= 关键字。
            size_t want = member == "sample" ? 2 : 1;
            if (a.size() != want) {
                err(c.line, c.col,
                    std::format("random.{}() takes {} positional argument{}, got {}", member, want,
                                want == 1 ? "" : "s", a.size()));
                return mk_list(t_unknown());
            }
            TypePtr kty = member == "sample" ? (a.size() > 1 ? a[1] : nullptr) : kt;
            if (kty && !is_unknown(kty) && kty->kind != TyKind::Int)
                err(c.line, c.col, std::format("random.{}() size must be an int", member));
            TypePtr et = t_unknown();
            TypePtr st = a[0];
            if (st && st->kind == TyKind::List) {
                if (!st->sub.empty()) et = st->sub[0];
            } else if (st && st->kind == TyKind::Tuple) {
                if (!st->sub.empty()) {
                    et = st->sub[0];
                    for (size_t i = 1; i < st->sub.size(); ++i) et = join(et, st->sub[i]);
                }
            } else if (st && st->kind == TyKind::Str) {
                et = t_str();
            } else if (st && st->kind != TyKind::Unknown) {
                err(c.args[0]->line, c.args[0]->col,
                    std::format("random.{}() expects a sequence, got '{}'", member,
                                type_name(st)));
            }
            return mk_list(et);
        }
        if (member != "choice" && member != "shuffle") return std::nullopt;
        if (!c.keywords.empty()) {
            err(c.line, c.col,
                std::format("random.{}() does not take keyword arguments", member));
            for (auto& kw : c.keywords) expr(*kw.value);
        }
        if (c.args.size() != 1) {
            for (auto& arg : c.args) expr(*arg);
            err(c.line, c.col,
                std::format("random.{}() expects 1 argument, got {}", member, c.args.size()));
            return t_unknown();
        }
        TypePtr st = expr(*c.args[0]);
        if (member == "shuffle") {
            if (st && st->kind != TyKind::List && st->kind != TyKind::Unknown)
                err(c.args[0]->line, c.args[0]->col,
                    std::format("random.shuffle() expects a list, got '{}'", type_name(st)));
            return t_none();
        }
        if (st && st->kind == TyKind::List)
            return st->sub.empty() ? t_unknown() : st->sub[0];
        if (st && st->kind == TyKind::Tuple) {
            if (st->sub.empty()) return t_unknown();
            TypePtr acc = st->sub[0];
            for (size_t i = 1; i < st->sub.size(); ++i) acc = join(acc, st->sub[i]);
            return acc;
        }
        if (st && st->kind == TyKind::Str) return t_str();
        if (st && st->kind != TyKind::Unknown)
            err(c.args[0]->line, c.args[0]->col,
                std::format("random.choice() expects a sequence, got '{}'", type_name(st)));
        return t_unknown();
}

std::optional<TypePtr> TypeChecker::itertools_call(const std::string& member, Call& c) {
        if (member != "chain" && member != "islice" && member != "repeat" &&
            member != "product" && member != "permutations" && member != "combinations" &&
            member != "combinations_with_replacement" && member != "zip_longest" &&
            member != "accumulate")
            return std::nullopt;
        if (!c.keywords.empty()) {
            err(c.line, c.col,
                std::format("itertools.{}() does not take keyword arguments", member));
            for (auto& kw : c.keywords) expr(*kw.value);
        }
        std::vector<TypePtr> a;
        for (auto& arg : c.args) a.push_back(expr(*arg));
        auto is_list = [](const TypePtr& t) { return t && t->kind == TyKind::List; };
        auto elem_of = [](const TypePtr& t) {
            return t && t->kind == TyKind::List && !t->sub.empty() ? t->sub[0] : t_unknown();
        };
        if (member == "chain") {
            if (a.size() != 2) {
                err(c.line, c.col,
                    std::format("itertools.chain() expects 2 arguments, got {}", a.size()));
                return mk_list(t_unknown());
            }
            for (size_t i = 0; i < 2; ++i)
                if (!is_list(a[i]) && !is_unknown(a[i]))
                    err(c.args[i]->line, c.args[i]->col,
                        std::format("itertools.chain() expects lists, got '{}'",
                                    type_name(a[i])));
            TypePtr ea = elem_of(a[0]);
            TypePtr eb = elem_of(a[1]);
            return mk_list(join(ea, eb));
        }
        if (member == "islice") {
            if (a.size() != 2 && a.size() != 3) {
                err(c.line, c.col,
                    std::format("itertools.islice() expects 2 or 3 arguments, got {}", a.size()));
                return mk_list(t_unknown());
            }
            if (!is_list(a[0]) && !is_unknown(a[0]))
                err(c.args[0]->line, c.args[0]->col,
                    std::format("itertools.islice() expects a list, got '{}'",
                                type_name(a[0])));
            for (size_t i = 1; i < a.size(); ++i)
                if (a[i] && !is_unknown(a[i]) && a[i]->kind != TyKind::Int)
                    err(c.args[i]->line, c.args[i]->col,
                        "itertools.islice() indices must be int");
            return is_list(a[0]) ? a[0] : mk_list(t_unknown());
        }
        if (member == "product") {
            // product() 无实参时 CPython 返回 [()]，这里照做。
            std::vector<TypePtr> elems;
            elems.reserve(a.size());
            for (size_t i = 0; i < a.size(); ++i) {
                if (!is_list(a[i]) && !is_unknown(a[i]))
                    err(c.args[i]->line, c.args[i]->col,
                        std::format("itertools.product() expects lists, got '{}'",
                                    type_name(a[i])));
                elems.push_back(elem_of(a[i]));
            }
            return mk_list(mk_tuple(std::move(elems)));
        }
        if (member == "permutations" || member == "combinations" ||
            member == "combinations_with_replacement") {
            bool argc_ok = member == "permutations" ? (a.size() == 1 || a.size() == 2)
                                                    : a.size() == 2;
            if (!argc_ok) {
                err(c.line, c.col,
                    std::format("itertools.{}() expects {} arguments, got {}", member,
                                member == "permutations" ? "1 or 2" : "2", a.size()));
                return mk_list(mk_tuple_rest(t_unknown()));
            }
            if (!is_list(a[0]) && !is_unknown(a[0]))
                err(c.args[0]->line, c.args[0]->col,
                    std::format("itertools.{}() expects a list, got '{}'", member,
                                type_name(a[0])));
            if (a.size() > 1 && a[1] && !is_unknown(a[1]) && a[1]->kind != TyKind::Int)
                err(c.args[1]->line, c.args[1]->col,
                    std::format("itertools.{}() r must be int", member));
            return mk_list(mk_tuple_rest(elem_of(a[0])));
        }
        if (member == "zip_longest") {
            if (a.empty()) {
                err(c.line, c.col, "itertools.zip_longest() expects at least 1 argument");
                return mk_list(mk_tuple_rest(t_unknown()));
            }
            // 短的序列用 None 补齐：每一列的类型是 Optional[elem]。
            std::vector<TypePtr> cols;
            cols.reserve(a.size());
            for (size_t i = 0; i < a.size(); ++i) {
                if (!is_list(a[i]) && !is_unknown(a[i]))
                    err(c.args[i]->line, c.args[i]->col,
                        std::format("itertools.zip_longest() expects lists, got '{}'",
                                    type_name(a[i])));
                cols.push_back(mk_optional(elem_of(a[i])));
            }
            return mk_list(mk_tuple(std::move(cols)));
        }
        if (member == "accumulate") {
            if (a.size() != 1) {
                err(c.line, c.col,
                    std::format("itertools.accumulate() expects 1 argument, got {}", a.size()));
                return mk_list(t_unknown());
            }
            if (!is_list(a[0]) && !is_unknown(a[0]))
                err(c.args[0]->line, c.args[0]->col,
                    std::format("itertools.accumulate() expects a list, got '{}'",
                                type_name(a[0])));
            TypePtr e = elem_of(a[0]);
            if (e && e->kind != TyKind::Int && e->kind != TyKind::Float &&
                e->kind != TyKind::Bool && !is_unknown(e)) {
                err(c.args[0]->line, c.args[0]->col,
                    "itertools.accumulate() expects list[int] or list[float]");
                return mk_list(t_unknown());
            }
            return a[0];
        }
        // repeat(x, n)
        if (a.size() != 2) {
            err(c.line, c.col,
                std::format("itertools.repeat() expects 2 arguments, got {}", a.size()));
            return mk_list(t_unknown());
        }
        if (a[1] && !is_unknown(a[1]) && a[1]->kind != TyKind::Int)
            err(c.args[1]->line, c.args[1]->col, "itertools.repeat() count must be int");
        return mk_list(a[0] ? a[0] : t_unknown());
}

std::optional<TypePtr> TypeChecker::operator_call(const std::string& member, Call& c) {
        static const std::unordered_map<std::string, Tok> kBin = {
            {"add", Tok::PLUS},        {"sub", Tok::MINUS},
            {"mul", Tok::STAR},        {"truediv", Tok::SLASH},
            {"floordiv", Tok::DOUBLESLASH}, {"mod", Tok::PERCENT},
            {"pow", Tok::DOUBLESTAR},  {"and_", Tok::AMPER},
            {"or_", Tok::VBAR},        {"xor", Tok::CIRCUMFLEX},
            {"lshift", Tok::LSHIFT},   {"rshift", Tok::RSHIFT},
            {"concat", Tok::PLUS},
        };
        static const std::unordered_map<std::string, CmpOp> kCmp = {
            {"eq", CmpOp::Eq}, {"ne", CmpOp::Ne}, {"lt", CmpOp::Lt},
            {"le", CmpOp::Le}, {"gt", CmpOp::Gt}, {"ge", CmpOp::Ge},
        };
        auto known = kBin.count(member) || kCmp.count(member) ||
                     member == "neg" || member == "pos" || member == "abs" ||
                     member == "invert" || member == "truth" || member == "not_" ||
                     member == "is_" || member == "is_not" || member == "contains" ||
                     member == "getitem" || member == "itemgetter" ||
                     member == "attrgetter";
        if (!known) return std::nullopt;
        if (!c.keywords.empty()) {
            err(c.line, c.col,
                std::format("operator.{}() does not take keyword arguments", member));
            for (auto& kw : c.keywords) expr(*kw.value);
        }
        std::vector<TypePtr> a;
        for (auto& arg : c.args) a.push_back(expr(*arg));
        auto want_args = [&](size_t n) {
            if (a.size() == n) return true;
            err(c.line, c.col,
                std::format("operator.{}() expects {} argument{}, got {}", member, n,
                            n == 1 ? "" : "s", a.size()));
            return false;
        };
        if (member == "itemgetter") {
            if (a.empty()) {
                err(c.line, c.col, "operator.itemgetter() expects at least 1 argument");
            }
            for (size_t i = 0; i < a.size(); ++i)
                if (a[i] && !is_unknown(a[i]) && a[i]->kind != TyKind::Int)
                    err(c.args[i]->line, c.args[i]->col,
                        "operator.itemgetter() only supports int keys");
            // 返回一个合成的 getter：单键返回 unknown；多键返回变长元组，
            // 这样调用点至少能按元组打印/解包。
            auto f = std::make_shared<FuncInfo>();
            f->name = "operator.itemgetter.<getter>";
            Param p;
            p.name = "obj";
            p.ty = t_unknown();
            f->pos.push_back(std::move(p));
            f->ret = a.size() > 1 ? mk_tuple_rest(t_unknown()) : t_unknown();
            f->ret_known = true;
            auto ft = new_ty(TyKind::Func);
            ft->fn = std::move(f);
            return ft;
        }
        if (member == "attrgetter") {
            err(c.line, c.col, "operator.attrgetter() is not supported yet");
            return t_unknown();
        }
        if (auto it = kBin.find(member); it != kBin.end()) {
            if (!want_args(2)) return t_unknown();
            return binop_type(it->second, a[0], a[1], c.line, c.col);
        }
        if (auto it = kCmp.find(member); it != kCmp.end()) {
            if (!want_args(2)) return t_bool();
            compare_type(it->second, a[0], a[1], c.line, c.col);
            return t_bool();
        }
        if (member == "is_" || member == "is_not") {
            want_args(2);
            return t_bool();
        }
        if (member == "truth" || member == "not_") {
            want_args(1);
            return t_bool();
        }
        if (member == "neg" || member == "pos" || member == "abs" ||
            member == "invert") {
            if (!want_args(1)) return t_unknown();
            TypePtr t = a[0];
            // 用户类：和一元运算符一样走 __neg__/__pos__/__abs__/__invert__。
            if (t && t->kind == TyKind::Class) {
                const char* dn = member == "neg"    ? "__neg__"
                                 : member == "pos"  ? "__pos__"
                                 : member == "abs"  ? "__abs__"
                                                    : "__invert__";
                if (TypePtr r = dunder_unary_ret(dn, t)) return r;
                if (t->cls && t->cls->is_open()) return t_unknown();
                err(c.args[0]->line, c.args[0]->col,
                    std::format("bad operand type for operator.{}(): '{}'", member,
                                type_name(t)));
                return t_unknown();
            }
            if (member == "invert") {
                if (t && !is_unknown(t) && t->kind != TyKind::Int &&
                    t->kind != TyKind::Bool)
                    err(c.args[0]->line, c.args[0]->col,
                        std::format("bad operand type for operator.invert(): '{}'",
                                    type_name(t)));
                return t_int();
            }
            if (is_unknown(t) || is_num(t) || (t && t->kind == TyKind::Complex) ||
                (member != "abs" && t && t->kind == TyKind::TimeDelta))
                return t;
            err(c.args[0]->line, c.args[0]->col,
                std::format("bad operand type for operator.{}(): '{}'", member,
                            type_name(t)));
            return t_unknown();
        }
        if (member == "contains") {
            if (!want_args(2)) return t_bool();
            TypePtr cont = a[0];
            if (cont && !is_unknown(cont) && cont->kind != TyKind::List &&
                cont->kind != TyKind::Tuple && cont->kind != TyKind::Dict &&
                cont->kind != TyKind::Set && cont->kind != TyKind::Str &&
                cont->kind != TyKind::Bytes && cont->kind != TyKind::Range)
                err(c.args[0]->line, c.args[0]->col,
                    std::format("operator.contains() expects a container, got '{}'",
                                type_name(cont)));
            return t_bool();
        }
        // getitem
        if (!want_args(2)) return t_unknown();
        return subscript_type(a[0], a[1], c.line, c.col);
}

std::optional<TypePtr> TypeChecker::collections_call(const std::string& member, Call& c) {
        if (member == "defaultdict") {
            if (!c.keywords.empty()) {
                err(c.line, c.col, "collections.defaultdict() does not take keyword arguments");
                for (auto& kw : c.keywords) expr(*kw.value);
            }
            if (c.args.size() > 2) {
                err(c.line, c.col,
                    std::format("collections.defaultdict() expects at most 2 arguments, got {}",
                                c.args.size()));
                for (auto& arg : c.args) expr(*arg);
                return mk_dict(t_unknown(), t_unknown());
            }
            TypePtr vt = t_unknown();
            if (!c.args.empty()) {
                auto* n = dynamic_cast<Name*>(c.args[0].get());
                auto* none = dynamic_cast<Constant*>(c.args[0].get());
                // 只识别未被遮蔽的内置零参数工厂；其它 callable 暂不支持，
                // 因为缺省读取发生在运行时，任意闭包还需要一套调用协议。
                if (n && !lookup(n->id)) {
                    if (n->id == "int") vt = t_int();
                    else if (n->id == "float") vt = t_float();
                    else if (n->id == "bool") vt = t_bool();
                    else if (n->id == "str") vt = t_str();
                    else if (n->id == "list") vt = mk_list(t_unknown());
                    else if (n->id == "dict") vt = mk_dict(t_unknown(), t_unknown());
                    else if (n->id == "set") vt = mk_set(t_unknown());
                    else if (n->id == "tuple") vt = mk_tuple_rest(t_unknown());
                    else {
                        expr(*c.args[0]);
                        err(c.args[0]->line, c.args[0]->col,
                            std::format("collections.defaultdict() factory '{}' is not supported "
                                        "yet; use int/float/bool/str/list/dict/set/tuple",
                                        n->id));
                    }
                } else if (none && none->ck == Constant::CK::None) {
                    // defaultdict(None) 和普通 dict 一样，缺省 KeyError。
                    expr(*c.args[0]);
                } else {
                    expr(*c.args[0]);
                    err(c.args[0]->line, c.args[0]->col,
                        "collections.defaultdict() currently supports built-in factories only");
                }
            }
            TypePtr kt = t_unknown();
            if (c.args.size() == 2) {
                TypePtr init = expr(*c.args[1]);
                if (init && init->kind == TyKind::Dict && init->sub.size() >= 2) {
                    kt = init->sub[0];
                    vt = init->sub[1];
                } else if (!is_unknown(init)) {
                    err(c.args[1]->line, c.args[1]->col,
                        std::format("collections.defaultdict() initial value must be a dict, got "
                                    "'{}'",
                                    type_name(init)));
                }
            }
            return mk_dict(kt, vt);
        }
        if (member == "deque") {
            if (c.args.size() > 2) {
                err(c.line, c.col,
                    std::format("collections.deque() expects at most 2 arguments, got {}",
                                c.args.size()));
                for (auto& arg : c.args) expr(*arg);
                for (auto& kw : c.keywords) expr(*kw.value);
                return mk_deque(t_unknown());
            }
            TypePtr et = t_unknown();
            std::vector<TypePtr> a;
            for (auto& arg : c.args) a.push_back(expr(*arg));
            if (!a.empty()) {
                if (a[0] && a[0]->kind == TyKind::List && !a[0]->sub.empty())
                    et = a[0]->sub[0];
                else if (a[0] && !is_unknown(a[0]))
                    err(c.args[0]->line, c.args[0]->col,
                        std::format("collections.deque() expects a list, got '{}'",
                                    type_name(a[0])));
            }
            for (auto& kw : c.keywords) {
                if (kw.arg == "maxlen") {
                    expr(*kw.value);
                } else {
                    err(kw.value->line, kw.value->col,
                        std::format("collections.deque() got an unexpected keyword argument "
                                    "'{}'",
                                    kw.arg));
                }
            }
            return mk_deque(et);
        }
        if (member != "Counter") return std::nullopt;
        if (!c.keywords.empty()) {
            err(c.line, c.col, "collections.Counter() does not take keyword arguments");
            for (auto& kw : c.keywords) expr(*kw.value);
        }
        std::vector<TypePtr> a;
        for (auto& arg : c.args) a.push_back(expr(*arg));
        if (a.size() != 1) {
            err(c.line, c.col,
                std::format("collections.Counter() expects 1 argument, got {}", a.size()));
            return mk_dict(t_unknown(), t_int());
        }
        TypePtr et = t_unknown();
        if (a[0] && a[0]->kind == TyKind::List && !a[0]->sub.empty()) et = a[0]->sub[0];
        else if (a[0] && !is_unknown(a[0]))
            err(c.args[0]->line, c.args[0]->col,
                std::format("collections.Counter() expects a list, got '{}'",
                            type_name(a[0])));
        return mk_dict(et, t_int());
}

std::optional<TypePtr> TypeChecker::functools_call(const std::string& member, Call& c) {
        if (member == "reduce") {
            if (!c.keywords.empty()) {
                err(c.line, c.col, "functools.reduce() does not take keyword arguments");
                for (auto& kw : c.keywords) expr(*kw.value);
            }
            std::vector<TypePtr> a;
            for (auto& arg : c.args) a.push_back(expr(*arg));
            if (a.size() < 2 || a.size() > 3) {
                err(c.line, c.col,
                    std::format("functools.reduce() expects 2 or 3 arguments, got {}",
                                a.size()));
                return t_unknown();
            }
            if (a[0] && !is_unknown(a[0]) && a[0]->kind != TyKind::Func &&
                a[0]->kind != TyKind::BoundMethod) {
                err(c.args[0]->line, c.args[0]->col,
                    std::format("functools.reduce() expects a function, got '{}'",
                                type_name(a[0])));
            }
            TypePtr elem = iter_elem(a[1], c.args[1]->line, c.args[1]->col);
            // reduce 不直接调用 f，形参类型只能在这里回填：否则 lambda /
            // 具名函数的形参一直是 unknown，float 元素会按整数寄存器传参。
            // 同一个函数被不同类型的 reduce 用过就退回 unknown（和普通
            // 调用点的单态化规则一致），否则会按错误的值域编译。
            if (a[0] && a[0]->kind == TyKind::Func && a[0]->fn && elem && !is_unknown(elem)) {
                FuncInfoPtr fi = a[0]->fn;
                bool dirty = false;
                for (size_t i = 0; i < fi->pos.size() && i < 2; ++i) {
                    Param& p = fi->pos[i];
                    if (p.multi) continue;
                    if (is_unknown(p.ty)) {
                        p.ty = elem;
                        p.inferred = true;
                        dirty = true;
                    } else if (!assignable(p.ty, elem) && p.inferred) {
                        p.ty = t_unknown();
                        p.inferred = false;
                        p.multi = true;
                        dirty = true;
                    }
                }
                if (dirty) fi->sig_dirty = true;
            }
            // 回调的形参域决定 reduce 用哪套寄存器传参：只要回调的形参里
            // 有一个是 float（且没有具体非 float 形参），折叠就必须走 double
            // 版——float 版把列表当 double 数组读、按 xmm 传参。否则浮点的
            // 位模式会被当整数塞进整型实参寄存器，静默算错。
            // 两个形参必须都是 float，回调的入口才恰好是 xmm0/xmm1；
            // (float, unknown) 这种混合 ABI 是 xmm0+rdi，两套跳板都对不上。
            bool cb_fp = false;
            bool cb_mixed = false;
            if (a[0] && a[0]->kind == TyKind::Func && a[0]->fn && a[0]->fn->pos.size() >= 2) {
                const TypePtr& p0 = a[0]->fn->pos[0].ty;
                const TypePtr& p1 = a[0]->fn->pos[1].ty;
                bool f0 = p0 && p0->kind == TyKind::Float;
                bool f1 = p1 && p1->kind == TyKind::Float;
                cb_fp = f0 && f1;
                cb_mixed = f0 != f1;
            }
            if (cb_mixed) {
                err(c.args[0]->line, c.args[0]->col,
                    "reduce() callback must take two float parameters or none");
            }
            // 元素域是 float、未知或整型（int 会被提升成 double），且回调要
            // double 时，结果域是 float。
            TypePtr base = elem;
            if (cb_fp && (!elem || elem->kind == TyKind::Unknown ||
                          elem->kind == TyKind::Int || elem->kind == TyKind::Bool))
                base = t_float();
            // 有初始值时结果类型是元素类型与初始值的公共上界。
            if (a.size() == 3) return join(base, a[2]);
            return base;
        }
        if (member == "wraps") {
            if (!c.keywords.empty()) {
                err(c.line, c.col, "functools.wraps() does not take keyword arguments");
                for (auto& kw : c.keywords) expr(*kw.value);
            }
            std::vector<TypePtr> a;
            for (auto& arg : c.args) a.push_back(expr(*arg));
            if (a.size() != 1) {
                err(c.line, c.col,
                    std::format("functools.wraps() expects 1 argument, got {}", a.size()));
            }
            // 只当装饰器用：返回 unknown，装饰器那一步会因为「看不出来」
            // 保留被装饰函数原有的签名——正是 wraps 的语义。
            return t_unknown();
        }
        if (member == "partial") {
            std::vector<TypePtr> a;
            for (auto& arg : c.args) {
                if (arg->kind == ExprKind::Starred) {
                    err(arg->line, arg->col,
                        "functools.partial() does not support * expansion");
                    expr(*static_cast<Starred&>(*arg).value);
                    a.push_back(t_unknown());
                    continue;
                }
                a.push_back(expr(*arg));
            }
            std::vector<TypePtr> kwtys;
            for (auto& kw : c.keywords) kwtys.push_back(expr(*kw.value));
            if (a.empty()) {
                err(c.line, c.col, "functools.partial() expects at least 1 argument");
                return t_unknown();
            }
            TypePtr ft = a[0];
            if (is_unknown(ft)) return t_unknown();
            if (!ft || ft->kind != TyKind::Func || !ft->fn) {
                err(c.args[0]->line, c.args[0]->col,
                    std::format("functools.partial() expects a function, got '{}'",
                                type_name(ft)));
                return t_unknown();
            }
            FuncInfoPtr fn = ft->fn;
            if (fn->varargs || fn->kwargs) {
                err(c.args[0]->line, c.args[0]->col,
                    "functools.partial() does not support functions with *args/**kwargs");
                return t_unknown();
            }
            int npos = static_cast<int>(fn->pos.size());
            int nkw = static_cast<int>(fn->kwonly.size());
            std::vector<bool> bound(static_cast<size_t>(npos + nkw), false);
            std::vector<int> bound_params;
            auto param_ty = [&](int idx) -> TypePtr {
                if (idx < npos) return fn->pos[static_cast<size_t>(idx)].ty;
                return fn->kwonly[static_cast<size_t>(idx - npos)].ty;
            };
            auto param_name = [&](int idx) -> const std::string& {
                if (idx < npos) return fn->pos[static_cast<size_t>(idx)].name;
                return fn->kwonly[static_cast<size_t>(idx - npos)].name;
            };
            auto bind_arg = [&](int idx, const TypePtr& at, const Expr& ae) {
                if (bound[static_cast<size_t>(idx)]) {
                    err(ae.line, ae.col,
                        std::format("functools.partial() got multiple values for argument '{}'",
                                    param_name(idx)));
                    return;
                }
                bound[static_cast<size_t>(idx)] = true;
                bound_params.push_back(idx);
                TypePtr pt = param_ty(idx);
                if (!is_unknown(pt) && !is_unknown(at) && !assignable(pt, at)) {
                    err(ae.line, ae.col,
                        std::format("functools.partial() argument '{}' expects '{}', got '{}'",
                                    param_name(idx), type_name(pt), type_name(at)));
                }
            };
            size_t extra_pos = a.size() - 1;
            if (extra_pos > static_cast<size_t>(npos)) {
                err(c.args[static_cast<size_t>(npos) + 1]->line,
                    c.args[static_cast<size_t>(npos) + 1]->col,
                    std::format("functools.partial() takes at most {} positional arguments",
                                npos));
                extra_pos = static_cast<size_t>(npos);
            }
            for (size_t i = 0; i < extra_pos; ++i)
                bind_arg(static_cast<int>(i), a[i + 1], *c.args[i + 1]);
            for (size_t k = 0; k < c.keywords.size(); ++k) {
                const std::string& name = c.keywords[k].arg;
                int idx = -1;
                for (int i = 0; i < npos; ++i)
                    if (fn->pos[static_cast<size_t>(i)].name == name) { idx = i; break; }
                if (idx < 0)
                    for (int i = 0; i < nkw; ++i)
                        if (fn->kwonly[static_cast<size_t>(i)].name == name) {
                            idx = npos + i;
                            break;
                        }
                if (idx < 0) {
                    err(c.keywords[k].value->line, c.keywords[k].value->col,
                        std::format("functools.partial() got an unexpected keyword argument '{}'",
                                    name));
                    continue;
                }
                if (idx < static_cast<int>(fn->n_posonly)) {
                    err(c.keywords[k].value->line, c.keywords[k].value->col,
                        std::format("functools.partial() argument '{}' is positional-only", name));
                    continue;
                }
                bind_arg(idx, kwtys[k], *c.keywords[k].value);
            }
            auto sig = std::make_shared<FuncInfo>();
            sig->name = "partial";
            sig->ret = fn->ret;
            sig->ret_known = fn->ret_known;
            auto plan = std::make_shared<PartialPlan>();
            plan->bound_params = std::move(bound_params);
            int remaining_posonly = 0;
            for (int i = 0; i < npos; ++i) {
                if (bound[static_cast<size_t>(i)]) continue;
                sig->pos.push_back(fn->pos[static_cast<size_t>(i)]);
                plan->remaining.push_back(i);
                if (i < static_cast<int>(fn->n_posonly)) ++remaining_posonly;
            }
            for (int i = 0; i < nkw; ++i) {
                if (bound[static_cast<size_t>(npos + i)]) continue;
                sig->kwonly.push_back(fn->kwonly[static_cast<size_t>(i)]);
                plan->remaining.push_back(npos + i);
            }
            sig->n_posonly = remaining_posonly;
            plan->sig = sig;
            partial_plans_[&c] = plan;
            TypePtr out = new_ty(TyKind::Func);
            out->fn = sig;
            return out;
        }
        if (member == "lru_cache") {
            // 装饰器参数：maxsize / typed。参数只求值；缓存策略在降级期决定。
            if (c.args.size() > 1)
                err(c.line, c.col,
                    "functools.lru_cache() takes at most 1 positional argument");
            for (auto& arg : c.args) expr(*arg);
            for (auto& kw : c.keywords) {
                expr(*kw.value);
                if (kw.arg != "maxsize" && kw.arg != "typed")
                    err(kw.value->line, kw.value->col,
                        std::format("functools.lru_cache() got an unexpected keyword argument '{}'",
                                    kw.arg));
            }
            return t_unknown();
        }
        return std::nullopt;
}

// @functools.lru_cache / @lru_cache(...) 在类型上是恒等装饰器：
// 包装函数的签名与被装饰函数相同，调用点仍按原签名检查实参。
bool TypeChecker::is_lru_cache_expr(const Expr* e) {
    if (auto* call = dynamic_cast<const Call*>(e)) e = call->func.get();
    if (auto* at = dynamic_cast<const Attribute*>(e)) {
        auto* rn = dynamic_cast<const Name*>(at->value.get());
        if (!rn || at->attr != "lru_cache") return false;
        auto mod = lookup_module_alias(rn->id);
        return mod && mod->builtin && mod->name == "functools";
    }
    if (auto* n = dynamic_cast<const Name*>(e)) {
        auto* sym = lookup(n->id);
        return sym && sym->ty && sym->ty->kind == TyKind::Func && sym->ty->fn &&
               sym->ty->fn->name == "functools.lru_cache";
    }
    return false;
}

std::optional<TypePtr> TypeChecker::json_call(const std::string& member, Call& c) {
        // 动态 JSON（0.447）的成员集合。这些名字在 builtin_sigs 里也登记了
        // 签名（from-import / 属性查找要用），但**调用检查必须在这里截住**：
        // 走通用签名路径会把 unknown 形参当用户函数做调用点类型推断，
        // json.stringify 同时收 float/bool/None 就会报「used with both
        // float and non-float values」。这里直接给返回类型、只做必要校验。
        static const std::unordered_map<std::string, std::pair<size_t, int>> kDyn = {
            // name → {参数个数, 返回类型编码}
            // 0 unknown / 1 str / 2 int / 3 float / 4 bool / 5 list[str] /
            // 6 dict[str,unknown] / 7 list[unknown]
            {"parse", {1, 0}},
            {"stringify", {1, 1}},
            {"get", {2, 0}},
            {"get_or", {3, 0}},
            {"has", {2, 4}},
            {"path", {2, 0}},
            {"len", {1, 2}},
            {"keys", {1, 5}},
            {"type", {1, 1}},
            {"is_null", {1, 4}},
            {"as_str", {1, 1}},
            {"as_int", {1, 2}},
            {"as_float", {1, 3}},
            {"as_bool", {1, 4}},
            {"as_dict", {1, 6}},
            {"as_list", {1, 7}},
            {"null", {0, 0}},
            {"bool", {1, 0}},
        };
        auto dyn_it = kDyn.find(member);
        if (member != "dumps" && member != "loads" && dyn_it == kDyn.end())
            return std::nullopt;
        // dumps 收 indent= / sort_keys= / ensure_ascii= / separators= /
        // allow_nan= 关键字，其它不收。
        for (auto& kw : c.keywords) {
            if (member == "dumps" &&
                (kw.arg == "indent" || kw.arg == "sort_keys" ||
                 kw.arg == "ensure_ascii" || kw.arg == "separators" ||
                 kw.arg == "allow_nan")) {
                expr(*kw.value);
                continue;
            }
            err(c.line, c.col,
                std::format("json.{}() does not take keyword arguments", member));
            for (auto& k2 : c.keywords) expr(*k2.value);
            break;
        }
        std::vector<TypePtr> a;
        for (auto& arg : c.args) a.push_back(expr(*arg));
        if (member == "dumps") {
            if (a.size() != 1) {
                err(c.line, c.col,
                    std::format("json.dumps() expects 1 argument, got {}", a.size()));
            }
            return t_str();
        }
        if (member == "loads") {
            // loads(text, sample)：样例值只提供静态类型，运行时不求值。
            if (a.size() != 2) {
                err(c.line, c.col,
                    std::format("json.loads() expects 2 arguments (text, sample), got {}",
                                a.size()));
                return t_unknown();
            }
            if (a[0] && !is_unknown(a[0]) && a[0]->kind != TyKind::Str)
                err(c.args[0]->line, c.args[0]->col,
                    std::format("json.loads() expects a str, got '{}'", type_name(a[0])));
            TypePtr sample = a[1];
            if (!sample || sample->kind == TyKind::Unknown) {
                err(c.args[1]->line, c.args[1]->col,
                    "json.loads() needs a sample value with a static type");
                return t_unknown();
            }
            return sample;
        }
        // ── 动态 JSON 成员 ─────────────────────────────────────────
        const auto& [nargs, rc] = dyn_it->second;
        if (a.size() != nargs) {
            err(c.line, c.col,
                std::format("json.{}() expects {} argument{}, got {}", member, nargs,
                            nargs == 1 ? "" : "s", a.size()));
            return t_unknown();
        }
        // 少量静态校验：参数类别明显不对时早点报，运行期还能再兜一层。
        if (member == "parse" && a[0] && !is_unknown(a[0]) && a[0]->kind != TyKind::Str)
            err(c.args[0]->line, c.args[0]->col,
                std::format("json.parse() expects a str, got '{}'", type_name(a[0])));
        if ((member == "get" || member == "get_or" || member == "has") && a[1] &&
            !is_unknown(a[1]) && a[1]->kind != TyKind::Str && a[1]->kind != TyKind::Int)
            err(c.args[1]->line, c.args[1]->col,
                std::format("json.{}() key must be a str or int, got '{}'", member,
                            type_name(a[1])));
        if (member == "path" && a[1] && !is_unknown(a[1]) &&
            !(a[1]->kind == TyKind::List && !a[1]->sub.empty() && a[1]->sub[0] &&
              a[1]->sub[0]->kind == TyKind::Str))
            err(c.args[1]->line, c.args[1]->col,
                "json.path() expects a list[str]");
        if (member == "bool" && a[0] && !is_unknown(a[0]) && a[0]->kind != TyKind::Bool)
            err(c.args[0]->line, c.args[0]->col,
                std::format("json.bool() expects a bool, got '{}'", type_name(a[0])));
        switch (rc) {
            case 1: return t_str();
            case 2: return t_int();
            case 3: return t_float();
            case 4: return t_bool();
            case 5: return mk_list(t_str());
            case 6: return mk_dict(t_str(), t_unknown());
            case 7: return mk_list(t_unknown());
            default: return t_unknown();
        }
}


}  // namespace pyadamas
