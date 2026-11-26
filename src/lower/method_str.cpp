// lower/method_str.cpp —— str / bytes 的方法调用降级
#include "lower/impl.h"

namespace pyadamas {

VReg Lowerer::lower_method_str(Attribute& at, Call& c, VReg recv) {
    if (!c.keywords.empty()) {
        err(c.line, c.col,
            std::format("str.{}() does not take keyword arguments", at.attr));
        return const_none();
    }
    auto need_str = [&](size_t i) -> VReg {
        VReg v = expr(*c.args[i]);
        TypePtr t = tc_.type_of(c.args[i].get());
        if (!t || (t->kind != TyKind::Str && t->kind != TyKind::Bytes))
            err(c.args[i]->line, c.args[i]->col,
                std::format("str.{}() expects a string argument", at.attr));
        return v;
    };
    auto need_int = [&](size_t i, int64_t def) -> VReg {
        if (i >= c.args.size()) return const_int(def);
        VReg v = expr(*c.args[i]);
        return coerce(v, tc_.type_of(c.args[i].get()), t_int());
    };
    if (at.attr == "encode") {
        // str.encode()：真正的 str -> bytes 转换（ASCII 零复制，非 ASCII 拷字节）。
        // 编码名参数继续忽略（现状如此）。
        for (auto& a : c.args) expr(*a);
        VReg v = call_runtime(RtFn::StrEncode, t_bytes(), {recv});
        push_temp(v);
        return v;
    }
    auto arg_count = [&](size_t lo, size_t hi) {
        if (c.args.size() >= lo && c.args.size() <= hi) return true;
        err(c.line, c.col,
            std::format("str.{}() takes {} argument(s)", at.attr,
                        lo == hi ? std::to_string(lo)
                                 : std::format("{}..{}", lo, hi)));
        return false;
    };
    auto emit_op = [&](StrOp op, TypePtr ret, VReg a0, VReg a1, VReg a2) -> VReg {
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::StrMethod);
        in.args = {recv, const_int(static_cast<int64_t>(op)),
                   a0 == kNoReg ? const_int(0) : a0,
                   a1 == kNoReg ? const_int(0) : a1,
                   a2 == kNoReg ? const_int(0) : a2};
        in.ty = ret;
        VReg v = def(ret, std::move(in));
        if (ir_is_heap(ret)) push_temp(v);
        return v;
    };
    TypePtr rty = tc_.type_of(&c);
    if (at.attr == "upper" || at.attr == "lower" || at.attr == "capitalize" ||
        at.attr == "title" || at.attr == "casefold") {
        if (!arg_count(0, 0)) return const_none();
        StrOp op = at.attr == "upper" ? StrOp::Upper
                   : at.attr == "lower" || at.attr == "casefold" ? StrOp::Lower
                   : at.attr == "capitalize" ? StrOp::Capitalize
                                               : StrOp::Title;
        return emit_op(op, t_str(), kNoReg, kNoReg, kNoReg);
    }
    if (at.attr == "strip" || at.attr == "lstrip" || at.attr == "rstrip") {
        if (!arg_count(0, 1)) return const_none();
        bool has_chars = c.args.size() == 1;
        StrOp op;
        if (at.attr == "strip") op = has_chars ? StrOp::StripChars : StrOp::Strip;
        else if (at.attr == "lstrip") op = has_chars ? StrOp::LStripChars : StrOp::LStrip;
        else op = has_chars ? StrOp::RStripChars : StrOp::RStrip;
        VReg chars = has_chars ? need_str(0) : kNoReg;
        return emit_op(op, t_str(), chars, kNoReg, kNoReg);
    }
    if (at.attr == "split") {
        if (!arg_count(0, 2)) return const_none();
        TypePtr lt = mk_list(t_str());
        if (c.args.empty())
            return emit_op(StrOp::SplitWs, lt, kNoReg, kNoReg, kNoReg);
        VReg sep = need_str(0);
        VReg maxsplit = need_int(1, -1);
        return emit_op(StrOp::SplitSep, lt, sep, maxsplit, kNoReg);
    }
    if (at.attr == "splitlines") {
        if (!arg_count(0, 0)) return const_none();
        return emit_op(StrOp::SplitLines, mk_list(t_str()), kNoReg, kNoReg, kNoReg);
    }
    if (at.attr == "join") {
        if (!arg_count(1, 1)) return const_none();
        TypePtr lt = tc_.type_of(c.args[0].get());
        VReg parts;
        if (lt && lt->kind == TyKind::Generator) {
            // 生成器没有下标：先物化成列表再拼。
            TypePtr se = t_unknown();
            parts = seq_as_list(c.args[0].get(), se);
        } else if (!lt || (lt->kind != TyKind::List && lt->kind != TyKind::Tuple)) {
            err(c.args[0]->line, c.args[0]->col,
                "str.join() expects a list of strings");
            return const_none();
        } else {
            parts = expr(*c.args[0]);
        }
        return emit_op(StrOp::Join, t_str(), parts, kNoReg, kNoReg);
    }
    if (at.attr == "replace") {
        if (!arg_count(2, 3)) return const_none();
        VReg old = need_str(0);
        VReg nw = need_str(1);
        VReg cnt = need_int(2, -1);
        return emit_op(StrOp::Replace, t_str(), old, nw, cnt);
    }
    if (at.attr == "find" || at.attr == "rfind" || at.attr == "index" ||
        at.attr == "rindex") {
        if (!arg_count(1, 3)) return const_none();
        VReg sub = need_str(0);
        VReg start = need_int(1, 0);
        VReg end = need_int(2, std::numeric_limits<int64_t>::max());
        StrOp op = at.attr == "find" ? StrOp::Find
                   : at.attr == "rfind" ? StrOp::RFind
                   : at.attr == "rindex" ? StrOp::RIndex : StrOp::Find;
        VReg pos = emit_op(op, t_int(), sub, start, end);
        if (at.attr == "index") {
            int ok_b = new_block("str.index.ok");
            int bad_b = new_block("str.index.bad");
            Inst c0;
            c0.op = IrOp::Cmp;
            c0.cop = CmpKind::Ge;
            c0.a = pos;
            c0.b = const_int(0);
            VReg ok = def(t_bool(), std::move(c0));
            Inst br;
            br.op = IrOp::Branch;
            br.a = ok;
            br.target = ok_b;
            br.target2 = bad_b;
            emit(std::move(br));
            switch_to(bad_b);
            call_void(raise_rt(RtFn::Raise),
                      {const_str("ValueError"), const_str("substring not found")});
            Inst j;
            j.op = IrOp::Jump;
            j.target = ok_b;
            emit(std::move(j));
            switch_to(ok_b);
        }
        return pos;
    }
    if (at.attr == "startswith" || at.attr == "endswith") {
        if (!arg_count(1, 1)) return const_none();
        VReg prefix = need_str(0);
        return emit_op(at.attr == "startswith" ? StrOp::StartsWith : StrOp::EndsWith,
                       t_bool(), prefix, kNoReg, kNoReg);
    }
    if (at.attr == "count") {
        if (!arg_count(1, 1)) return const_none();
        VReg sub = need_str(0);
        return emit_op(StrOp::Count, t_int(), sub, kNoReg, kNoReg);
    }
    if (at.attr == "zfill") {
        if (!arg_count(1, 1)) return const_none();
        VReg width = need_int(0, 0);
        return emit_op(StrOp::ZFill, t_str(), width, kNoReg, kNoReg);
    }
    if (at.attr == "ljust" || at.attr == "rjust" || at.attr == "center") {
        if (!arg_count(1, 2)) return const_none();
        VReg width = need_int(0, 0);
        VReg fill = c.args.size() == 2 ? need_str(1) : kNoReg;
        StrOp op = at.attr == "ljust" ? StrOp::LJust
                   : at.attr == "rjust" ? StrOp::RJust : StrOp::Center;
        return emit_op(op, t_str(), width, fill, kNoReg);
    }
    if (at.attr == "removeprefix" || at.attr == "removesuffix") {
        if (!arg_count(1, 1)) return const_none();
        VReg affix = need_str(0);
        return emit_op(at.attr == "removeprefix" ? StrOp::RemovePrefix
                                                  : StrOp::RemoveSuffix,
                       t_str(), affix, kNoReg, kNoReg);
    }
    if (at.attr == "isascii") {
        if (!arg_count(0, 0)) return const_none();
        return emit_op(StrOp::IsAscii, t_bool(), kNoReg, kNoReg, kNoReg);
    }
    if (at.attr == "isdigit" || at.attr == "isalpha" || at.attr == "isspace" ||
        at.attr == "isupper" || at.attr == "islower" || at.attr == "isalnum") {
        if (!arg_count(0, 0)) return const_none();
        StrOp op = at.attr == "isdigit" ? StrOp::IsDigit
                   : at.attr == "isalpha" ? StrOp::IsAlpha
                   : at.attr == "isspace" ? StrOp::IsSpace
                   : at.attr == "isupper" ? StrOp::IsUpper
                   : at.attr == "islower" ? StrOp::IsLower : StrOp::IsAlphaNum;
        return emit_op(op, t_bool(), kNoReg, kNoReg, kNoReg);
    }
    err(c.line, c.col,
        std::format("str method '{}' is not supported by the code generator yet", at.attr));
    return const_none();
}

VReg Lowerer::lower_method_bytes(Attribute& at, Call& c, VReg recv) {
    if (at.attr == "decode") {
        // bytes.decode()：真正的 bytes -> str 转换（ASCII 零复制，非 ASCII 数码点）。
        for (auto& a : c.args) expr(*a);
        VReg v = call_runtime(RtFn::BytesDecode, t_str(), {recv});
        push_temp(v);
        return v;
    }
    // 其余方法在字节层面和 str 完全一致，借道 StrMethod；区别只在结果标成 bytes。
    // 注意：操作码要按方法名选，不能写死——写死成 0（= Upper）会让所有方法都变 upper。
    auto need_bytes = [&](size_t i) -> VReg {
        VReg v = expr(*c.args[i]);
        TypePtr t = tc_.type_of(c.args[i].get());
        if (!t || (t->kind != TyKind::Str && t->kind != TyKind::Bytes))
            err(c.args[i]->line, c.args[i]->col,
                std::format("bytes.{}() expects a string argument", at.attr));
        return v;
    };
    auto need_int = [&](size_t i, int64_t dv) -> VReg {
        if (i >= c.args.size()) return const_int(dv);
        return coerce(expr(*c.args[i]), tc_.type_of(c.args[i].get()), t_int());
    };
    auto arg_count = [&](size_t lo, size_t hi) {
        if (c.args.size() >= lo && c.args.size() <= hi) return true;
        err(c.line, c.col,
            std::format("bytes.{}() takes {} argument(s)", at.attr,
                        lo == hi ? std::to_string(lo)
                                 : std::format("{}..{}", lo, hi)));
        return false;
    };
    auto emit = [&](StrOp op, TypePtr ret, VReg a0, VReg a1, VReg a2) -> VReg {
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::BytesMethod);
        in.args = {recv, const_int(static_cast<int64_t>(op)),
                   a0 == kNoReg ? const_int(0) : a0,
                   a1 == kNoReg ? const_int(0) : a1,
                   a2 == kNoReg ? const_int(0) : a2};
        in.ty = ret;
        VReg v = def(ret, std::move(in));
        if (ir_is_heap(ret)) push_temp(v);
        return v;
    };
    if (at.attr == "upper" || at.attr == "lower") {
        if (!arg_count(0, 0)) return const_none();
        return emit(at.attr == "upper" ? StrOp::Upper : StrOp::Lower, t_bytes(),
                    kNoReg, kNoReg, kNoReg);
    }
    if (at.attr == "strip" || at.attr == "lstrip" || at.attr == "rstrip") {
        if (!arg_count(0, 1)) return const_none();
        bool has_chars = c.args.size() == 1;
        StrOp op;
        if (at.attr == "strip") op = has_chars ? StrOp::StripChars : StrOp::Strip;
        else if (at.attr == "lstrip") op = has_chars ? StrOp::LStripChars : StrOp::LStrip;
        else op = has_chars ? StrOp::RStripChars : StrOp::RStrip;
        return emit(op, t_bytes(), has_chars ? need_bytes(0) : kNoReg, kNoReg, kNoReg);
    }
    if (at.attr == "replace") {
        if (!arg_count(2, 3)) return const_none();
        VReg old_s = need_bytes(0);
        VReg nw = need_bytes(1);
        VReg cnt = need_int(2, -1);
        return emit(StrOp::Replace, t_bytes(), old_s, nw, cnt);
    }
    if (at.attr == "split") {
        if (!arg_count(0, 2)) return const_none();
        TypePtr lt = mk_list(t_bytes());
        if (c.args.empty())
            return emit(StrOp::SplitWs, lt, kNoReg, kNoReg, kNoReg);
        VReg sep = need_bytes(0);
        VReg maxsplit = need_int(1, -1);
        return emit(StrOp::SplitSep, lt, sep, maxsplit, kNoReg);
    }
    if (at.attr == "find" || at.attr == "rfind") {
        if (!arg_count(1, 3)) return const_none();
        VReg sub = need_bytes(0);
        VReg start = need_int(1, 0);
        VReg end = need_int(2, std::numeric_limits<int64_t>::max());
        return emit(at.attr == "find" ? StrOp::Find : StrOp::RFind, t_int(),
                    sub, start, end);
    }
    if (at.attr == "count") {
        if (!arg_count(1, 1)) return const_none();
        VReg sub = need_bytes(0);
        return emit(StrOp::Count, t_int(), sub, kNoReg, kNoReg);
    }
    if (at.attr == "startswith" || at.attr == "endswith") {
        if (!arg_count(1, 1)) return const_none();
        VReg prefix = need_bytes(0);
        return emit(at.attr == "startswith" ? StrOp::StartsWith : StrOp::EndsWith,
                    t_bool(), prefix, kNoReg, kNoReg);
    }
    if (at.attr == "join") {
        if (!arg_count(1, 1)) return const_none();
        TypePtr lt = tc_.type_of(c.args[0].get());
        VReg parts;
        if (lt && lt->kind == TyKind::Generator) {
            // 生成器没有下标：先物化成列表再拼
            TypePtr se = t_unknown();
            parts = seq_as_list(c.args[0].get(), se);
        } else if (!lt || (lt->kind != TyKind::List && lt->kind != TyKind::Tuple)) {
            err(c.args[0]->line, c.args[0]->col,
                "bytes.join() expects a list of bytes");
            return const_none();
        } else {
            parts = expr(*c.args[0]);
        }
        return emit(StrOp::Join, t_bytes(), parts, kNoReg, kNoReg);
    }
    err(c.line, c.col, std::format("bytes method '{}' is not supported", at.attr));
    return const_none();
}

}  // namespace pyadamas
