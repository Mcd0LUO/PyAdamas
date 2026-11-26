// lower/builtin_misc.cpp —— 内置函数：IO、数值与内部工具
#include "lower/impl.h"

namespace pyadamas {

VReg Lowerer::lower_builtin_misc(const std::string& name, Call& c) {
    if (name == "open") {
        static const char* kParams[] = {"file",      "mode",    "buffering", "encoding",
                                        "errors",    "newline", "closefd",   "opener"};
        constexpr size_t kNP = 8;
        if (c.args.size() > kNP) {
            err(c.line, c.col, "open() takes at most 8 arguments");
            return const_none();
        }
        std::vector<VReg> bound(kNP, kNoReg);
        std::vector<Expr*> bexpr(kNP, nullptr);
        // 求值顺序按源码：位置实参先，关键字按出现顺序。
        for (size_t i = 0; i < c.args.size(); ++i) {
            bound[i] = expr(*c.args[i]);
            bexpr[i] = c.args[i].get();
        }
        for (auto& kw : c.keywords) {
            if (kw.arg.empty()) {
                err(kw.value->line, kw.value->col, "open() does not take '**kwargs'");
                continue;
            }
            int idx = -1;
            for (int p = 0; p < static_cast<int>(kNP); ++p)
                if (kw.arg == kParams[p]) idx = p;
            VReg v = expr(*kw.value);
            if (idx < 0) {
                err(kw.value->line, kw.value->col,
                    std::format("open() got an unexpected keyword argument '{}'", kw.arg));
                continue;
            }
            if (bound[static_cast<size_t>(idx)] != kNoReg) {
                err(kw.value->line, kw.value->col,
                    std::format("open() got multiple values for argument '{}'", kw.arg));
                continue;
            }
            bound[static_cast<size_t>(idx)] = v;
            bexpr[static_cast<size_t>(idx)] = kw.value.get();
        }
        if (bound[0] == kNoReg) {
            err(c.line, c.col, "open() missing required argument 'file'");
            return const_none();
        }
        if (bound[7] != kNoReg) {
            err(c.line, c.col, "open() opener= is not supported by the code generator");
            return const_none();
        }
        TypePtr ft = tc_.type_of(bexpr[0]);
        if (!ft || (ft->kind != TyKind::Str && ft->kind != TyKind::Int &&
                    ft->kind != TyKind::Bool)) {
            err(c.line, c.col, "open() file must be a statically known path or file descriptor");
            return const_none();
        }
        bool is_fd = ft->kind == TyKind::Int || ft->kind == TyKind::Bool;
        VReg mode = bound[1] == kNoReg ? const_int(0) : bound[1];
        VReg buffering = bound[2] == kNoReg
                             ? const_int(-1)
                             : coerce(bound[2], tc_.type_of(bexpr[2]), t_int());
        // newline: None→0(通用换行), ""→1, "\n"→2, "\r"→3, "\r\n"→4
        int64_t nl = 0;
        if (bexpr[5]) {
            const Constant* cc =
                bexpr[5]->kind == ExprKind::Constant ? static_cast<const Constant*>(bexpr[5])
                                                     : nullptr;
            if (cc && cc->ck == Constant::CK::None) {
                nl = 0;
            } else if (cc && cc->ck == Constant::CK::Str) {
                if (cc->s.empty()) nl = 1;
                else if (cc->s == "\n") nl = 2;
                else if (cc->s == "\r") nl = 3;
                else if (cc->s == "\r\n") nl = 4;
                else {
                    err(bexpr[5]->line, bexpr[5]->col,
                        "open() newline must be None, '', '\\n', '\\r' or '\\r\\n'");
                    nl = 1;
                }
            } else {
                err(bexpr[5]->line, bexpr[5]->col,
                    "open() newline must be a literal (None, '', '\\n', '\\r' or '\\r\\n')");
            }
        }
        VReg closefd = bound[6] == kNoReg
                           ? const_int(1)
                           : coerce(bound[6], tc_.type_of(bexpr[6]), t_int());
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::FileOpen);
        in.args = {is_fd ? const_int(0) : bound[0], is_fd ? bound[0] : const_int(-1), mode,
                   buffering, const_int(nl), closefd};
        in.ty = t_file();
        VReg f = def(t_file(), std::move(in));
        push_temp(f);
        // encoding/errors 只在实际给了的时候再配置一次（默认 utf-8/strict）。
        if (bound[3] != kNoReg || bound[4] != kNoReg) {
            call_void(RtFn::FileConfig,
                      {f, bound[3] == kNoReg ? const_int(0) : bound[3],
                       bound[4] == kNoReg ? const_int(0) : bound[4]});
        }
        return f;
    }

    if (name == "input") {
        if (c.args.size() > 1) {
            err(c.line, c.col, "input() takes at most one argument");
            return const_str("");
        }
        VReg prompt = c.args.empty() ? const_int(0) : expr(*c.args[0]);
        VReg s = call_runtime(RtFn::Input, t_str(), {prompt});
        push_temp(s);
        return s;
    }

    if (name == "abs") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "abs() takes exactly one argument");
            return const_none();
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        VReg v = expr(*c.args[0]);
        // 用户类走 __abs__（CPython 语义），否则下面会把对象指针当 i64
        // 取绝对值——静默算错。
        if (t && t->kind == TyKind::Class) {
            TypePtr want = tc_.type_of(&c);
            if (want && want->kind == TyKind::Unknown) want = nullptr;
            VReg r = dunder_call(v, t, "__abs__", {}, want);
            if (r != kNoReg) return r;
            err(c.args[0]->line, c.args[0]->col,
                std::format("bad operand type for abs(): '{}'",
                            t->cls ? t->cls->name : "object"));
            return const_none();
        }
        if (t && t->kind == TyKind::Float)
            return call_runtime(RtFn::MathFabs, t_float(), {v});
        if (t && t->kind == TyKind::Complex)
            return call_runtime(RtFn::ComplexAbs, t_float(), {v});
        if (t && t->kind == TyKind::Big) {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::BigAbs);
            in.args = {v};
            return def_heap(t_big(), std::move(in));
        }
        return call_runtime(RtFn::AbsI64, t_int(), {v});
    }

    if (name == "min" || name == "max") return lower_minmax(c, name == "min");
    if (name == "sum") return lower_sum(c);
    if (name == "any" || name == "all") return lower_anyall(c, name == "any");
    if (name == "complex") {
        if (c.args.size() > 2) {
            err(c.line, c.col, "complex() takes at most two arguments");
            return const_none();
        }
        VReg re = const_float(0.0);
        VReg im = const_float(0.0);
        if (!c.args.empty()) {
            TypePtr t0 = tc_.type_of(c.args[0].get());
            VReg v0 = expr(*c.args[0]);
            if (t0 && t0->kind == TyKind::Complex && c.args.size() == 1) return v0;
            re = (t0 && t0->kind == TyKind::Float) ? v0 : convert_to(v0, t0, t_float());
        }
        if (c.args.size() == 2) {
            TypePtr t1 = tc_.type_of(c.args[1].get());
            VReg v1 = expr(*c.args[1]);
            im = (t1 && t1->kind == TyKind::Float) ? v1 : convert_to(v1, t1, t_float());
        }
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::ComplexNew);
        in.args = {re, im};
        return def_heap(t_complex(), std::move(in));
    }

    if (name == "pow") {
        if (c.args.size() != 2 && c.args.size() != 3) {
            err(c.line, c.col, "pow() takes 2 or 3 arguments");
            return const_none();
        }
        TypePtr ty = tc_.type_of(&c);
        if (!ty || ty->kind == TyKind::Unknown) ty = t_int();
        if (c.args.size() == 3) {
            bool any_big = false;
            for (auto& arg : c.args)
                if (tc_.type_of(arg.get())->kind == TyKind::Big) any_big = true;
            if (any_big) {
                // 0.495：big 的模幂走运行时（平方乘 + 逐步取模）。
                VReg b = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_big());
                VReg e = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_big());
                VReg m = coerce(expr(*c.args[2]), tc_.type_of(c.args[2].get()), t_big());
                VReg r = call_runtime(RtFn::BigPowMod, t_big(), {b, e, m});
                push_temp(r);
                return r;
            }
            VReg a = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
            VReg b = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int());
            VReg m = coerce(expr(*c.args[2]), tc_.type_of(c.args[2].get()), t_int());
            // 0.495：int 的三参 pow 也走 big 的模幂 —— 旧的 Pow+Mod 在 i64 上
            // 先算幂再取模，2 ** 100 直接回绕成 0（CPython 是 976371285）。
            // 模数在 i64 内 ⇒ 结果必然在 i64 内，回程 BigToI64 无损。
            VReg ab = call_runtime(RtFn::BigFromI64, t_big(), {a});
            push_temp(ab);
            VReg bb = call_runtime(RtFn::BigFromI64, t_big(), {b});
            push_temp(bb);
            VReg mb = call_runtime(RtFn::BigFromI64, t_big(), {m});
            push_temp(mb);
            VReg pm = call_runtime(RtFn::BigPowMod, t_big(), {ab, bb, mb});
            push_temp(pm);
            return call_runtime(RtFn::BigToI64, t_int(), {pm});
        }
        VReg a = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), ty);
        VReg b = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), ty);
        Inst in;
        if (ty->kind == TyKind::Big) {
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::BigPow);
            in.args = {a, b};
            return def_heap(ty, std::move(in));
        }
        in.op = IrOp::Bin;
        in.bop = BinKind::Pow;
        in.a = a;
        in.b = b;
        return def(ty, std::move(in));
    }

    return kNoReg;
}

}  // namespace pyadamas
