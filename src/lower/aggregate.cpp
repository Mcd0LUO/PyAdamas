// lower/aggregate.cpp —— min/max/sum/any/all 的降级
#include "lower/impl.h"

namespace pyadamas {


// min()/max()：多实参逐个比较；单实参按容器迭代。空序列抛 ValueError。
VReg Lowerer::lower_minmax(Call& c, bool is_min) {
    if (c.args.empty()) {
        err(c.line, c.col,
            std::format("{}() expects at least one argument", is_min ? "min" : "max"));
        return const_none();
    }
    TypePtr acc_ty = tc_.type_of(&c);
    if (!acc_ty || acc_ty->kind == TyKind::Unknown) acc_ty = t_unknown();
    else if (!is_num(acc_ty) && acc_ty->kind != TyKind::Str) {
        err(c.line, c.col,
            std::format("{}() arguments must be numbers or strings", is_min ? "min" : "max"));
        return const_none();
    }
    int best = new_temp_slot(acc_ty);
    // big 之间的大小关系是运行时函数 pa_big_cmp（返回 -1/0/1），裸 Cmp 的
    // codegen 没有 big 路径，会把 big 指针当整数比。这里统一收口。
    // 统一走 lower_cmp_single（0.633）：它按静态类型分派——big 走 pa_big_cmp、
    // 字符串走内容比较、unknown 走 pa_any_cmp 的动态分派、float 走 SSE 比较。
    // 原来只特判了 big，且生成器路径干脆没用它（`min(generator of big)` 段错误）。
    auto take_new = [&](VReg cand, const TypePtr& ty) -> VReg {
        VReg cur = load_slot(best, ty);
        return lower_cmp_single(cand, ty, cur, ty, is_min ? CmpOp::Lt : CmpOp::Gt, c.line,
                                c.col);
    };

    if (c.args.size() == 1) {
        TypePtr seq_ty = tc_.type_of(c.args[0].get());
        bool ok = seq_ty && (seq_ty->kind == TyKind::List || seq_ty->kind == TyKind::Tuple ||
                             seq_ty->kind == TyKind::Str || seq_ty->kind == TyKind::Bytes ||
                             seq_ty->kind == TyKind::Dict ||
                             seq_ty->kind == TyKind::Set || seq_ty->kind == TyKind::Range ||
                             seq_ty->kind == TyKind::Generator);
        if (!ok) {
            err(c.args[0]->line, c.args[0]->col,
                std::format("cannot iterate over '{}' in {}()", type_name(seq_ty),
                            is_min ? "min" : "max"));
            return const_none();
        }
        TypePtr elem_ty = t_unknown();
        if (seq_ty->kind == TyKind::Str) elem_ty = t_str();
        else if (seq_ty->kind == TyKind::Bytes) elem_ty = t_int();
        else if (!seq_ty->sub.empty()) elem_ty = seq_ty->sub[0];
        // bytes 不能按下标当 list 读：先物化成 list[int] 再走通用路径。
        // str 同理物化成 list[str]：逐下标回调 pa_str_char_at 是 O(n^2)，
        // 一次物化是 O(n)，与今天 list 路径的成本一致。
        VReg seq_conv = kNoReg;
        if (seq_ty->kind == TyKind::Bytes) {
            seq_conv = call_runtime(RtFn::BytesToList, mk_list(t_int()), {expr(*c.args[0])});
            push_temp(seq_conv);
            seq_ty = mk_list(t_int());
            elem_ty = t_int();
        } else if (seq_ty->kind == TyKind::Str) {
            seq_conv = call_runtime(RtFn::StrToList, mk_list(t_str()), {expr(*c.args[0])});
            push_temp(seq_conv);
            seq_ty = mk_list(t_str());
            elem_ty = t_str();
        }

        // range：按边界算，不物化。空 range 同样报 ValueError。
        if (seq_ty->kind == TyKind::Range) {
            VReg seq = expr(*c.args[0]);
            return call_runtime(is_min ? RtFn::RangeMinI64 : RtFn::RangeMaxI64, t_int(),
                                {seq});
        }

        // 生成器：没有长度也没有下标，流式比较，空的时候同样报 ValueError。
        if (seq_ty->kind == TyKind::Generator) {
            // 元素类型未知时无法选比较指令（big 要 pa_big_cmp、float 要 SSE、
            // 字符串要比内容）。之前会退化成裸 Cmp/AnyCmp 的错配：实测
            // min(generator of big) 返回较大值、混合 int/big 段错误。
            // 明确拒绝，别静默错值。
            TypePtr et0 = seq_ty->sub.empty() ? t_unknown() : seq_ty->sub[0];
            // big 元素同样拒绝：生成器路径拿不到稳定的静态元素域，
            // pa_big_cmp 的入参一旦不是 big 指针就会崩（实测段错误）。
            if (!et0 || et0->kind == TyKind::Unknown || et0->kind == TyKind::Big) {
                err(c.args[0]->line, c.args[0]->col,
                    std::format("{}() over a generator requires a known element type",
                                is_min ? "min" : "max"));
                return const_none();
            }
            VReg seq = expr(*c.args[0]);
            int seen = new_temp_slot(t_int());
            store_slot(seen, const_int(0));
            emit_gen_for_each(seq, seq_ty, [&](VReg ev, TypePtr et, int cond, int) {
                // 生成器的结果类型常常推不出（min(generator of big) → acc_ty 是
                // unknown），此时按元素类型比较，否则 big 会退化成指针比较。
                TypePtr cmp_ty = acc_ty;
                if ((!cmp_ty || cmp_ty->kind == TyKind::Unknown) && et &&
                    et->kind != TyKind::Unknown)
                    cmp_ty = et;
                VReg v = coerce(ev, et, cmp_ty);
                Inst c0;
                c0.op = IrOp::Cmp;
                c0.cop = CmpKind::Eq;
                c0.a = load_slot(seen, t_int());
                c0.b = const_int(0);
                VReg is_first = def(t_bool(), std::move(c0));
                int init_b = new_block("minmax.ginit");
                int cmp_b = new_block("minmax.gcmp");
                Inst b0;
                b0.op = IrOp::Branch;
                b0.a = is_first;
                b0.target = init_b;
                b0.target2 = cmp_b;
                emit(std::move(b0));
                switch_to(init_b);
                store_slot(best, v);
                store_slot(seen, const_int(1));
                Inst j0;
                j0.op = IrOp::Jump;
                j0.target = cond;
                emit(std::move(j0));
                switch_to(cmp_b);
                // 与容器/多实参路径同口径：big 必须走 pa_big_cmp，裸 Cmp 比的是
                // 堆指针（0.633，实测 min(generator of big) 段错误）。
                VReg take = take_new(v, cmp_ty);
                int choose = new_block("minmax.gchoose");
                Inst bt;
                bt.op = IrOp::Branch;
                bt.a = take;
                bt.target = choose;
                bt.target2 = cond;
                emit(std::move(bt));
                switch_to(choose);
                store_slot(best, v);
                Inst jc;
                jc.op = IrOp::Jump;
                jc.target = cond;
                emit(std::move(jc));
            });
            int empty_b = new_block("minmax.gempty");
            int done_b = new_block("minmax.gdone");
            Inst ce;
            ce.op = IrOp::Cmp;
            ce.cop = CmpKind::Eq;
            ce.a = load_slot(seen, t_int());
            ce.b = const_int(0);
            VReg is_empty = def(t_bool(), std::move(ce));
            Inst be;
            be.op = IrOp::Branch;
            be.a = is_empty;
            be.target = empty_b;
            be.target2 = done_b;
            emit(std::move(be));
            switch_to(empty_b);
            call_void(raise_rt(RtFn::Raise), {const_str("ValueError"),
                                    const_str(is_min ? "min() iterable argument is empty"
                                                     : "max() iterable argument is empty")});
            end_unreachable();  // Raise 是 noreturn
            switch_to(done_b);
            return load_slot(best, acc_ty);
        }

        // SIMD min/max：int/bool 列表用 AVX2 归约；float 仍走标量以保持
        // NaN 下的比较顺序语义。
        if (opts_.opt_level >= 2 &&
            (seq_ty->kind == TyKind::List || seq_ty->kind == TyKind::Tuple) &&
            (elem_ty->kind == TyKind::Int || elem_ty->kind == TyKind::Bool) &&
            (acc_ty->kind == TyKind::Int || acc_ty->kind == TyKind::Bool)) {
            VReg seq = seq_conv != kNoReg ? seq_conv : expr(*c.args[0]);
            return call_runtime(is_min ? RtFn::ListMinI64 : RtFn::ListMaxI64, acc_ty,
                                {seq});
        }

        VReg seq = seq_conv != kNoReg ? seq_conv : expr(*c.args[0]);
        int seq_slot = new_temp_slot(seq_ty);
        store_slot(seq_slot, seq);
        Inst ln;
        ln.op = IrOp::Len;
        ln.a = seq;
        VReg n = def(t_int(), std::move(ln));
        int n_slot = new_temp_slot(t_int());
        store_slot(n_slot, n);

        int empty_b = new_block("minmax.empty");
        int nonempty_b = new_block("minmax.nonempty");
        Inst ce;
        ce.op = IrOp::Cmp;
        ce.cop = CmpKind::Eq;
        ce.a = n;
        ce.b = const_int(0);
        VReg is_empty = def(t_bool(), std::move(ce));
        Inst be;
        be.op = IrOp::Branch;
        be.a = is_empty;
        be.target = empty_b;
        be.target2 = nonempty_b;
        emit(std::move(be));
        switch_to(empty_b);
        call_void(raise_rt(RtFn::Raise), {const_str("ValueError"),
                                const_str(is_min ? "min() iterable argument is empty"
                                                 : "max() iterable argument is empty")});
        end_unreachable();  // Raise 是 noreturn
        switch_to(nonempty_b);

        auto elem_at = [&](VReg idx) -> VReg {
            VReg sv = load_slot(seq_slot, seq_ty);
            if (seq_ty->kind == TyKind::Dict || seq_ty->kind == TyKind::Set)
                return call_runtime(RtFn::DictKeyAt, elem_ty, {sv, idx});
            Inst get;
            get.op = (opts_.opt_level >= 1 &&
                      (seq_ty->kind == TyKind::List || seq_ty->kind == TyKind::Tuple))
                         ? IrOp::IndexLoadUnchecked
                         : IrOp::IndexLoad;
            get.a = sv;
            get.b = idx;
            return seq_ty->kind == TyKind::Str ? def_heap(elem_ty, std::move(get))
                                               : def(elem_ty, std::move(get));
        };

        VReg first = coerce(elem_at(const_int(0)), elem_ty, acc_ty);
        store_slot(best, first);
        int idx = new_temp_slot(t_int());
        store_slot(idx, const_int(1));

        int cond = new_block("minmax.cond");
        int body = new_block("minmax.body");
        int choose = new_block("minmax.choose");
        int next = new_block("minmax.next");
        int end = new_block("minmax.end");
        Inst j;
        j.op = IrOp::Jump;
        j.target = cond;
        emit(std::move(j));

        switch_to(cond);
        Inst cmp;
        cmp.op = IrOp::Cmp;
        cmp.cop = CmpKind::Lt;
        cmp.a = load_slot(idx, t_int());
        cmp.b = load_slot(n_slot, t_int());
        VReg cc = def(t_bool(), std::move(cmp));
        Inst br;
        br.op = IrOp::Branch;
        br.a = cc;
        br.target = body;
        br.target2 = end;
        emit(std::move(br));

        switch_to(body);
        VReg ev = coerce(elem_at(load_slot(idx, t_int())), elem_ty, acc_ty);
        VReg take = take_new(ev, acc_ty);
        Inst bt;
        bt.op = IrOp::Branch;
        bt.a = take;
        bt.target = choose;
        bt.target2 = next;
        emit(std::move(bt));
        switch_to(choose);
        store_slot(best, ev);
        Inst jc;
        jc.op = IrOp::Jump;
        jc.target = next;
        emit(std::move(jc));

        switch_to(next);
        Inst add;
        add.op = IrOp::Bin;
        add.bop = BinKind::Add;
        add.a = load_slot(idx, t_int());
        add.b = const_int(1);
        store_slot(idx, def(t_int(), std::move(add)));
        Inst jb;
        jb.op = IrOp::Jump;
        jb.target = cond;
        emit(std::move(jb));

        switch_to(end);
        return load_slot(best, acc_ty);
    }

    VReg v0 = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), acc_ty);
    store_slot(best, v0);
    for (size_t i = 1; i < c.args.size(); ++i) {
        VReg v = coerce(expr(*c.args[i]), tc_.type_of(c.args[i].get()), acc_ty);
        VReg take = take_new(v, acc_ty);
        int choose = new_block("minmax.choose");
        int next = new_block("minmax.next");
        Inst br;
        br.op = IrOp::Branch;
        br.a = take;
        br.target = choose;
        br.target2 = next;
        emit(std::move(br));
        switch_to(choose);
        store_slot(best, v);
        Inst j;
        j.op = IrOp::Jump;
        j.target = next;
        emit(std::move(j));
        switch_to(next);
    }
    return load_slot(best, acc_ty);
}

// sum(iterable, start=0)：计数器循环 + 加法，累加器落槽。
VReg Lowerer::lower_sum(Call& c) {
    if (c.args.empty() || c.args.size() > 2) {
        err(c.line, c.col, "sum() takes one or two arguments");
        return const_none();
    }
    TypePtr acc_ty = tc_.type_of(&c);
    if (!acc_ty || acc_ty->kind == TyKind::Unknown) acc_ty = t_unknown();
    else if (!is_num(acc_ty)) {
        err(c.line, c.col, std::format("sum() over non-numeric '{}'", type_name(acc_ty)));
        return const_none();
    }
    TypePtr seq_ty = tc_.type_of(c.args[0].get());
    bool ok = seq_ty && (seq_ty->kind == TyKind::List || seq_ty->kind == TyKind::Tuple ||
                         seq_ty->kind == TyKind::Str || seq_ty->kind == TyKind::Bytes ||
                         seq_ty->kind == TyKind::Dict ||
                         seq_ty->kind == TyKind::Set || seq_ty->kind == TyKind::Range ||
                         seq_ty->kind == TyKind::Generator);
    if (!ok) {
        err(c.args[0]->line, c.args[0]->col,
            std::format("cannot iterate over '{}' in sum()", type_name(seq_ty)));
        return const_none();
    }
    TypePtr elem_ty = t_unknown();
    if (seq_ty->kind == TyKind::Str) elem_ty = t_str();
    else if (seq_ty->kind == TyKind::Bytes) elem_ty = t_int();
    else if (!seq_ty->sub.empty()) elem_ty = seq_ty->sub[0];
    // bytes 不能按下标当 list 读：先物化成 list[int] 再走通用路径。
    VReg seq_conv = kNoReg;
    if (seq_ty->kind == TyKind::Bytes) {
        seq_conv = call_runtime(RtFn::BytesToList, mk_list(t_int()), {expr(*c.args[0])});
        push_temp(seq_conv);
        seq_ty = mk_list(t_int());
        elem_ty = t_int();
    } else if (seq_ty->kind == TyKind::Str) {
        // sum(str) 本来就是类型错误（元素是 str）；仍物化避免 O(n^2) 取元素。
        seq_conv = call_runtime(RtFn::StrToList, mk_list(t_str()), {expr(*c.args[0])});
        push_temp(seq_conv);
        seq_ty = mk_list(t_str());
        elem_ty = t_str();
    }

    // range：按边界流式累加，不物化。
    if (seq_ty->kind == TyKind::Range) {
        VReg seq = expr(*c.args[0]);
        VReg start = acc_ty && acc_ty->kind == TyKind::Float
                      ? const_float(0.0)
                      : (acc_ty && acc_ty->kind == TyKind::Big ? const_big("0")
                                                               : const_int(0));
        if (c.args.size() == 2)
            start = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), acc_ty);
        if (acc_ty && acc_ty->kind == TyKind::Float)
            return call_runtime(RtFn::RangeSumF64, t_float(), {seq, start});
        return call_runtime(RtFn::RangeSumI64, t_int(), {seq, start});
    }

    // 生成器：流式累加，不建中间列表（这正是生成器表达式存在的意义）。
    if (seq_ty->kind == TyKind::Generator) {
        VReg seq = expr(*c.args[0]);
        VReg start = acc_ty && acc_ty->kind == TyKind::Float
                      ? const_float(0.0)
                      : (acc_ty && acc_ty->kind == TyKind::Big ? const_big("0")
                                                               : const_int(0));
        if (c.args.size() == 2)
            start = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), acc_ty);
        int acc = new_temp_slot(acc_ty);
        store_slot(acc, start);
        emit_gen_for_each(seq, seq_ty, [&](VReg ev, TypePtr et, int, int) {
            VReg v = coerce(ev, et, acc_ty);
            if (acc_ty && acc_ty->kind == TyKind::Big) {
                store_slot(acc,
                           call_runtime(RtFn::BigAdd, t_big(), {load_slot(acc, acc_ty), v}));
                return;
            }
            Inst add;
            add.op = IrOp::Bin;
            add.bop = BinKind::Add;
            add.a = load_slot(acc, acc_ty);
            add.b = v;
            store_slot(acc, def(acc_ty, std::move(add)));
        });
        return load_slot(acc, acc_ty);
    }

    // SIMD 归约：只覆盖同域 list/tuple[int|bool] 与 float；混合数值
    // （如 int 列表 + float start）继续走标量循环保持转换语义。
    if ((seq_ty->kind == TyKind::List || seq_ty->kind == TyKind::Tuple) &&
        !seq_ty->sub.empty() && acc_ty) {
        bool ints = (elem_ty->kind == TyKind::Int || elem_ty->kind == TyKind::Bool) &&
                    (acc_ty->kind == TyKind::Int || acc_ty->kind == TyKind::Bool);
        // 浮点：一律走运行时（CPython 3.12+ 的 Neumaier 补偿求和）。原来只有
        // --ffast-math 才走 SIMD 归约，默认落到标量朴素累加，与 CPython 有
        // 1 ulp 甚至量级差（sum([1e16,1,-1e16]) 是 0 而不是 1）。
        bool floats = elem_ty->kind == TyKind::Float && acc_ty->kind == TyKind::Float;
        if (ints || floats) {
            VReg seq = seq_conv != kNoReg ? seq_conv : expr(*c.args[0]);
            VReg start = floats ? const_float(0.0) : const_int(0);
            if (c.args.size() == 2)
                start = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), acc_ty);
            return call_runtime(ints ? RtFn::ListSumI64 : RtFn::ListSumF64, acc_ty,
                                {seq, start});
        }
    }

    VReg seq = seq_conv != kNoReg ? seq_conv : expr(*c.args[0]);
    int seq_slot = new_temp_slot(seq_ty);
    store_slot(seq_slot, seq);
    Inst ln;
    ln.op = IrOp::Len;
    ln.a = seq;
    VReg n = def(t_int(), std::move(ln));
    int n_slot = new_temp_slot(t_int());
    store_slot(n_slot, n);
    int idx = new_temp_slot(t_int());
    store_slot(idx, const_int(0));

    VReg start = acc_ty && acc_ty->kind == TyKind::Float
                      ? const_float(0.0)
                      : (acc_ty && acc_ty->kind == TyKind::Big ? const_big("0")
                                                               : const_int(0));
    if (c.args.size() == 2)
        start = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), acc_ty);
    int acc = new_temp_slot(acc_ty);
    store_slot(acc, start);

    int cond = new_block("sum.cond");
    int body = new_block("sum.body");
    int incr = new_block("sum.incr");
    int end = new_block("sum.end");
    Inst j;
    j.op = IrOp::Jump;
    j.target = cond;
    emit(std::move(j));

    switch_to(cond);
    Inst cmp;
    cmp.op = IrOp::Cmp;
    cmp.cop = CmpKind::Lt;
    cmp.a = load_slot(idx, t_int());
    cmp.b = load_slot(n_slot, t_int());
    VReg cc = def(t_bool(), std::move(cmp));
    Inst br;
    br.op = IrOp::Branch;
    br.a = cc;
    br.target = body;
    br.target2 = end;
    emit(std::move(br));

    switch_to(body);
    VReg sv = load_slot(seq_slot, seq_ty);
    VReg iv = load_slot(idx, t_int());
    VReg ev;
    if (seq_ty->kind == TyKind::Dict || seq_ty->kind == TyKind::Set)
        ev = call_runtime(RtFn::DictKeyAt, elem_ty, {sv, iv});
    else {
        Inst get;
        get.op = (opts_.opt_level >= 1 &&
                  (seq_ty->kind == TyKind::List || seq_ty->kind == TyKind::Tuple))
                     ? IrOp::IndexLoadUnchecked
                     : IrOp::IndexLoad;
        get.a = sv;
        get.b = iv;
        ev = seq_ty->kind == TyKind::Str ? def_heap(elem_ty, std::move(get))
                                         : def(elem_ty, std::move(get));
    }
    ev = coerce(ev, elem_ty, acc_ty);
    if (acc_ty && acc_ty->kind == TyKind::Big) {
        // big 加法是运行时函数，不是裸 Bin：Bin 的 codegen 没有 big 路径，
        // 会把 big 指针当整数相加（实测段错误）。
        store_slot(acc, call_runtime(RtFn::BigAdd, t_big(), {load_slot(acc, acc_ty), ev}));
    } else {
        Inst add;
        add.op = IrOp::Bin;
        add.bop = BinKind::Add;
        add.a = load_slot(acc, acc_ty);
        add.b = ev;
        store_slot(acc, def(acc_ty, std::move(add)));
    }
    Inst jin;
    jin.op = IrOp::Jump;
    jin.target = incr;
    emit(std::move(jin));

    switch_to(incr);
    Inst add2;
    add2.op = IrOp::Bin;
    add2.bop = BinKind::Add;
    add2.a = load_slot(idx, t_int());
    add2.b = const_int(1);
    store_slot(idx, def(t_int(), std::move(add2)));
    Inst jb;
    jb.op = IrOp::Jump;
    jb.target = cond;
    emit(std::move(jb));

    switch_to(end);
    return load_slot(acc, acc_ty);
}


// any()/all()：短路迭代。any 遇到真值返回 True，all 遇到假值返回 False。
VReg Lowerer::lower_anyall(Call& c, bool is_any) {
    if (c.args.size() != 1) {
        err(c.line, c.col,
            std::format("{}() takes exactly one argument", is_any ? "any" : "all"));
        return const_bool(false);
    }
    TypePtr seq_ty = tc_.type_of(c.args[0].get());
    bool ok = seq_ty && (seq_ty->kind == TyKind::List || seq_ty->kind == TyKind::Tuple ||
                         seq_ty->kind == TyKind::Str || seq_ty->kind == TyKind::Bytes ||
                         seq_ty->kind == TyKind::Dict ||
                         seq_ty->kind == TyKind::Set || seq_ty->kind == TyKind::Range ||
                         seq_ty->kind == TyKind::Generator);
    if (!ok) {
        err(c.args[0]->line, c.args[0]->col,
            std::format("cannot iterate over '{}' in {}()", type_name(seq_ty),
                        is_any ? "any" : "all"));
        return const_bool(false);
    }
    TypePtr elem_ty = t_unknown();
    if (seq_ty->kind == TyKind::Str) elem_ty = t_str();
    else if (seq_ty->kind == TyKind::Bytes) elem_ty = t_int();
    else if (!seq_ty->sub.empty()) elem_ty = seq_ty->sub[0];
    // bytes 不能按下标当 list 读：先物化成 list[int] 再走通用路径。
    VReg seq_conv = kNoReg;
    if (seq_ty->kind == TyKind::Bytes) {
        seq_conv = call_runtime(RtFn::BytesToList, mk_list(t_int()), {expr(*c.args[0])});
        push_temp(seq_conv);
        seq_ty = mk_list(t_int());
        elem_ty = t_int();
    } else if (seq_ty->kind == TyKind::Str) {
        // any/all 对 str 是「每个字符非空即真」；物化避免逐下标 O(n^2)。
        seq_conv = call_runtime(RtFn::StrToList, mk_list(t_str()), {expr(*c.args[0])});
        push_temp(seq_conv);
        seq_ty = mk_list(t_str());
        elem_ty = t_str();
    }

    // range：按边界算，不物化。
    if (seq_ty->kind == TyKind::Range) {
        VReg seq = expr(*c.args[0]);
        return call_runtime(is_any ? RtFn::RangeAny : RtFn::RangeAll, t_bool(), {seq});
    }

    // 生成器：短路迭代，命中就停（剩下的元素连算都不算）。
    if (seq_ty->kind == TyKind::Generator) {
        VReg seq = expr(*c.args[0]);
        int result = new_temp_slot(t_bool());
        store_slot(result, const_bool(!is_any));
        int stop_b = new_block("anyall.gstop");
        emit_gen_for_each(seq, seq_ty, [&](VReg ev, TypePtr, int cond, int) {
            Inst ib;
            ib.op = IrOp::Branch;
            ib.a = ev;
            ib.target = is_any ? stop_b : cond;
            ib.target2 = is_any ? cond : stop_b;
            emit(std::move(ib));
        });
        int done_b = new_block("anyall.gdone");
        Inst j;
        j.op = IrOp::Jump;
        j.target = done_b;
        emit(std::move(j));
        switch_to(stop_b);
        store_slot(result, const_bool(is_any));
        Inst j2;
        j2.op = IrOp::Jump;
        j2.target = done_b;
        emit(std::move(j2));
        switch_to(done_b);
        return load_slot(result, t_bool());
    }

    VReg seq = seq_conv != kNoReg ? seq_conv : expr(*c.args[0]);
    int seq_slot = new_temp_slot(seq_ty);
    store_slot(seq_slot, seq);
    Inst ln;
    ln.op = IrOp::Len;
    ln.a = seq;
    VReg n = def(t_int(), std::move(ln));
    int n_slot = new_temp_slot(t_int());
    store_slot(n_slot, n);
    int idx = new_temp_slot(t_int());
    store_slot(idx, const_int(0));
    int result = new_temp_slot(t_bool());
    store_slot(result, const_bool(!is_any));

    int cond = new_block("anyall.cond");
    int body = new_block("anyall.body");
    int incr = new_block("anyall.incr");
    int stop = new_block("anyall.stop");
    int end = new_block("anyall.end");
    Inst j;
    j.op = IrOp::Jump;
    j.target = cond;
    emit(std::move(j));

    switch_to(cond);
    Inst cmp;
    cmp.op = IrOp::Cmp;
    cmp.cop = CmpKind::Lt;
    cmp.a = load_slot(idx, t_int());
    cmp.b = load_slot(n_slot, t_int());
    VReg cc = def(t_bool(), std::move(cmp));
    Inst br;
    br.op = IrOp::Branch;
    br.a = cc;
    br.target = body;
    br.target2 = end;
    emit(std::move(br));

    switch_to(body);
    VReg sv = load_slot(seq_slot, seq_ty);
    VReg iv = load_slot(idx, t_int());
    VReg ev;
    if (seq_ty->kind == TyKind::Dict || seq_ty->kind == TyKind::Set)
        ev = call_runtime(RtFn::DictKeyAt, elem_ty, {sv, iv});
    else {
        Inst get;
        get.op = (opts_.opt_level >= 1 &&
                  (seq_ty->kind == TyKind::List || seq_ty->kind == TyKind::Tuple))
                     ? IrOp::IndexLoadUnchecked
                     : IrOp::IndexLoad;
        get.a = sv;
        get.b = iv;
        ev = seq_ty->kind == TyKind::Str ? def_heap(elem_ty, std::move(get))
                                         : def(elem_ty, std::move(get));
    }
    Inst ib;
    ib.op = IrOp::Branch;
    ib.a = ev;
    ib.target = is_any ? stop : incr;
    ib.target2 = is_any ? incr : stop;
    emit(std::move(ib));

    switch_to(stop);
    store_slot(result, const_bool(is_any));
    Inst js;
    js.op = IrOp::Jump;
    js.target = end;
    emit(std::move(js));

    switch_to(incr);
    Inst add;
    add.op = IrOp::Bin;
    add.bop = BinKind::Add;
    add.a = load_slot(idx, t_int());
    add.b = const_int(1);
    store_slot(idx, def(t_int(), std::move(add)));
    Inst jb;
    jb.op = IrOp::Jump;
    jb.target = cond;
    emit(std::move(jb));

    switch_to(end);
    return load_slot(result, t_bool());
}

}  // namespace pyadamas
