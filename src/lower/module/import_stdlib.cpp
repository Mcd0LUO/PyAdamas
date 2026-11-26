// lower/import_stdlib.cpp —— operator 与 collections 的降级
#include "lower/impl.h"

namespace pyadamas {

VReg Lowerer::lower_operator(const std::string& member, Call& c) {
    if (!c.keywords.empty())
        err(c.line, c.col,
            std::format("operator.{}() does not take keyword arguments", member));
    TypePtr rt = tc_.type_of(&c);
    auto argty = [&](size_t i) { return tc_.type_of(c.args[i].get()); };
    auto eval_all = [&]() {
        for (auto& a : c.args) (void)expr(*a);
    };
    static const std::unordered_map<std::string, Tok> kBin = {
        {"add", Tok::PLUS},           {"sub", Tok::MINUS},
        {"mul", Tok::STAR},           {"truediv", Tok::SLASH},
        {"floordiv", Tok::DOUBLESLASH}, {"mod", Tok::PERCENT},
        {"pow", Tok::DOUBLESTAR},     {"and_", Tok::AMPER},
        {"or_", Tok::VBAR},           {"xor", Tok::CIRCUMFLEX},
        {"lshift", Tok::LSHIFT},      {"rshift", Tok::RSHIFT},
        {"concat", Tok::PLUS},
    };
    static const std::unordered_map<std::string, CmpOp> kCmp = {
        {"eq", CmpOp::Eq}, {"ne", CmpOp::Ne}, {"lt", CmpOp::Lt},
        {"le", CmpOp::Le}, {"gt", CmpOp::Gt}, {"ge", CmpOp::Ge},
    };

    // index(x)：整数域（int/bool 等）原样返回；其余类型在运行期抛 TypeError，
    // 消息与 CPython 对齐（`'float' object cannot be interpreted as an integer`）。
    if (member == "index") {
        if (c.args.size() != 1) {
            eval_all();
            err(c.line, c.col, "operator.index() expects 1 argument");
            return const_none();
        }
        VReg v = expr(*c.args[0]);
        TypePtr t = strip_optional(argty(0));
        auto tk = t ? t->kind : TyKind::Unknown;
        bool int_domain = tk == TyKind::Int || tk == TyKind::Bool || tk == TyKind::I8 ||
                          tk == TyKind::I16 || tk == TyKind::I32 ||
                          tk == TyKind::U8 || tk == TyKind::U16 || tk == TyKind::U32 ||
                          tk == TyKind::U64 || tk == TyKind::Big ||
                          (t && is_unknown(t));
        if (int_domain) return v;
        const char* what = tk == TyKind::Float   ? "float"
                           : tk == TyKind::Str   ? "str"
                           : tk == TyKind::Bytes ? "bytes"
                           : tk == TyKind::List  ? "list"
                           : tk == TyKind::Dict  ? "dict"
                           : tk == TyKind::Set   ? "set"
                           : tk == TyKind::Tuple ? "tuple"
                                                 : "object";
        call_void(raise_rt(RtFn::Raise),
                  {const_str("TypeError"),
                   const_str(std::format("'{}' object cannot be interpreted as an integer", what))});
        return const_int(0);
    }
    // setitem(obj, key, value) / delitem(obj, key)：静态类型能定的按类型直接
    // 发射（tuple/str/标量的 TypeError 消息才能与 CPython 逐字一致 —— tuple 与
    // list 在运行期同形，运行期分不开）；unknown 走 pa_any_setindex/delindex。
    if (member == "setitem" || member == "delitem") {
        bool is_set = member == "setitem";
        size_t want = is_set ? 3 : 2;
        if (c.args.size() != want) {
            eval_all();
            err(c.line, c.col, std::format("operator.{}() expects {} arguments", member, want));
            return const_none();
        }
        VReg o = expr(*c.args[0]);
        VReg k = expr(*c.args[1]);
        VReg v = is_set ? expr(*c.args[2]) : kNoReg;
        TypePtr t = strip_optional(argty(0));
        auto tk = t ? t->kind : TyKind::Unknown;
        const char* name = tk == TyKind::Tuple  ? "tuple"
                           : tk == TyKind::Str  ? "str"
                           : tk == TyKind::Bytes ? "str"
                           : tk == TyKind::Int  ? "int"
                           : tk == TyKind::Float ? "float"
                           : tk == TyKind::Bool ? "bool"
                           : tk == TyKind::None ? "NoneType"
                                                 : nullptr;
        if (name) {
            // CPython 的删消息不统一：tuple/str 是 "doesn't support item deletion"，
            // 标量（int/float/bool/None）走通用路径是 "does not support item deletion"。
            bool str_like = tk == TyKind::Tuple || tk == TyKind::Str || tk == TyKind::Bytes;
            std::string msg = is_set
                ? std::format("'{}' object does not support item assignment", name)
                : (str_like
                       ? std::format("'{}' object doesn't support item deletion", name)
                       : std::format("'{}' object does not support item deletion", name));
            call_void(raise_rt(RtFn::Raise), {const_str("TypeError"), const_str(msg)});
            return const_none();
        }
        if (is_set)
            call_void(RtFn::AnySetIndex, {o, k, v});
        else
            call_void(RtFn::AnyDelIndex, {o, k});
        return const_none();
    }
    // length_hint(x, default=0)：有 len 的容器取 len；没有 len 时给 default 就返回它，
    // 否则抛 TypeError（消息与 CPython 一致：object of type 'X' has no len()）。
    if (member == "length_hint") {
        if (c.args.empty() || c.args.size() > 2) {
            eval_all();
            err(c.line, c.col, "operator.length_hint() expects 1 or 2 arguments");
            return const_none();
        }
        VReg v = expr(*c.args[0]);
        TypePtr t = strip_optional(argty(0));
        bool has_default = c.args.size() == 2;
        VReg dv = has_default ? expr(*c.args[1]) : const_int(0);
        auto tk = t ? t->kind : TyKind::Unknown;
        bool has_len = tk == TyKind::List || tk == TyKind::Str || tk == TyKind::Dict ||
                       tk == TyKind::Tuple || tk == TyKind::Set || tk == TyKind::Bytes ||
                       tk == TyKind::Range;
        if (has_len) {
            Inst ln;
            ln.op = IrOp::Len;
            ln.a = v;
            return def(t_int(), std::move(ln));
        }
        // 没有 len 就返回 default（签名里 default 默认 0）。注意 CPython 的
        // operator.length_hint(obj) 对没有 len 的对象**不抛**，直接返回 0 ——
        // 与内置 len() 不同，别按 len 的语义写。
        return dv;
    }
    if (member == "itemgetter") {
        TypePtr fty = rt && rt->kind == TyKind::Func ? rt : nullptr;
        if (!fty) {
            eval_all();
            err(c.line, c.col, "operator.itemgetter() is not callable here");
            return const_none();
        }
        return build_itemgetter(c, fty);
    }
    if (member == "attrgetter") {
        eval_all();
        err(c.line, c.col, "operator.attrgetter() is not supported yet");
        return const_none();
    }
    if (auto it = kBin.find(member); it != kBin.end()) {
        if (c.args.size() != 2) {
            err(c.line, c.col,
                std::format("operator.{}() expects 2 arguments, got {}", member,
                            c.args.size()));
            eval_all();
            return const_none();
        }
        VReg l = expr(*c.args[0]);
        VReg r = expr(*c.args[1]);
        TypePtr lt = argty(0);
        TypePtr rty = argty(1);
        if ((lt && (lt->kind == TyKind::DateTime || lt->kind == TyKind::TimeDelta)) ||
            (rty && (rty->kind == TyKind::DateTime || rty->kind == TyKind::TimeDelta))) {
            err(c.line, c.col,
                std::format("operator.{}() does not support datetime/timedelta yet",
                            member));
            return const_none();
        }
        if ((lt && lt->kind == TyKind::Class) || (rty && rty->kind == TyKind::Class)) {
            VReg dr = lower_dunder_binop(l, lt, r, rty, it->second, false);
            if (dr != kNoReg) return dr;
        }
        // operator.mul 也支持序列重复。
        if (member == "mul") {
            auto seq = [](const TypePtr& t) {
                return t && (t->kind == TyKind::Str || t->kind == TyKind::List ||
                             t->kind == TyKind::Tuple);
            };
            if (seq(lt) || seq(rty)) {
                bool left = seq(lt);
                VReg base = left ? l : r;
                VReg n = coerce(left ? r : l, left ? rty : lt, t_int());
                RtFn fn = rt && rt->kind == TyKind::Str ? RtFn::StrRepeat : RtFn::ListRepeat;
                VReg v = call_runtime(fn, rt, {base, n});
                push_temp(v);
                return v;
            }
        }
        // 和普通二元运算保持一致：容器拼接后端还没有对应指令。
        if (rt && (rt->kind == TyKind::List || rt->kind == TyKind::Tuple ||
                   rt->kind == TyKind::Set || rt->kind == TyKind::Dict)) {
            err(c.line, c.col,
                std::format("operator.{}() on '{}' is not supported by the code generator "
                            "yet",
                            member, type_name(rt)));
            return const_none();
        }
        if (rt && rt->kind == TyKind::Str && member != "add" && member != "concat") {
            err(c.line, c.col,
                std::format("operator.{}() on 'str' is not supported", member));
            return const_none();
        }
        if (rt && rt->kind == TyKind::Float) {
            if (!ir_is_float(lt)) l = convert_to(l, lt, t_float());
            if (!ir_is_float(rty)) r = convert_to(r, rty, t_float());
        }
        Inst in;
        in.op = IrOp::Bin;
        in.bop = map_bin(it->second);
        in.a = l;
        in.b = r;
        return def_heap(rt, std::move(in));
    }
    if (auto it = kCmp.find(member); it != kCmp.end()) {
        if (c.args.size() != 2) {
            err(c.line, c.col,
                std::format("operator.{}() expects 2 arguments, got {}", member,
                            c.args.size()));
            eval_all();
            return const_bool(false);
        }
        VReg l = expr(*c.args[0]);
        VReg r = expr(*c.args[1]);
        TypePtr lt = argty(0);
        TypePtr rty = argty(1);
        CmpOp op = it->second;
        if (lt && rty && lt->kind == rty->kind &&
            (lt->kind == TyKind::DateTime || lt->kind == TyKind::TimeDelta)) {
            RtFn fn = lt->kind == TyKind::TimeDelta ? RtFn::TimeDeltaCmp : RtFn::DateTimeCmp;
            VReg cv = call_runtime(fn, t_int(), {l, r});
            Inst in;
            in.op = IrOp::Cmp;
            in.cop = map_cmp(op);
            in.a = cv;
            in.b = const_int(0);
            return def(t_bool(), std::move(in));
        }
        if (op == CmpOp::Eq || op == CmpOp::Ne) {
            VReg eq = lower_dunder_eq(l, lt, r, rty, op == CmpOp::Ne);
            if (eq != kNoReg) return eq;
            VReg ceq = lower_container_eq(l, lt, r, rty);
            if (ceq != kNoReg) {
                if (op == CmpOp::Ne) {
                    Inst un;
                    un.op = IrOp::Un;
                    un.uop = UnKind::Not;
                    un.a = ceq;
                    return def(t_bool(), std::move(un));
                }
                return ceq;
            }
        } else {
            VReg dc = lower_dunder_cmp(l, lt, r, rty, op);
            if (dc != kNoReg) return dc;
        }
        if ((ir_is_float(lt) || ir_is_float(rty)) && is_num(lt) && is_num(rty)) {
            if (!ir_is_float(lt)) l = convert_to(l, lt, t_float());
            if (!ir_is_float(rty)) r = convert_to(r, rty, t_float());
        }
        Inst in;
        in.op = IrOp::Cmp;
        in.cop = map_cmp(op);
        in.a = l;
        in.b = r;
        return def(t_bool(), std::move(in));
    }
    if (member == "is_" || member == "is_not") {
        if (c.args.size() != 2) {
            err(c.line, c.col,
                std::format("operator.{}() expects 2 arguments, got {}", member,
                            c.args.size()));
            eval_all();
            return const_bool(false);
        }
        VReg l = expr(*c.args[0]);
        VReg r = expr(*c.args[1]);
        auto as_bits = [&](VReg v, TypePtr t) -> VReg {
            if (!t || !ir_is_heap(t)) return v;
            Inst mv;
            mv.op = IrOp::Mov;
            mv.a = v;
            return def(t_int(), std::move(mv));
        };
        Inst in;
        in.op = IrOp::Cmp;
        in.cop = member == "is_" ? CmpKind::Eq : CmpKind::Ne;
        in.a = as_bits(l, argty(0));
        in.b = as_bits(r, argty(1));
        return def(t_bool(), std::move(in));
    }
    if (member == "truth" || member == "not_") {
        if (c.args.size() != 1) {
            err(c.line, c.col,
                std::format("operator.{}() expects 1 argument, got {}", member,
                            c.args.size()));
            eval_all();
            return const_bool(false);
        }
        VReg v = expr(*c.args[0]);
        VReg t = truth_bool(v, argty(0));
        if (member == "truth") return t;
        Inst un;
        un.op = IrOp::Un;
        un.uop = UnKind::Not;
        un.a = t;
        return def(t_bool(), std::move(un));
    }
    if (member == "neg" || member == "pos" || member == "abs" || member == "invert") {
        if (c.args.size() != 1) {
            err(c.line, c.col,
                std::format("operator.{}() expects 1 argument, got {}", member,
                            c.args.size()));
            eval_all();
            return const_none();
        }
        VReg v = expr(*c.args[0]);
        TypePtr t = argty(0);
        // 用户类：走 __neg__/__pos__/__abs__/__invert__，和一元运算符一致。
        if (t && t->kind == TyKind::Class) {
            const char* dn = member == "neg"    ? "__neg__"
                             : member == "pos"  ? "__pos__"
                             : member == "abs"  ? "__abs__"
                                                : "__invert__";
            TypePtr want = (rt && rt->kind != TyKind::Unknown) ? rt : nullptr;
            VReg r = dunder_call(v, t, dn, {}, want);
            if (r != kNoReg) return r;
            err(c.args[0]->line, c.args[0]->col,
                std::format("bad operand type for operator.{}(): '{}'", member,
                            t->cls ? t->cls->name : "object"));
            return const_none();
        }
        if (member == "abs") {
            if (t && t->kind == TyKind::Float)
                return call_runtime(RtFn::MathFabs, t_float(), {v});
            if (t && t->kind == TyKind::Complex)
                return call_runtime(RtFn::ComplexAbs, t_float(), {v});
            return call_runtime(RtFn::AbsI64, t_int(), {v});
        }
        if (member == "pos") return v;
        if (member == "neg" && t && t->kind == TyKind::Complex) {
            VReg out = call_runtime(RtFn::ComplexNeg, rt, {v});
            push_temp(out);
            return out;
        }
        if (member == "neg" && t && t->kind == TyKind::TimeDelta) {
            VReg out = call_runtime(RtFn::TimeDeltaNeg, rt, {v});
            push_temp(out);
            return out;
        }
        Inst in;
        in.op = IrOp::Un;
        in.uop = member == "neg" ? UnKind::Neg : UnKind::BitNot;
        in.a = v;
        return def(rt, std::move(in));
    }
    if (member == "contains") {
        if (c.args.size() != 2) {
            err(c.line, c.col,
                std::format("operator.contains() expects 2 arguments, got {}",
                            c.args.size()));
            eval_all();
            return const_bool(false);
        }
        VReg cont = expr(*c.args[0]);
        VReg item = expr(*c.args[1]);
        TypePtr ct = argty(0);
        TypePtr ity = argty(1);
        if (ct && (ct->kind == TyKind::List || ct->kind == TyKind::Tuple)) {
            TypePtr et = ct->sub.empty() ? t_unknown() : ct->sub[0];
            item = coerce(item, ity, et);
            int ck = key_kind(et);
            if (ct->kind == TyKind::Tuple)
                for (auto& sub : ct->sub)
                    if (key_kind(sub) != ck) {
                        ck = 3;
                        break;
                    }
            return call_runtime(RtFn::ListContains, t_bool(),
                                {cont, item, const_int(ck)});
        }
        if (ct && (ct->kind == TyKind::Dict || ct->kind == TyKind::Set)) {
            TypePtr kt = ct->sub.empty() ? t_unknown() : ct->sub[0];
            item = coerce(item, ity, kt);
            return call_runtime(RtFn::DictContains, t_bool(), {cont, item});
        }
        if (ct && (ct->kind == TyKind::Str || ct->kind == TyKind::Bytes))
            return call_runtime(RtFn::StrContains, t_bool(), {cont, item});
        if (ct && ct->kind == TyKind::Range) {
            item = coerce(item, ity, t_int());
            return call_runtime(RtFn::RangeContains, t_bool(), {cont, item});
        }
        err(c.line, c.col, std::format("operator.contains() on '{}' is not supported",
                                       type_name(ct)));
        return const_bool(false);
    }
    // getitem
    if (c.args.size() != 2) {
        err(c.line, c.col,
            std::format("operator.getitem() expects 2 arguments, got {}", c.args.size()));
        eval_all();
        return const_none();
    }
    VReg obj = expr(*c.args[0]);
    VReg idx = expr(*c.args[1]);
    TypePtr ot = argty(0);
    TypePtr ity = argty(1);
    if (ot && ot->kind == TyKind::Dict) {
        TypePtr kt = ot->sub.empty() ? t_unknown() : ot->sub[0];
        idx = coerce(idx, ity, kt);
        return call_runtime(RtFn::DictGet, rt, {obj, idx});
    }
    if (ot && (ot->kind == TyKind::List || ot->kind == TyKind::Tuple ||
               ot->kind == TyKind::Str)) {
        idx = coerce(idx, ity, t_int());
        Inst in;
        in.op = IrOp::IndexLoad;
        in.a = obj;
        in.b = idx;
        if (ot->kind == TyKind::Str && rt && rt->kind == TyKind::Str)
            return def_heap(rt, std::move(in));
        return def(rt, std::move(in));
    }
    err(c.line, c.col,
        std::format("operator.getitem() on '{}' is not supported", type_name(ot)));
    return const_none();
}

VReg Lowerer::lower_collections(const std::string& member, Call& c) {
    // deque 收 maxlen= 关键字，其它几个不收。
    if (!c.keywords.empty() && member != "deque") {
        err(c.line, c.col,
            std::format("collections.{}() does not take keyword arguments", member));
    }
    if (member == "defaultdict") {
        if (c.args.size() > 2) {
            err(c.line, c.col,
                std::format("collections.defaultdict() expects at most 2 arguments, got {}",
                            c.args.size()));
            return const_none();
        }
        int64_t factory = 0;
        if (!c.args.empty()) {
            auto* n = dynamic_cast<Name*>(c.args[0].get());
            auto* none = dynamic_cast<Constant*>(c.args[0].get());
            if (n && is_builtin(n->id)) {
                if (n->id == "int") factory = 1;
                else if (n->id == "float") factory = 2;
                else if (n->id == "bool") factory = 3;
                else if (n->id == "str") factory = 4;
                else if (n->id == "list") factory = 5;
                else if (n->id == "dict") factory = 6;
                else if (n->id == "set") factory = 7;
                else if (n->id == "tuple") factory = 8;
                else {
                    expr(*c.args[0]);
                    err(c.args[0]->line, c.args[0]->col,
                        std::format("collections.defaultdict() factory '{}' is not supported",
                                    n->id));
                }
            } else if (none && none->ck == Constant::CK::None) {
                expr(*c.args[0]);
            } else {
                expr(*c.args[0]);
                err(c.args[0]->line, c.args[0]->col,
                    "collections.defaultdict() currently supports built-in factories only");
            }
        }
        TypePtr rt = tc_.type_of(&c);
        if (!rt || rt->kind != TyKind::Dict) rt = mk_dict(t_unknown(), t_unknown());
        DictExpr empty;
        empty.line = c.line;
        empty.col = c.col;
        VReg dict = lower_dict(empty, rt);
        if (factory) call_void(RtFn::DictSetDefaultFactory, {dict, const_int(factory)});
        if (c.args.size() == 2) {
            VReg init = expr(*c.args[1]);
            call_void(RtFn::DictUpdate, {dict, init});
        }
        return dict;
    }
    if (member == "Counter") {
        if (c.args.size() != 1) {
            err(c.line, c.col,
                std::format("collections.Counter() expects 1 argument, got {}",
                            c.args.size()));
            return const_none();
        }
        TypePtr at = tc_.type_of(c.args[0].get());
        TypePtr et = (at && at->kind == TyKind::List && !at->sub.empty()) ? at->sub[0]
                                                                          : t_unknown();
        VReg items = expr(*c.args[0]);
        TypePtr rt = tc_.type_of(&c);
        VReg v = call_runtime(RtFn::CollCounter, rt,
                              {items, const_int(hash_key_kind(et)), const_int(own_kind(et))});
        push_temp(v);
        return v;
    }
    if (member == "OrderedDict") {
        // pyadamas 的 dict 本来就按插入序保存，OrderedDict 直接当空 dict 用。
        if (!c.args.empty()) {
            err(c.line, c.col, "collections.OrderedDict() takes no arguments in pyadamas");
            return const_none();
        }
        DictExpr empty;
        empty.line = c.line;
        empty.col = c.col;
        return lower_dict(empty, mk_dict(t_str(), t_unknown()));
    }
    if (member == "deque") {
        TypePtr rt = tc_.type_of(&c);
        if (!rt || rt->kind != TyKind::Deque) rt = new_ty(TyKind::Deque);
        VReg maxlen = const_int(0);
        for (auto& kw : c.keywords) {
            if (kw.arg == "maxlen") {
                maxlen = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_int());
            } else {
                err(kw.value->line, kw.value->col,
                    std::format("collections.deque() got an unexpected keyword argument '{}'",
                                kw.arg));
            }
        }
        if (c.args.size() > 2) {
            err(c.line, c.col,
                "collections.deque() takes at most 2 positional arguments");
            return const_none();
        }
        if (c.args.size() == 2) {
            maxlen = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int());
        }
        if (!c.args.empty()) {
            TypePtr at = tc_.type_of(c.args[0].get());
            VReg items;
            if (at && at->kind == TyKind::Str) {
                items = call_runtime(RtFn::StrToList, mk_list(t_str()), {expr(*c.args[0])});
                push_temp(items);
            } else {
                items = expr(*c.args[0]);
            }
            VReg v = call_runtime(RtFn::DequeFromList, rt, {items, maxlen});
            push_temp(v);
            return v;
        }
        VReg v = call_runtime(RtFn::DequeNew, rt, {maxlen});
        push_temp(v);
        return v;
    }
    err(c.line, c.col, std::format("collections.{} is not supported", member));
    return const_none();
}


}  // namespace pyadamas
