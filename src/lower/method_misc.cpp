// lower/method_misc.cpp —— random / re / file / int 的方法调用降级
#include "lower/impl.h"

namespace pyadamas {

VReg Lowerer::lower_method_random(Attribute& at, Call& c, VReg recv) {
    if (!c.keywords.empty()) {
        err(c.line, c.col,
            std::format("random.Random.{}() does not take keyword arguments", at.attr));
        return const_none();
    }
    auto int_at = [&](size_t i) -> VReg {
        return coerce(expr(*c.args[i]), tc_.type_of(c.args[i].get()), t_int());
    };
    auto float_at = [&](size_t i) -> VReg {
        return coerce(expr(*c.args[i]), tc_.type_of(c.args[i].get()), t_float());
    };
    if (at.attr == "random") {
        if (!c.args.empty()) {
            err(c.line, c.col, "random() takes no arguments");
            return const_none();
        }
        return call_runtime(RtFn::RandNext, t_float(), {recv});
    }
    if (at.attr == "seed") {
        if (c.args.size() > 1) {
            err(c.line, c.col, "seed() takes at most one argument");
            return const_none();
        }
        VReg seed = const_int(-9223372036854775807LL - 1);
        if (!c.args.empty()) seed = int_at(0);
        call_void(RtFn::RandSeed, {recv, seed});
        return const_none();
    }
    if (at.attr == "randint") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "randint() takes exactly two arguments");
            return const_none();
        }
        VReg a = int_at(0);
        VReg b = int_at(1);
        return call_runtime(RtFn::RandRandint, t_int(), {recv, a, b});
    }
    if (at.attr == "uniform") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "uniform() takes exactly two arguments");
            return const_none();
        }
        VReg a = float_at(0);
        VReg b = float_at(1);
        return call_runtime(RtFn::RandUniform, t_float(), {recv, a, b});
    }
    if (at.attr == "choice") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "choice() takes exactly one argument");
            return const_none();
        }
        TypePtr st = tc_.type_of(c.args[0].get());
        VReg seq = expr(*c.args[0]);
        TypePtr elem = tc_.type_of(&c);
        if (st && st->kind == TyKind::Str) {
            seq = call_runtime(RtFn::StrToList, mk_list(t_str()), {seq});
            push_temp(seq);
            elem = t_str();
        }
        VReg v = call_runtime(RtFn::RandChoice, elem ? elem : t_unknown(), {recv, seq});
        if (ir_is_heap(elem)) {
            rc_inc(v);
            push_temp(v);
        }
        return v;
    }
    if (at.attr == "shuffle") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "shuffle() takes exactly one argument");
            return const_none();
        }
        VReg l = expr(*c.args[0]);
        call_void(RtFn::RandShuffle, {recv, l});
        return const_none();
    }
    err(c.line, c.col,
        std::format("random.Random method '{}' is not supported", at.attr));
    return const_none();
}

VReg Lowerer::lower_method_regex(Attribute& at, Call& c, VReg recv) {
    if (!c.keywords.empty()) {
        err(c.line, c.col,
            std::format("re.Pattern.{}() does not take keyword arguments", at.attr));
        return const_none();
    }
    auto int_at = [&](size_t i) -> VReg {
        return coerce(expr(*c.args[i]), tc_.type_of(c.args[i].get()), t_int());
    };
    if (at.attr == "search" || at.attr == "match" || at.attr == "fullmatch") {
        if (c.args.size() != 1) {
            err(c.line, c.col,
                std::format("{}() takes exactly one argument", at.attr));
            return const_none();
        }
        int64_t mode = at.attr == "search" ? 0 : at.attr == "match" ? 1 : 2;
        VReg text = expr(*c.args[0]);
        TypePtr mt = mk_optional(t_match());
        VReg m = call_runtime(RtFn::RegexSearch, mt, {recv, text, const_int(mode)});
        if (ir_is_heap(mt)) push_temp(m);
        return m;
    }
    if (at.attr == "findall") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "findall() takes exactly one argument");
            return const_none();
        }
        VReg text = expr(*c.args[0]);
        VReg l = call_runtime(RtFn::RegexFindAll, mk_list(t_str()), {recv, text});
        push_temp(l);
        return l;
    }
    if (at.attr == "finditer") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "finditer() takes exactly one argument");
            return const_none();
        }
        VReg text = expr(*c.args[0]);
        VReg l = call_runtime(RtFn::RegexFindIter, mk_list(t_match()), {recv, text});
        push_temp(l);
        return l;
    }
    if (at.attr == "sub") {
        if (c.args.size() < 2 || c.args.size() > 3) {
            err(c.line, c.col, "sub() takes two or three arguments");
            return const_none();
        }
        TypePtr rty0 = tc_.type_of(c.args[0].get());
        VReg text = expr(*c.args[1]);
        VReg count = c.args.size() == 3 ? int_at(2) : const_int(0);
        VReg s;
        if (rty0 && rty0->kind == TyKind::Func) {
            // repl 是函数：运行时每匹配一次调一次闭包。
            VReg fnv = expr(*c.args[0]);
            s = call_runtime(RtFn::RegexSubFn, t_str(), {recv, fnv, text, count});
        } else {
            VReg repl = expr(*c.args[0]);
            s = call_runtime(RtFn::RegexSub, t_str(), {recv, repl, text, count});
        }
        push_temp(s);
        return s;
    }
    if (at.attr == "split") {
        if (c.args.size() < 1 || c.args.size() > 2) {
            err(c.line, c.col, "split() takes one or two arguments");
            return const_none();
        }
        VReg text = expr(*c.args[0]);
        VReg maxsplit = c.args.size() == 2 ? int_at(1) : const_int(0);
        VReg l = call_runtime(RtFn::RegexSplit, mk_list(t_str()),
                              {recv, text, maxsplit});
        push_temp(l);
        return l;
    }
    err(c.line, c.col,
        std::format("re.Pattern method '{}' is not supported", at.attr));
    return const_none();
}

VReg Lowerer::lower_method_match(Attribute& at, Call& c, VReg recv) {
    if (!c.keywords.empty()) {
        err(c.line, c.col,
            std::format("re.Match.{}() does not take keyword arguments", at.attr));
        return const_none();
    }
    auto int_at = [&](size_t i) -> VReg {
        return coerce(expr(*c.args[i]), tc_.type_of(c.args[i].get()), t_int());
    };
    if (at.attr == "group") {
        if (c.args.size() > 1) {
            // group(1, 2, ...) 返回元组；先按 list 建，再改标成 tuple。
            VReg l = call_runtime(RtFn::ListNew, mk_list(t_str()),
                                  {const_int(static_cast<int64_t>(c.args.size())),
                                   const_int(1)});
            push_temp(l);
            for (size_t i = 0; i < c.args.size(); ++i) {
                TypePtr a0 = tc_.type_of(c.args[i].get());
                VReg s;
                if (a0 && a0->kind == TyKind::Str) {
                    VReg sv = expr(*c.args[i]);
                    s = call_runtime(RtFn::RegexGroupStr, t_str(), {recv, sv});
                } else {
                    VReg n = int_at(i);
                    s = call_runtime(RtFn::RegexGroup, t_str(), {recv, n});
                }
                call_void(RtFn::ListAppend, {l, s, const_int(1)});
            }
            return retype(l, mk_tuple_rest(t_str()));
        }
        if (c.args.empty()) {
            VReg s = call_runtime(RtFn::RegexGroup, t_str(), {recv, const_int(0)});
            push_temp(s);
            return s;
        }
        TypePtr a0 = tc_.type_of(c.args[0].get());
        if (a0 && a0->kind == TyKind::Str) {
            VReg sv = expr(*c.args[0]);
            VReg s = call_runtime(RtFn::RegexGroupStr, t_str(), {recv, sv});
            push_temp(s);
            return s;
        }
        VReg n = int_at(0);
        VReg s = call_runtime(RtFn::RegexGroup, t_str(), {recv, n});
        push_temp(s);
        return s;
    }
    if (at.attr == "expand") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "expand() takes exactly one argument");
            return const_none();
        }
        VReg t = expr(*c.args[0]);
        VReg s = call_runtime(RtFn::RegexExpand, t_str(), {recv, t});
        push_temp(s);
        return s;
    }
    if (at.attr == "groups") {
        if (!c.args.empty()) {
            err(c.line, c.col, "groups() takes no arguments");
            return const_none();
        }
        VReg l = call_runtime(RtFn::RegexGroups, mk_list(t_str()), {recv});
        push_temp(l);
        return l;
    }
    if (at.attr == "groupdict") {
        if (!c.args.empty()) {
            err(c.line, c.col, "groupdict() takes no arguments");
            return const_none();
        }
        VReg d = call_runtime(RtFn::RegexGroupDict, mk_dict(t_str(), t_str()), {recv});
        push_temp(d);
        return d;
    }
    if (at.attr == "start" || at.attr == "end") {
        if (c.args.size() > 1) {
            err(c.line, c.col,
                std::format("{}() takes at most one argument", at.attr));
            return const_none();
        }
        VReg n = c.args.empty() ? const_int(0) : int_at(0);
        return call_runtime(at.attr == "start" ? RtFn::RegexStart : RtFn::RegexEnd,
                            t_int(), {recv, n});
    }
    if (at.attr == "span") {
        if (c.args.size() > 1) {
            err(c.line, c.col, "span() takes at most one argument");
            return const_none();
        }
        VReg n = c.args.empty() ? const_int(0) : int_at(0);
        VReg t = call_runtime(RtFn::RegexSpan, mk_tuple({t_int(), t_int()}),
                              {recv, n});
        push_temp(t);
        return t;
    }
    err(c.line, c.col, std::format("re.Match method '{}' is not supported", at.attr));
    return const_none();
}

VReg Lowerer::lower_method_file(Attribute& at, Call& c, VReg recv) {
    if (!c.keywords.empty()) {
        err(c.line, c.col,
            std::format("file.{}() does not take keyword arguments", at.attr));
        return const_none();
    }
    auto int_arg = [&](size_t i, int64_t def) -> VReg {
        if (i >= c.args.size()) return const_int(def);
        VReg v = expr(*c.args[i]);
        return coerce(v, tc_.type_of(c.args[i].get()), t_int());
    };
    if (at.attr == "read") {
        if (c.args.size() > 1) {
            err(c.line, c.col, "file.read() takes at most one argument");
            return const_none();
        }
        VReg s = call_runtime(RtFn::FileRead, t_str(), {recv, int_arg(0, -1)});
        push_temp(s);
        return s;
    }
    if (at.attr == "readline") {
        if (c.args.size() > 1) {
            err(c.line, c.col, "file.readline() takes at most one argument");
            return const_none();
        }
        VReg s = call_runtime(RtFn::FileReadLine, t_str(), {recv, int_arg(0, -1)});
        push_temp(s);
        return s;
    }
    if (at.attr == "readlines") {
        if (c.args.size() > 1) {
            err(c.line, c.col, "file.readlines() takes at most one argument");
            return const_none();
        }
        VReg l = call_runtime(RtFn::FileReadLines, mk_list(t_str()),
                              {recv, int_arg(0, -1)});
        push_temp(l);
        return l;
    }
    if (at.attr == "write") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "file.write() takes exactly one argument");
            return const_none();
        }
        TypePtr wt = tc_.type_of(c.args[0].get());
        VReg v = expr(*c.args[0]);
        if (wt && wt->kind != TyKind::Str && wt->kind != TyKind::Unknown) {
            err(c.args[0]->line, c.args[0]->col, "file.write() expects a string");
            return const_int(0);
        }
        return call_runtime(RtFn::FileWrite, t_int(), {recv, v});
    }
    if (at.attr == "writelines") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "file.writelines() takes exactly one argument");
            return const_none();
        }
        TypePtr lt = tc_.type_of(c.args[0].get());
        if (!lt || (lt->kind != TyKind::List && lt->kind != TyKind::Tuple)) {
            err(c.args[0]->line, c.args[0]->col,
                "file.writelines() expects a list of strings");
            return const_none();
        }
        VReg l = expr(*c.args[0]);
        call_void(RtFn::FileWritelines, {recv, l});
        return const_none();
    }
    if (at.attr == "seek") {
        if (c.args.empty() || c.args.size() > 2) {
            err(c.line, c.col, "file.seek() takes 1 or 2 arguments");
            return const_none();
        }
        VReg off = int_arg(0, 0);
        VReg wh = int_arg(1, 0);
        return call_runtime(RtFn::FileSeek, t_int(), {recv, off, wh});
    }
    if (at.attr == "tell" || at.attr == "fileno") {
        if (!c.args.empty()) {
            err(c.line, c.col, std::format("file.{}() takes no arguments", at.attr));
            return const_none();
        }
        return call_runtime(at.attr == "tell" ? RtFn::FileTell : RtFn::FileFileno,
                            t_int(), {recv});
    }
    if (at.attr == "truncate") {
        if (c.args.size() > 1) {
            err(c.line, c.col, "file.truncate() takes at most one argument");
            return const_none();
        }
        return call_runtime(RtFn::FileTruncate, t_int(), {recv, int_arg(0, -1)});
    }
    if (at.attr == "isatty" || at.attr == "readable" || at.attr == "writable" ||
        at.attr == "seekable") {
        if (!c.args.empty()) {
            err(c.line, c.col, std::format("file.{}() takes no arguments", at.attr));
            return const_none();
        }
        RtFn fn = at.attr == "isatty"     ? RtFn::FileIsatty
                  : at.attr == "readable" ? RtFn::FileReadable
                  : at.attr == "writable" ? RtFn::FileWritable
                                          : RtFn::FileSeekable;
        return call_runtime(fn, t_bool(), {recv});
    }
    if (at.attr == "close") {
        if (!c.args.empty()) {
            err(c.line, c.col, "file.close() takes no arguments");
            return const_none();
        }
        call_void(RtFn::FileClose, {recv});
        return const_none();
    }
    if (at.attr == "flush") {
        if (!c.args.empty()) {
            err(c.line, c.col, "file.flush() takes no arguments");
            return const_none();
        }
        call_void(RtFn::FileFlush, {recv});
        return const_none();
    }
    err(c.line, c.col,
        std::format("file method '{}' is not supported (use 'with open(...)')", at.attr));
    return const_none();
}

VReg Lowerer::lower_method_int(Call& c, VReg recv) {
    if (!c.args.empty()) {
        err(c.line, c.col, "bit_length() takes no arguments");
        return const_none();
    }
    return call_runtime(RtFn::IntBitLength, t_int(), {recv});
}

}  // namespace pyadamas
