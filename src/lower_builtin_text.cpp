// lower_builtin_text.cpp —— 内置函数：文本表示与标量转换
#include "lower_impl.h"

namespace pyadamas {

VReg Lowerer::lower_builtin_text(const std::string& name, Call& c) {
    // pa_repr_tuple 只认识 int/float/str/bool/None 五种元素。异构元组或
    // 元素是类/复数/range 时没有单一 kind，只能逐个取元素、按各自静态
    // 类型转文本再拼。CPython 的 str(tuple) 就是 repr(tuple)。
    auto tuple_repr_any = [&](auto&& self, TypePtr tt, VReg tv) -> VReg {
        auto scalar_kind = [](TypePtr et) -> int64_t {
            if (!et) return -1;
            switch (et->kind) {
                case TyKind::Int: return 0;
                case TyKind::Float: return 1;
                case TyKind::Str: return 2;
                case TyKind::Bool: return 3;
                case TyKind::None: return 4;
                default: return -1;
            }
        };
        auto concat = [&](VReg a, VReg b) {
            VReg s = call_runtime(RtFn::StrConcat, t_str(), {a, b});
            push_temp(s);
            return s;
        };
        VReg acc = const_str("(");
        for (size_t i = 0; i < tt->sub.size(); ++i) {
            if (i) acc = concat(acc, const_str(", "));
            TypePtr et = tt->sub[i];
            // 元组元素是容器内部的借用引用，不能交给临时栈释放。
            Inst get;
            get.op = IrOp::IndexLoad;
            get.a = tv;
            get.b = const_int(static_cast<int64_t>(i));
            VReg ev = def(et ? et : t_unknown(), std::move(get));
            int64_t k = scalar_kind(et);
            VReg piece = kNoReg;
            if (k == 2) {
                piece = call_runtime(RtFn::ReprStr, t_str(), {ev});
                push_temp(piece);
            } else if (k >= 0) {
                piece = convert_to(ev, et, t_str());
            } else if (et && et->kind == TyKind::Tuple) {
                int64_t ek = et->sub.empty() ? 0 : scalar_kind(et->sub[0]);
                bool homo = ek >= 0;
                for (size_t j = 1; homo && j < et->sub.size(); ++j)
                    if (scalar_kind(et->sub[j]) != ek) homo = false;
                if (et->variadic && ek < 0) {
                    err(c.line, c.col,
                        std::format("repr() of a variadic tuple with element type '{}' "
                                    "is not supported yet",
                                    type_name(et->sub[0])));
                    return const_str("");
                }
                if (homo) {
                    piece = call_runtime(RtFn::ReprTuple, t_str(), {ev, const_int(ek)});
                    push_temp(piece);
                } else {
                    piece = self(self, et, ev);
                }
            } else if (et && et->kind == TyKind::Range) {
                piece = call_runtime(RtFn::RangeRepr, t_str(), {ev});
                push_temp(piece);
            } else if (et && et->kind == TyKind::Complex) {
                piece = convert_to(ev, et, t_str());
            } else if (et && et->kind == TyKind::Class) {
                piece = dunder_str(ev, et, true);
                if (piece == kNoReg) piece = class_default_str(et);
            } else {
                err(c.line, c.col,
                    std::format("repr() of a tuple element of type '{}' is not "
                                "supported yet",
                                type_name(et)));
                return const_str("");
            }
            acc = concat(acc, piece);
        }
        if (tt->sub.size() == 1) acc = concat(acc, const_str(","));
        return concat(acc, const_str(")"));
    };

    // 同构且元素属于 pa_repr_tuple 支持的类型时走运行时快路径；
    // 其余固定长度元组逐元素拼接。变长元组只有元素同构，
    // 非标量元素无法在运行时按 kind 渲染。
    auto tuple_text = [&](TypePtr t, VReg v) -> VReg {
        TypePtr et = t->sub.empty() ? t_unknown() : t->sub[0];
        bool same = true;
        for (auto& sub : t->sub)
            if (!type_eq(sub, et)) { same = false; break; }
        bool scalar = et && (et->kind == TyKind::Int || et->kind == TyKind::Float ||
                             et->kind == TyKind::Str || et->kind == TyKind::Bool ||
                             et->kind == TyKind::None);
        if (same && scalar) {
            int64_t kind = 0;
            if (et->kind == TyKind::Float) kind = 1;
            else if (et->kind == TyKind::Str) kind = 2;
            else if (et->kind == TyKind::Bool) kind = 3;
            else if (et->kind == TyKind::None) kind = 4;
            VReg s = call_runtime(RtFn::ReprTuple, t_str(), {v, const_int(kind)});
            push_temp(s);
            return s;
        }
        if (t->variadic) {
            err(c.line, c.col,
                std::format("repr()/str() of a variadic tuple with element type '{}' "
                            "is not supported yet",
                            type_name(et)));
            return const_str("");
        }
        return tuple_repr_any(tuple_repr_any, t, v);
    };



    if (name == "ord") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "ord() takes exactly one argument");
            return const_int(0);
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        if (!t || (t->kind != TyKind::Str && t->kind != TyKind::Unknown)) {
            err(c.line, c.col, std::format("ord() expects a string, got '{}'", type_name(t)));
            return const_int(0);
        }
        VReg v = expr(*c.args[0]);
        return call_runtime(RtFn::Ord, t_int(), {v});
    }

    if (name == "__pa_capwords") {
        if (c.args.size() != 1 || !c.keywords.empty()) {
            err(c.line, c.col, "capwords() takes exactly one argument");
            return const_str("");
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        if (t && t->kind != TyKind::Str && t->kind != TyKind::Unknown) {
            err(c.line, c.col,
                std::format("capwords() expects a string, got '{}'", type_name(t)));
            return const_str("");
        }
        VReg v = expr(*c.args[0]);
        VReg r = call_runtime(RtFn::StrCapwords, t_str(), {v});
        push_temp(r);
        return r;
    }

    if (name == "__pa_sha256") {
        if (c.args.size() != 1 || !c.keywords.empty()) {
            err(c.line, c.col, "sha256() takes exactly one argument");
            return const_str("");
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        if (t && t->kind != TyKind::Bytes && t->kind != TyKind::Unknown) {
            err(c.line, c.col,
                std::format("sha256() expects bytes, got '{}'", type_name(t)));
            return const_str("");
        }
        VReg v = expr(*c.args[0]);
        VReg r = call_runtime(RtFn::Sha256Digest, t_bytes(), {v});
        push_temp(r);
        return r;
    }

    if (name == "__pa_sha256_hex") {
        if (c.args.size() != 1 || !c.keywords.empty()) {
            err(c.line, c.col, "sha256() takes exactly one argument");
            return const_str("");
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        if (t && t->kind != TyKind::Bytes && t->kind != TyKind::Unknown) {
            err(c.line, c.col,
                std::format("sha256() expects bytes, got '{}'", type_name(t)));
            return const_str("");
        }
        VReg v = expr(*c.args[0]);
        VReg r = call_runtime(RtFn::Sha256Hex, t_str(), {v});
        push_temp(r);
        return r;
    }

    if (name == "__pa_b64encode") {
        if (c.args.size() != 2 || !c.keywords.empty()) {
            err(c.line, c.col, "b64encode() takes exactly two arguments");
            return const_str("");
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        if (t && t->kind != TyKind::Bytes && t->kind != TyKind::Unknown) {
            err(c.line, c.col,
                std::format("b64encode() expects bytes, got '{}'", type_name(t)));
            return const_str("");
        }
        VReg v = expr(*c.args[0]);
        VReg flag = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int());
        VReg r = call_runtime(RtFn::B64Encode, t_bytes(), {v, flag});
        push_temp(r);
        return r;
    }

    if (name == "__pa_b64decode") {
        if (c.args.size() != 1 || !c.keywords.empty()) {
            err(c.line, c.col, "b64decode() takes exactly one argument");
            return const_str("");
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        if (t && t->kind != TyKind::Bytes && t->kind != TyKind::Unknown) {
            err(c.line, c.col,
                std::format("b64decode() expects bytes, got '{}'", type_name(t)));
            return const_str("");
        }
        VReg v = expr(*c.args[0]);
        VReg r = call_runtime(RtFn::B64Decode, t_bytes(), {v});
        push_temp(r);
        return r;
    }

    if (name == "chr") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "chr() takes exactly one argument");
            return const_str("");
        }
        VReg v = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
        VReg s = call_runtime(RtFn::Chr, t_str(), {v});
        push_temp(s);
        return s;
    }

    if (name == "hex" || name == "bin" || name == "oct") {
        if (c.args.size() != 1) {
            err(c.line, c.col, std::format("{}() takes exactly one argument", name));
            return const_str("");
        }
        int64_t base = name == "hex" ? 16 : name == "bin" ? 2 : 8;
        VReg v = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
        VReg s = call_runtime(RtFn::IntToBase, t_str(), {v, const_int(base)});
        push_temp(s);
        return s;
    }

    if (name == "round") {
        if (c.args.empty() || c.args.size() > 2) {
            err(c.line, c.col, "round() takes 1 or 2 arguments");
            return const_int(0);
        }
        TypePtr at = tc_.type_of(c.args[0].get());
        VReg v = expr(*c.args[0]);
        if (at && at->kind == TyKind::Float) {
            VReg nd = c.args.size() == 2
                          ? coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int())
                          : const_int(0);
            VReg r = call_runtime(RtFn::RoundF64, t_float(), {v, nd});
            // round(x) 返回 int；round(x, n) 仍返回 float。
            if (c.args.size() == 1) return convert_to(r, t_float(), t_int());
            return r;
        }
        // 整数输入：单参就是它自己；带负 ndigits 要按 10 的幂取整。
        if (c.args.size() == 2 && at &&
            (at->kind == TyKind::Int || at->kind == TyKind::Bool)) {
            VReg nd = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int());
            return call_runtime(RtFn::RoundI64, t_int(), {v, nd});
        }
        return v;
    }

    if (name == "divmod") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "divmod() takes exactly two arguments");
            return const_none();
        }
        TypePtr ty = tc_.type_of(&c);
        TypePtr et = t_int();
        if (ty && ty->kind == TyKind::Tuple && !ty->sub.empty()) et = ty->sub[0];
        VReg a = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), et);
        VReg b = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), et);
        Inst q;
        q.op = IrOp::Bin;
        q.bop = BinKind::FloorDiv;
        q.a = a;
        q.b = b;
        VReg qv = def(et, std::move(q));
        Inst m;
        m.op = IrOp::Bin;
        m.bop = BinKind::Mod;
        m.a = a;
        m.b = b;
        VReg mv = def(et, std::move(m));
        Inst mt;
        mt.op = IrOp::MakeTuple;
        mt.imm = !et ? 2 : et->kind == TyKind::Unknown ? 2 : (ir_is_heap(et) ? 1 : 0);
        mt.args = {qv, mv};
        return def_heap(ty && ty->kind == TyKind::Tuple ? ty : mk_tuple({et, et}),
                        std::move(mt));
    }

    if (name == "repr") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "repr() takes exactly one argument");
            return const_str("");
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        VReg v = expr(*c.args[0]);
        if (t && t->kind == TyKind::Str) {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::ReprStr);
            in.args = {v};
            return def_heap(t_str(), std::move(in));
        }
        if (t && t->kind == TyKind::Range) {
            // repr(range(10)) 是 "range(0, 10)"，和 CPython 一样
            VReg s = call_runtime(RtFn::RangeRepr, t_str(), {v});
            push_temp(s);
            return s;
        }
        if (t && t->kind == TyKind::Tuple) return tuple_text(t, v);
        if (t && t->kind == TyKind::ClassObj) {
            VReg s = call_runtime(RtFn::ClassRepr, t_str(), {v});
            push_temp(s);
            return s;
        }
        if (t && (t->kind == TyKind::Int || t->kind == TyKind::Bool ||
                  t->kind == TyKind::Float || t->kind == TyKind::Complex ||
                  t->kind == TyKind::None))
            return convert_to(v, t, t_str());
        if (t && t->kind == TyKind::Class) {
            VReg s = dunder_str(v, t, true);
            return s != kNoReg ? s : class_default_str(t);
        }
        err(c.line, c.col,
            std::format("repr() of '{}' is not supported by the code generator yet",
                        type_name(t)));
        return const_str("");
    }

    if (name == "format") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "format() takes exactly two arguments");
            return const_str("");
        }
        TypePtr vt = tc_.type_of(c.args[0].get());
        if (vt && vt->kind == TyKind::Class && vt->cls &&
            vt->cls->method("__format__")) {
            VReg v = expr(*c.args[0]);
            VReg spec = expr(*c.args[1]);
            if (spec == kNoReg) return const_str("");
            VReg r = dunder_call(v, vt, "__format__", {spec}, t_str());
            if (r == kNoReg) {
                err(c.line, c.col,
                    std::format("__format__ on '{}' must be a normal method",
                                vt->cls->name));
                return const_str("");
            }
            return r;
        }
        RtFn fn = RtFn::FmtI64;
        if (vt && vt->kind == TyKind::Float) fn = RtFn::FmtF64;
        else if (vt && vt->kind == TyKind::Bool) fn = RtFn::FmtBool;
        else if (vt && vt->kind == TyKind::Str) fn = RtFn::FmtStr;
        else if (vt && vt->kind == TyKind::Complex) fn = RtFn::FmtComplex;
        else if (!vt || (vt->kind != TyKind::Int && vt->kind != TyKind::Unknown)) {
            err(c.line, c.col,
                std::format("format() of '{}' is not supported yet", type_name(vt)));
            return const_str("");
        }
        VReg v = expr(*c.args[0]);
        VReg spec = expr(*c.args[1]);
        if (spec == kNoReg) return const_str("");
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(fn);
        in.args = {v, spec};
        return def_heap(t_str(), std::move(in));
    }

    if (name == "int" || name == "float" || name == "str" || name == "bool") {
        TypePtr to = name == "int"     ? t_int()
                     : name == "float" ? t_float()
                     : name == "str"   ? t_str()
                                       : t_bool();
        if (c.args.empty()) return to->kind == TyKind::Int ? const_int(0)
                                       : to->kind == TyKind::Float ? const_float(0)
                                       : to->kind == TyKind::Str   ? const_str("")
                                                                   : const_bool(false);
        // int(str, base)
        if (name == "int" && c.args.size() == 2) {
            TypePtr st = tc_.type_of(c.args[0].get());
            VReg sv = expr(*c.args[0]);
            if (!st || st->kind != TyKind::Str) {
                err(c.line, c.col, "int(x, base) requires a string first argument");
                return const_int(0);
            }
            VReg bv = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int());
            return call_runtime(RtFn::StrToI64, t_int(), {sv, bv});
        }
        if (c.args.size() > 1) {
            err(c.line, c.col, std::format("{}() takes at most one argument", name));
            return const_none();
        }
        VReg v = expr(*c.args[0]);
        TypePtr from = tc_.type_of(c.args[0].get());
        // 字符串解析：以前走 Convert 会把字符串指针直接当数值，静默算错。
        if (name == "int" && from && from->kind == TyKind::Str)
            return call_runtime(RtFn::StrToI64, t_int(), {v, const_int(10)});
        if (name == "float" && from && from->kind == TyKind::Str)
            return call_runtime(RtFn::StrToF64, t_float(), {v});
        if (to->kind == TyKind::Str && from && from->kind == TyKind::Exception) {
            VReg s = call_runtime(RtFn::ExcValueStr, t_str(), {v});
            push_temp(s);
            return s;
        }
        if (to->kind == TyKind::Str && from && from->kind == TyKind::Tuple) {
            // str(tuple) 与 repr(tuple) 相同（CPython 也是这样）
            return tuple_text(from, v);
        }
        if (to->kind == TyKind::Str && from && from->kind == TyKind::ClassObj) {
            VReg s = call_runtime(RtFn::ClassRepr, t_str(), {v});
            push_temp(s);
            return s;
        }
        // 容器转标量没有定义（CPython 抛 TypeError）。以前这里直接把
        // 指针当数字用，会静默给出垃圾值——宁可报错。
        if (to->kind != TyKind::Bool && from &&
            (from->kind == TyKind::List || from->kind == TyKind::Dict ||
             from->kind == TyKind::Set ||
             (from->kind == TyKind::Range && to->kind != TyKind::Str))) {
            err(c.args[0]->line, c.args[0]->col,
                std::format("{}() argument must be a number or a string, not '{}'",
                            name, type_name(from)));
            return to->kind == TyKind::Int ? const_int(0)
                   : to->kind == TyKind::Float ? const_float(0.0)
                   : to->kind == TyKind::Str ? const_str("") : const_bool(false);
        }
        if (to->kind == TyKind::Str && from && from->kind == TyKind::Range) {
            // str(range) 与 repr(range) 相同
            VReg s = call_runtime(RtFn::RangeRepr, t_str(), {v});
            push_temp(s);
            return s;
        }
        // unknown 值的 str()：运行期先查注册表看是不是堆字符串，是就原样
        // 返回，否则按整数格式化（和 print 对 unknown 的输出一致）。
        if (to->kind == TyKind::Str && (!from || from->kind == TyKind::Unknown)) {
            VReg s = call_runtime(RtFn::StrAny, t_str(), {v});
            push_temp(s);
            return s;
        }
        // 显式 int(unknown) / float(unknown)：解盒（float 盒）或按 int 解释。
        if (from && from->kind == TyKind::Unknown && to->kind == TyKind::Int)
            return call_runtime(RtFn::AnyToI64, t_int(), {v});
        if (from && from->kind == TyKind::Unknown && to->kind == TyKind::Float)
            return call_runtime(RtFn::AnyToF64, t_float(), {v});
        return convert_to(v, from, to);
    }

    if (name == "format") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "format() takes exactly two arguments");
            return const_none();
        }
        VReg v = expr(*c.args[0]);
        VReg spec = expr(*c.args[1]);
        TypePtr vt = tc_.type_of(c.args[0].get());
        RtFn fn = RtFn::FmtStr;
        if (vt) {
            switch (vt->kind) {
                case TyKind::Bool: fn = RtFn::FmtBool; break;
                case TyKind::Int: fn = RtFn::FmtI64; break;
                case TyKind::Float: fn = RtFn::FmtF64; break;
                case TyKind::Str: fn = RtFn::FmtStr; break;
                case TyKind::Complex: fn = RtFn::FmtComplex; break;
                case TyKind::Unknown: break;
                default:
                    err(c.args[0]->line, c.args[0]->col,
                        std::format("format() cannot format '{}'", type_name(vt)));
                    return const_none();
            }
        }
        VReg r = call_runtime(fn, t_str(), {v, spec});
        push_temp(r);
        return r;
    }

    return kNoReg;
}

}  // namespace pyadamas
