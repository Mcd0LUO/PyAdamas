// lower_import_native.cpp —— 标准库原生调用：random/popen/itertools
#include "lower_impl.h"

namespace pyadamas {

VReg Lowerer::lower_random_extra(const std::string& member, Call& c) {
    // choices 的 k 是关键字参数，其它几个不收关键字。
    if (!c.keywords.empty() && member != "choices") {
        err(c.line, c.col, std::format("random.{}() does not take keyword arguments", member));
    }
    auto iarg = [&](size_t i) {
        return coerce(expr(*c.args[i]), tc_.type_of(c.args[i].get()), t_int());
    };
    auto farg = [&](size_t i) {
        return coerce(expr(*c.args[i]), tc_.type_of(c.args[i].get()), t_float());
    };
    if (member == "randrange") {
        if (c.args.size() < 1 || c.args.size() > 3) {
            err(c.line, c.col,
                std::format("random.randrange() takes 1 to 3 arguments, got {}", c.args.size()));
            return const_none();
        }
        VReg a = const_int(0);
        VReg b = const_int(0);
        VReg step = const_int(1);
        if (c.args.size() == 1) {
            b = iarg(0);
        } else {
            a = iarg(0);
            b = iarg(1);
            if (c.args.size() == 3) step = iarg(2);
        }
        return call_runtime(RtFn::RandRange, t_int(), {const_int(0), a, b, step});
    }
    if (member == "getrandbits") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "random.getrandbits() takes exactly one argument");
            return const_none();
        }
        VReg k = iarg(0);
        return call_runtime(RtFn::RandGetrandbits, t_int(), {const_int(0), k});
    }
    if (member == "gauss") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "random.gauss() takes exactly two arguments");
            return const_none();
        }
        VReg mu = farg(0);
        VReg sigma = farg(1);
        return call_runtime(RtFn::RandGauss, t_float(), {const_int(0), mu, sigma});
    }
    if (member == "expovariate") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "random.expovariate() takes exactly one argument");
            return const_none();
        }
        VReg lambd = farg(0);
        return call_runtime(RtFn::RandExpovariate, t_float(), {const_int(0), lambd});
    }
    if (member == "triangular") {
        if (c.args.size() < 2 || c.args.size() > 3) {
            err(c.line, c.col,
                std::format("random.triangular() takes 2 or 3 arguments, got {}", c.args.size()));
            return const_none();
        }
        VReg lo = farg(0);
        VReg hi = farg(1);
        VReg mode;
        if (c.args.size() == 3) {
            mode = farg(2);
        } else {
            // 没给 mode 时取中点 (low + high) / 2
            Inst add;
            add.op = IrOp::Bin;
            add.bop = BinKind::Add;
            add.a = lo;
            add.b = hi;
            VReg sum = def(t_float(), std::move(add));
            Inst div;
            div.op = IrOp::Bin;
            div.bop = BinKind::Div;
            div.a = sum;
            div.b = const_float(2.0);
            mode = def(t_float(), std::move(div));
        }
        return call_runtime(RtFn::RandTriangular, t_float(), {const_int(0), lo, hi, mode});
    }
    if (member == "sample" || member == "choices") {
        // sample 用位置参数 (seq, k)；choices 跟 CPython 一样用 k= 关键字。
        size_t want = member == "sample" ? 2 : 1;
        if (c.args.size() != want) {
            err(c.line, c.col,
                std::format("random.{}() takes {} positional argument{}, got {}", member, want,
                            want == 1 ? "" : "s", c.args.size()));
            return const_none();
        }
        VReg k;
        if (member == "sample") {
            k = iarg(1);
        } else if (!c.keywords.empty()) {
            if (c.keywords[0].arg != "k") {
                err(c.line, c.col, "random.choices() only takes the k= keyword");
                return const_none();
            }
            k = coerce(expr(*c.keywords[0].value),
                       tc_.type_of(c.keywords[0].value.get()), t_int());
        } else {
            k = const_int(1);
        }
        TypePtr st = tc_.type_of(c.args[0].get());
        TypePtr et = t_unknown();
        VReg seq;
        if (st && st->kind == TyKind::Str) {
            seq = call_runtime(RtFn::StrToList, mk_list(t_str()), {expr(*c.args[0])});
            push_temp(seq);
            et = t_str();
        } else {
            seq = expr(*c.args[0]);
            if (st && st->kind == TyKind::List) {
                if (!st->sub.empty()) et = st->sub[0];
            } else if (st && st->kind == TyKind::Tuple) {
                if (!st->sub.empty()) {
                    et = st->sub[0];
                    for (size_t i = 1; i < st->sub.size(); ++i) et = join(et, st->sub[i]);
                }
            }
        }
        TypePtr rt = tc_.type_of(&c);
        if (!rt || rt->kind != TyKind::List) rt = mk_list(et);
        VReg v = call_runtime(member == "sample" ? RtFn::RandSample : RtFn::RandChoices, rt,
                              {const_int(0), seq, k});
        push_temp(v);
        return v;
    }
    err(c.line, c.col, std::format("random.{} is not supported", member));
    return const_none();
}

/* stdin/stdout/stderr 的取值：None → 0（继承），PIPE/STDOUT/DEVNULL 是模块
   常量，已经降级成 -1/-2/-3 的整数常量，这里直接取整数值。 */
VReg Lowerer::stream_mode(Expr& e) {
    VReg v = expr(e);
    TypePtr t = tc_.type_of(&e);
    if (t && t->kind == TyKind::None) return const_int(0);
    return coerce(v, t, t_int());
}

VReg Lowerer::lower_popen(Call& c) {
    if (c.args.size() != 1) {
        err(c.line, c.col,
            std::format("subprocess.Popen() takes exactly one positional argument, got {}",
                        c.args.size()));
        return const_none();
    }
    TypePtr at = tc_.type_of(c.args[0].get());
    VReg argv;
    if (at && at->kind == TyKind::Str) {
        // Popen("cmd", shell=True)：字符串当单条命令，包成单元素列表。
        VReg s = expr(*c.args[0]);
        argv = call_runtime(RtFn::StrWrapList, mk_list(t_str()), {s});
        push_temp(argv);
    } else {
        if (at && at->kind != TyKind::List && at->kind != TyKind::Unknown)
            err(c.args[0]->line, c.args[0]->col,
                std::format("subprocess.Popen() expects a list of str or a str, got '{}'",
                            type_name(at)));
        argv = expr(*c.args[0]);
    }

    VReg shell = const_bool(false);
    VReg cwd = const_str("");
    VReg in_mode = const_int(0);
    VReg out_mode = const_int(0);
    VReg err_mode = const_int(0);
    for (auto& kw : c.keywords) {
        if (kw.arg == "shell") {
            shell = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_bool());
        } else if (kw.arg == "cwd") {
            TypePtr vt = tc_.type_of(kw.value.get());
            VReg v = expr(*kw.value);
            cwd = (vt && vt->kind == TyKind::None) ? const_str("") : coerce(v, vt, t_str());
        } else if (kw.arg == "stdin") {
            in_mode = stream_mode(*kw.value);
        } else if (kw.arg == "stdout") {
            out_mode = stream_mode(*kw.value);
        } else if (kw.arg == "stderr") {
            err_mode = stream_mode(*kw.value);
        } else if (kw.arg == "text") {
            // PyAdamas 的 subprocess 始终返回文本：text 只做求值，不影响行为。
            expr(*kw.value);
        } else {
            err(kw.value->line, kw.value->col,
                std::format("subprocess.Popen() got an unexpected keyword argument '{}'",
                            kw.arg));
        }
    }

    TypePtr rt = new_ty(TyKind::Popen);
    VReg v = call_runtime(RtFn::PopenNew, rt,
                          {argv, shell, cwd, in_mode, out_mode, err_mode});
    push_temp(v);
    return v;
}

/* subprocess.run("cmd", shell=True)：第一个位置参数是字符串时，包成单元素
   列表再走和列表一样的签名。列表参数走通用路径（bind_args）。 */
VReg Lowerer::lower_subprocess_run(Call& c) {
    VReg s = expr(*c.args[0]);
    VReg argv = call_runtime(RtFn::StrWrapList, mk_list(t_str()), {s});
    push_temp(argv);

    VReg capture = const_bool(false);
    VReg text = const_bool(false);
    VReg timeout = const_float(-1.0);
    VReg check = const_bool(false);
    VReg shell = const_bool(false);
    VReg cwd = const_str("");
    if (c.args.size() > 1)
        err(c.args[1]->line, c.args[1]->col,
            "subprocess.run() 的其余参数要用关键字传（capture_output= / shell= 等）");
    for (auto& kw : c.keywords) {
        if (kw.arg == "capture_output") {
            capture = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_bool());
        } else if (kw.arg == "text") {
            text = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_bool());
        } else if (kw.arg == "timeout") {
            timeout = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_float());
        } else if (kw.arg == "check") {
            check = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_bool());
        } else if (kw.arg == "shell") {
            shell = coerce(expr(*kw.value), tc_.type_of(kw.value.get()), t_bool());
        } else if (kw.arg == "cwd") {
            TypePtr vt = tc_.type_of(kw.value.get());
            VReg v = expr(*kw.value);
            cwd = (vt && vt->kind == TyKind::None) ? const_str("") : coerce(v, vt, t_str());
        } else {
            err(kw.value->line, kw.value->col,
                std::format("subprocess.run() got an unexpected keyword argument '{}'",
                            kw.arg));
        }
    }

    TypePtr rt = tc_.type_of(&c);
    if (!rt || rt->kind != TyKind::CompletedProcess) rt = t_completed();
    VReg v = call_runtime(RtFn::SubprocessRun, rt,
                          {argv, capture, text, timeout, check, shell, cwd});
    push_temp(v);
    return v;
}

VReg Lowerer::call_native(const std::string& module, const std::string& member, Call& c,
                          const FuncInfoPtr& info) {
    if (module == "subprocess" && member == "Popen") return lower_popen(c);
    if (module == "subprocess" && member == "run" && !c.args.empty()) {
        TypePtr at = tc_.type_of(c.args[0].get());
        if (at && at->kind == TyKind::Str) return lower_subprocess_run(c);
    }
    auto fn = native_fn(module, member);
    if (!fn) {
        err(c.line, c.col, std::format("{}.{} is not a supported native function", module, member));
        return const_none();
    }
    // random 里带 rng 首参的几个函数：参数在 lower_random_extra 里手工装配。
    if (module == "random") {
        if (member == "randrange" || member == "getrandbits" || member == "gauss" ||
            member == "expovariate" || member == "triangular" || member == "sample" ||
            member == "choices") {
            return lower_random_extra(member, c);
        }
    }
    // os.makedirs(name, exist_ok=False)：exist_ok 可选，参数在这里手工装配。
    if (module == "os" && member == "makedirs") {
        if (c.args.empty() || c.args.size() > 2) {
            err(c.line, c.col,
                std::format("os.makedirs() takes 1 or 2 arguments, got {}", c.args.size()));
            return const_none();
        }
        VReg p = expr(*c.args[0]);
        p = coerce(p, tc_.type_of(c.args[0].get()), t_str());
        VReg ok = const_bool(false);
        if (c.args.size() == 2)
            ok = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_bool());
        call_void(RtFn::OsMakedirs, {p, ok});
        return const_none();
    }
    // random.choice 返回序列元素类型，FuncInfo 的静态签名表达不了；
    // 结果类型以类型检查阶段记录在 Call 上的类型为准。
    if (module == "random" && member == "choice") {        if (c.args.size() != 1 || !c.keywords.empty()) {
            err(c.line, c.col, "random.choice() expects 1 positional argument");
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
        // 运行时返回借用元素；当元素是堆对象时补一个引用进临时栈。
        VReg v = call_runtime(*fn, elem ? elem : t_unknown(), {seq});
        if (ir_is_heap(elem)) {
            rc_inc(v);
            push_temp(v);
        }
        return v;
    }
    std::vector<VReg> args = bind_args(c, info, nullptr);
    if (module == "re" && (member == "search" || member == "match" || member == "fullmatch")) {
        int64_t mode = member == "search" ? 0 : member == "match" ? 1 : 2;
        args.push_back(const_int(mode));
        TypePtr rt = tc_.type_of(&c);
        if (!rt || rt->kind == TyKind::Unknown) rt = mk_optional(t_match());
        VReg v = call_runtime(RtFn::RegexSearchStr, rt, std::move(args));
        if (ir_is_heap(rt)) push_temp(v);
        return v;
    }
    if (*fn == RtFn::SysExit) {
        call_void(*fn, args);
        return const_none();
    }
    TypePtr rt = tc_.type_of(&c);
    if (!rt || rt->kind == TyKind::Unknown)
        rt = info && info->ret ? info->ret : t_float();
    VReg v = call_runtime(*fn, rt, std::move(args));
    if (ir_is_heap(rt)) push_temp(v);
    return v;
}

VReg Lowerer::lower_itertools(const std::string& member, Call& c) {
    if (!c.keywords.empty()) {
        err(c.line, c.col,
            std::format("itertools.{}() does not take keyword arguments", member));
    }
    // 把若干个列表实参装进一个临时 kind 0 列表，交给运行时的变参辅助函数。
    // kind 0 表示不持有元素引用：临时列表销毁时不会去 dec 源列表。
    auto pack_seqs = [&](size_t from, size_t to) {
        VReg seqs = call_runtime(RtFn::ListNew, mk_list(t_unknown()),
                                 {const_int(0), const_int(0)});
        push_temp(seqs);
        for (size_t i = from; i < to; ++i) {
            VReg v = expr(*c.args[i]);
            call_void(RtFn::ListAppend, {seqs, v});
        }
        return seqs;
    };
    // 结果列表的元素是元组：外层列表类别固定为 1，这里算的是元组内部的
    // 元素类别（list_kind 对 Tuple 类型会按各字段合并）。
    auto tuple_elem_kind = [&](const TypePtr& rt) {
        if (rt && rt->kind == TyKind::List && !rt->sub.empty())
            return list_kind_filled(rt->sub[0]);
        return 2;
    };
    if (member == "chain") {
        if (c.args.size() != 2) {
            err(c.line, c.col,
                std::format("itertools.chain() expects 2 arguments, got {}", c.args.size()));
            return const_none();
        }
        TypePtr rt = tc_.type_of(&c);
        VReg a = expr(*c.args[0]);
        VReg b = expr(*c.args[1]);
        VReg v = call_runtime(RtFn::IterChain, rt, {a, b, const_int(list_kind_filled(rt))});
        push_temp(v);
        return v;
    }
    if (member == "islice") {
        if (c.args.size() != 2 && c.args.size() != 3) {
            err(c.line, c.col,
                std::format("itertools.islice() expects 2 or 3 arguments, got {}",
                            c.args.size()));
            return const_none();
        }
        VReg x = expr(*c.args[0]);
        VReg start = const_int(0);
        VReg stop = const_int(0);
        if (c.args.size() == 2) {
            stop = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int());
        } else {
            start = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int());
            stop = coerce(expr(*c.args[2]), tc_.type_of(c.args[2].get()), t_int());
        }
        TypePtr rt = tc_.type_of(&c);
        VReg v = call_runtime(RtFn::ListSlice, rt, {x, start, stop, const_int(1)});
        push_temp(v);
        return v;
    }
    if (member == "repeat") {
        if (c.args.size() != 2) {
            err(c.line, c.col,
                std::format("itertools.repeat() expects 2 arguments, got {}", c.args.size()));
            return const_none();
        }
        TypePtr rt = tc_.type_of(&c);
        VReg v = expr(*c.args[0]);
        VReg n = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int());
        VReg out = call_runtime(RtFn::IterRepeat, rt, {v, n, const_int(list_kind_filled(rt))});
        push_temp(out);
        return out;
    }
    if (member == "product") {
        TypePtr rt = tc_.type_of(&c);
        VReg seqs = pack_seqs(0, c.args.size());
        VReg out = call_runtime(RtFn::IterProduct, rt,
                                {seqs, const_int(tuple_elem_kind(rt))});
        push_temp(out);
        return out;
    }
    if (member == "permutations" || member == "combinations" ||
        member == "combinations_with_replacement") {
        bool ok = member == "permutations" ? (c.args.size() == 1 || c.args.size() == 2)
                                           : c.args.size() == 2;
        if (!ok) {
            err(c.line, c.col,
                std::format("itertools.{}() expects {} arguments, got {}", member,
                            member == "permutations" ? "1 or 2" : "2", c.args.size()));
            return const_none();
        }
        TypePtr rt = tc_.type_of(&c);
        VReg pool = expr(*c.args[0]);
        VReg r = const_int(-1);
        if (c.args.size() > 1)
            r = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int());
        RtFn fn = member == "permutations"          ? RtFn::IterPermutations
                  : member == "combinations"        ? RtFn::IterCombinations
                                                    : RtFn::IterCombinationsWR;
        VReg out = call_runtime(fn, rt, {pool, r, const_int(tuple_elem_kind(rt))});
        push_temp(out);
        return out;
    }
    if (member == "zip_longest") {
        if (c.args.empty()) {
            err(c.line, c.col, "itertools.zip_longest() expects at least 1 argument");
            return const_none();
        }
        TypePtr rt = tc_.type_of(&c);
        VReg seqs = pack_seqs(0, c.args.size());
        VReg out = call_runtime(RtFn::IterZipLongest, rt,
                                {seqs, const_int(tuple_elem_kind(rt))});
        push_temp(out);
        return out;
    }
    if (member == "accumulate") {
        if (c.args.size() != 1) {
            err(c.line, c.col,
                std::format("itertools.accumulate() expects 1 argument, got {}",
                            c.args.size()));
            return const_none();
        }
        TypePtr rt = tc_.type_of(&c);
        TypePtr et = rt && rt->kind == TyKind::List && !rt->sub.empty() ? rt->sub[0]
                                                                        : t_unknown();
        VReg xs = expr(*c.args[0]);
        VReg out = call_runtime(RtFn::IterAccumulate, rt,
                                {xs, const_int(et && et->kind == TyKind::Float ? 1 : 0)});
        push_temp(out);
        return out;
    }
    err(c.line, c.col, std::format("itertools.{} is not supported", member));
    return const_none();
}

VReg Lowerer::build_itemgetter(Call& c, TypePtr fty) {
    std::vector<VReg> keys;
    std::vector<TypePtr> keytys;
    keys.reserve(c.args.size());
    keytys.reserve(c.args.size());
    for (auto& a : c.args) {
        TypePtr kt = tc_.type_of(a.get());
        keys.push_back(expr(*a));
        keytys.push_back(kt ? kt : t_unknown());
    }
    int nkey = static_cast<int>(keys.size());
    if (nkey == 0) {
        err(c.line, c.col, "operator.itemgetter() expects at least 1 argument");
        return const_none();
    }

    // 合成 getter 函数：obj 是 unknown 参数，key 从闭包 env 的 cell 里读。
    std::string label = std::format("__pa_itemgetter_{}", mod_.funcs.size());
    IRFunction* wf = mod_.add_func(label);
    int widx = static_cast<int>(mod_.funcs.size()) - 1;
    wf->ret = t_unknown();
    push_frame(wf, nullptr);
    int entry = new_block("entry");
    switch_to(entry);
    st_->fn_mark = rc_mark_at(st_->fn_mark_block, st_->fn_mark_pos);
    int oslot = wf->new_slot(t_unknown());
    wf->params.push_back(IRParam{"obj", t_unknown(), oslot});
    int es = wf->new_slot(t_unknown());
    st_->env_slot = es;
    wf->env_slot = es;
    st_->borrow_slots.insert(es);
    inc_params();

    VReg env = load_slot(es, t_unknown());
    VReg obj = load_slot(oslot, t_unknown());
    std::vector<VReg> vals;
    vals.reserve(static_cast<size_t>(nkey));
    for (int i = 0; i < nkey; ++i) {
        VReg cell = field_load(env, 16 + 8 * i, mk_list(t_unknown()));
        Inst ik;
        ik.op = IrOp::IndexLoad;
        ik.a = cell;
        ik.b = const_int(0);
        VReg key = def(t_unknown(), std::move(ik));
        Inst ld;
        ld.op = IrOp::IndexLoad;
        ld.a = obj;
        ld.b = key;
        vals.push_back(def(t_unknown(), std::move(ld)));
    }
    VReg out;
    if (nkey == 1) {
        out = vals[0];
    } else {
        // 多键返回元组；元素是容器内部的借用引用，入元组前 retain。
        for (auto& v : vals) retain_elem(v, 2);
        Inst mk;
        mk.op = IrOp::MakeTuple;
        mk.imm = 2;
        mk.args = std::move(vals);
        out = def_heap(t_unknown(), std::move(mk));
    }
    emit_return(out, t_unknown());
    pop_frame();

    Inst fc;
    fc.op = IrOp::FuncCode;
    fc.imm = widx;
    VReg code = def(t_int(), std::move(fc));
    VReg closure = call_runtime(RtFn::ClosureNew, fty, {code, const_int(nkey)});
    push_temp(closure);
    for (int i = 0; i < nkey; ++i) {
        VReg cell = make_cell_value(keys[static_cast<size_t>(i)],
                                    keytys[static_cast<size_t>(i)]);
        rc_inc(cell);
        field_store(closure, 16 + 8 * i, cell);
    }
    return closure;
}


}  // namespace pyadamas
