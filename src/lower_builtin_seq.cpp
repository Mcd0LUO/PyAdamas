// lower_builtin_seq.cpp —— 内置函数：序列与迭代
#include "lower_impl.h"

namespace pyadamas {

VReg Lowerer::lower_builtin_seq(const std::string& name, Call& c) {
    if (name == "len") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "len() takes exactly one argument");
            return const_int(0);
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        VReg v = expr(*c.args[0]);
        VReg ln = dunder_len(v, t);
        if (ln != kNoReg) return ln;
        if (t && t->kind == TyKind::Range) {
            // range 的长度按边界算，不物化
            return call_runtime(RtFn::RangeLen, t_int(), {v});
        }
        if (t && t->kind == TyKind::Deque) {
            return call_runtime(RtFn::DequeLen, t_int(), {v});
        }
        if (t && t->kind == TyKind::Class) {
            err(c.line, c.col,
                std::format("len() of '{}' which has no __len__",
                            t->cls ? t->cls->name : "object"));
            return const_int(0);
        }
        Inst in;
        in.op = IrOp::Len;
        in.a = v;
        return def(t_int(), std::move(in));
    }

    if (name == "len_bytes") {
        // 字节长度出口：读 PyAdamasStr 的 +8（字节数），O(1)。
        if (c.args.size() != 1) {
            err(c.line, c.col, "len_bytes() takes exactly one argument");
            return const_int(0);
        }
        VReg v = expr(*c.args[0]);
        return call_runtime(RtFn::StrLenBytes, t_int(), {v});
    }

    if (name == "next") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "next() takes exactly one argument");
            return const_none();
        }
        TypePtr gt = tc_.type_of(c.args[0].get());
        if (gt && gt->kind == TyKind::File) {
            // next(f)：读一行，EOF 抛 StopIteration。
            VReg f = expr(*c.args[0]);
            VReg line = call_runtime(RtFn::FileReadLine, t_str(), {f, const_int(-1)});
            push_temp(line);
            Inst ln;
            ln.op = IrOp::Len;
            ln.a = line;
            VReg n = def(t_int(), std::move(ln));
            Inst c0;
            c0.op = IrOp::Cmp;
            c0.cop = CmpKind::Eq;
            c0.a = n;
            c0.b = const_int(0);
            VReg empty = def(t_bool(), std::move(c0));
            int ok = new_block("next.ok");
            int stop = new_block("next.stop");
            Inst br;
            br.op = IrOp::Branch;
            br.a = empty;
            br.target = stop;
            br.target2 = ok;
            emit(std::move(br));
            switch_to(stop);
            call_void(raise_rt(RtFn::Raise), {const_str("StopIteration"), const_str("")});
            Inst j;
            j.op = IrOp::Jump;
            j.target = ok;
            emit(std::move(j));
            switch_to(ok);
            return line;
        }
        if (!gt || gt->kind != TyKind::Generator) {
            err(c.line, c.col, std::format("next() requires a generator, got '{}'", type_name(gt)));
            return const_none();
        }
        TypePtr et = gt->sub.empty() ? t_unknown() : gt->sub[0];
        VReg g = expr(*c.args[0]);
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::GenNext);
        in.args = {g};
        // 运行时按位返回（浮点也是整数寄存器），vreg 类型记为 unknown；
        // 堆元素才入临时栈。
        VReg v = def(t_unknown(), std::move(in));
        v = retype(v, et);
        if (ir_is_heap(et)) push_temp(v);
        return v;
    }

    if (name == "iter") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "iter() takes exactly one argument");
            return const_none();
        }
        TypePtr it = tc_.type_of(c.args[0].get());
        if (it && (it->kind == TyKind::File || it->kind == TyKind::List ||
                   it->kind == TyKind::Tuple || it->kind == TyKind::Str ||
                   it->kind == TyKind::Bytes || it->kind == TyKind::Dict ||
                   it->kind == TyKind::Set || it->kind == TyKind::Range ||
                   it->kind == TyKind::Generator)) {
            return expr(*c.args[0]);
        }
        if (it && it->kind == TyKind::Class) {
            VReg seq = expr(*c.args[0]);
            VReg r = dunder_call(seq, it, "__iter__", {}, nullptr);
            if (r != kNoReg) return r;
            err(c.args[0]->line, c.args[0]->col,
                std::format("iter() of '{}' which has no usable __iter__",
                            type_name(it)));
            return const_none();
        }
        if (!it || it->kind == TyKind::Unknown) return expr(*c.args[0]);
        err(c.line, c.col,
            std::format("iter() of '{}' is not supported by the code generator yet",
                        type_name(it)));
        return const_none();
    }

    if (name == "range") {
        if (c.args.empty() || c.args.size() > 3) {
            err(c.line, c.col, "range() takes 1 to 3 arguments");
            return const_none();
        }
        auto arg = [&](size_t i) {
            VReg v = expr(*c.args[i]);
            return coerce(v, tc_.type_of(c.args[i].get()), t_int());
        };
        VReg start = const_int(0);
        VReg stop = arg(0);
        VReg step = const_int(1);
        if (c.args.size() >= 2) {
            start = stop;
            stop = arg(1);
        }
        if (c.args.size() == 3) step = arg(2);
        VReg v = call_runtime(RtFn::RangeNew, mk_range(t_int()), {start, stop, step});
        push_temp(v);
        return v;
    }

    if (name == "sorted" || name == "reversed") {
        if (c.args.size() != 1) {
            err(c.line, c.col, std::format("{}() takes exactly one argument", name));
            return const_none();
        }
        TypePtr xt = tc_.type_of(c.args[0].get());
        if (name == "reversed" && xt && xt->kind == TyKind::Class && xt->cls &&
            xt->cls->method("__reversed__")) {
            const MethodInfo* m = xt->cls->method("__reversed__");
            TypePtr rt = m->call_sig ? m->call_sig->ret : nullptr;
            if (m->decorated && m->decorated_ty && m->decorated_ty->kind == TyKind::Func &&
                m->decorated_ty->fn)
                rt = m->decorated_ty->fn->ret;
            VReg v = expr(*c.args[0]);
            VReg r = dunder_call(v, xt, "__reversed__", {}, nullptr);
            if (r == kNoReg) {
                err(c.line, c.col,
                    std::format("__reversed__ on '{}' must be a normal method",
                                xt->cls->name));
                return const_none();
            }
            TypePtr elem = t_unknown();
            return value_as_list(r, rt, elem);
        }
        if (xt && xt->kind == TyKind::Deque) {
            VReg d = expr(*c.args[0]);
            VReg xs = call_runtime(RtFn::DequeToList, mk_list(t_int()), {d});
            push_temp(xs);
            return xs;
        }
        if (!xt || (xt->kind != TyKind::List && xt->kind != TyKind::Tuple &&
                    xt->kind != TyKind::Str && xt->kind != TyKind::Bytes &&
                    xt->kind != TyKind::Dict &&
                    xt->kind != TyKind::Set && xt->kind != TyKind::File &&
                    xt->kind != TyKind::Range && xt->kind != TyKind::Generator)) {
            err(c.args[0]->line, c.args[0]->col,
                std::format("{}() cannot iterate over '{}'", name, type_name(xt)));
            return const_none();
        }
        TypePtr se = t_unknown();
        VReg x = seq_as_list(c.args[0].get(), se);
        TypePtr ty = tc_.type_of(&c);
        if (!ty || ty->kind != TyKind::List) ty = mk_list(se);

        // sorted(xs, key=f)：先算出每个元素的键，再按键排。
        // 之前 key= 被静默忽略——按 key 排和不按 key 排结果不同。
        Expr* key_expr = nullptr;
        for (auto& kw : c.keywords)
            if (kw.arg == "key") key_expr = kw.value.get();

        VReg l;
        if (key_expr) {
            TypePtr kt0 = tc_.type_of(key_expr);
            if (!kt0 || kt0->kind != TyKind::Func || !kt0->fn) {
                err(key_expr->line, key_expr->col,
                    "sorted() 'key' must be a function taking one argument");
                return const_none();
            }
            // 内建函数没有闭包对象，不能按用户函数那样调用。len 特判成取长度，
            // 其它内建函数明确报错——宁可报错，也不要静默排错。
            bool key_builtin = kt0->fn->unchecked;
            bool key_is_len = key_builtin && kt0->fn->name == "len";
            if (key_builtin && !key_is_len) {
                err(key_expr->line, key_expr->col,
                    std::format("sorted() 'key' does not support the builtin '{}'",
                                kt0->fn->name));
                return const_none();
            }
            if (!key_builtin && kt0->fn->pos.size() != 1 && !kt0->fn->varargs) {
                err(key_expr->line, key_expr->col,
                    "sorted() 'key' must be a function taking one argument");
                return const_none();
            }
            TypePtr kret = (kt0->fn->ret_known && kt0->fn->ret) ? kt0->fn->ret
                                                                 : t_unknown();
            TypePtr pty = kt0->fn->pos.empty() ? t_unknown() : kt0->fn->pos[0].ty;

            VReg fn = expr(*key_expr);
            // 键表按「元素静态种类」建：kret 可能是 unknown（返回类型没
            // 标注/推断不出来），而实际返回的是堆 str 时，unknown 表销毁
            // 走注册表是安全的；反过来标的堆类型也一致。历史上这里传的是
            // own_kind(kret) 且列表 append 之后没有 take，堆键每轮被
            // 双重释放（N18：sorted(key=返回 str) 规模化 rc 崩）。
            VReg keys = call_runtime(RtFn::ListNew, mk_list(kret),
                                      {const_int(0), const_int(own_kind(kret))});
            push_temp(keys);

            // keys = [f(e) for e in vals]
            int idx = new_temp_slot(t_int());
            store_slot(idx, const_int(0));
            Inst ln;
            ln.op = IrOp::Len;
            ln.a = x;
            VReg n = def(t_int(), std::move(ln));
            int cond = new_block("sortkey.cond");
            int body = new_block("sortkey.body");
            int end = new_block("sortkey.end");
            Inst j;
            j.op = IrOp::Jump;
            j.target = cond;
            emit(std::move(j));
            switch_to(cond);
            Inst more;
            more.op = IrOp::Cmp;
            more.cop = CmpKind::Lt;
            more.a = load_slot(idx, t_int());
            more.b = n;
            VReg more_v = def(t_bool(), std::move(more));
            Inst br;
            br.op = IrOp::Branch;
            br.a = more_v;
            br.target = body;
            br.target2 = end;
            emit(std::move(br));
            switch_to(body);
            Inst get;
            get.op = IrOp::IndexLoad;
            get.a = x;
            get.b = load_slot(idx, t_int());
            VReg ev = def(se, std::move(get));
            ev = coerce(ev, se, pty);
            VReg kv;
            if (key_is_len) {
                // key=len：直接取长度，不走闭包调用
                Inst ln2;
                ln2.op = IrOp::Len;
                ln2.a = ev;
                kv = def(t_int(), std::move(ln2));
            } else {
                Inst call;
                call.op = IrOp::Call;
                call.a = fn;
                call.args = {ev};
                kv = def_heap(kret, std::move(call));
            }
            // 键表的元素所有权：和普通容器写入同一条纪律（先 take/prep
            // 再 append），不能只 append —— 键表销毁时会按元素种类 dec，
            // 少了这次 retain 就是「双重释放」（N18）。kret 是 unknown
            // 时 store_list_elem 走运行时探针定型。
            TypePtr keys_ty = mk_list(kret);
            bool kowned = move_fresh(kv, kret);
            store_list_elem(keys, keys_ty, kv, kowned);
            call_void(RtFn::ListAppend, {keys, kv, heap_arg(kret)});
            Inst inc;
            inc.op = IrOp::Bin;
            inc.bop = BinKind::Add;
            inc.a = load_slot(idx, t_int());
            inc.b = const_int(1);
            store_slot(idx, def(t_int(), std::move(inc)));
            {
                Inst jj;
                jj.op = IrOp::Jump;
                jj.target = cond;
                emit(std::move(jj));
            }
            switch_to(end);

            Inst in2;
            in2.op = IrOp::CallRt;
            in2.imm = static_cast<int64_t>(RtFn::ListSortedBy);
            in2.args = {x, keys, const_int(sort_kind(kret)), const_str(sort_desc(kret))};
            in2.ty = ty;
            l = def(ty, std::move(in2));
            push_temp(l);
        } else {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(name == "sorted" ? RtFn::SortedList
                                                           : RtFn::ListReversed);
            in.args = {x};
            if (name == "sorted") {
                in.args.push_back(const_int(sort_kind(se)));
                in.args.push_back(const_str(sort_desc(se)));
            }
            in.ty = ty;
            l = def(ty, std::move(in));
            push_temp(l);
        }
        // sorted(xs, reverse=True)：排完整体反转。reverse 只接受字面量，
        // 运行时的 bool 要单独做分支，这里先不做。
        if (name == "sorted") {
            for (auto& kw : c.keywords) {
                if (kw.arg == "key") continue;
                if (kw.arg != "reverse") {
                    err(kw.value->line, kw.value->col,
                        std::format("sorted() does not support the '{}' argument", kw.arg));
                    return const_none();
                }
                auto cv = as_const(kw.value.get());
                if (!cv) {
                    err(kw.value->line, kw.value->col,
                        "sorted() 'reverse' must be a literal True/False");
                    return const_none();
                }
                if (cv->i != 0) call_void(RtFn::ListReverse, {l});
            }
        }
        return l;
    }

    if (name == "enumerate") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "enumerate() supports exactly one argument (no start=)");
            return const_none();
        }
        TypePtr xt = tc_.type_of(c.args[0].get());
        if (xt && xt->kind == TyKind::Deque) {
            VReg d = expr(*c.args[0]);
            VReg xs = call_runtime(RtFn::DequeToList, mk_list(t_int()), {d});
            push_temp(xs);
            return xs;
        }
        if (!xt || (xt->kind != TyKind::List && xt->kind != TyKind::Tuple &&
                    xt->kind != TyKind::Str && xt->kind != TyKind::Bytes &&
                    xt->kind != TyKind::Dict &&
                    xt->kind != TyKind::Set && xt->kind != TyKind::File &&
                    xt->kind != TyKind::Range && xt->kind != TyKind::Generator)) {
            err(c.args[0]->line, c.args[0]->col,
                std::format("enumerate() cannot iterate over '{}'", type_name(xt)));
            return const_none();
        }
        TypePtr se = t_unknown();
        VReg x = seq_as_list(c.args[0].get(), se);
        TypePtr ty = tc_.type_of(&c);
        if (!ty || ty->kind != TyKind::List) ty = mk_list(mk_tuple({t_int(), se}));
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::EnumerateList);
        in.args = {x};
        in.ty = ty;
        VReg l = def(ty, std::move(in));
        push_temp(l);
        return l;
    }

    if (name == "zip") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "zip() supports exactly two iterables for now");
            return const_none();
        }
        TypePtr se1 = t_unknown(), se2 = t_unknown();
        VReg a = seq_as_list(c.args[0].get(), se1);
        VReg b = seq_as_list(c.args[1].get(), se2);
        TypePtr ty = tc_.type_of(&c);
        if (!ty || ty->kind != TyKind::List) ty = mk_list(mk_tuple({se1, se2}));
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::Zip2List);
        in.args = {a, b};
        in.ty = ty;
        VReg l = def(ty, std::move(in));
        push_temp(l);
        return l;
    }

    if (name == "bytes") {
        if (c.args.size() > 2) {
            err(c.line, c.col, "bytes() expects at most 2 arguments");
            return const_none();
        }
        TypePtr ty = t_bytes();
        if (c.args.empty()) return retype(const_str(""), ty);
        // 第二参是编码/错误（str/None）：只改变语义，忽略。
        TypePtr at = tc_.type_of(c.args[0].get());
        VReg v = expr(*c.args[0]);
        if (at && at->kind == TyKind::Bytes) return v;
        if (at && at->kind == TyKind::Int) {
            VReg b = call_runtime(RtFn::BytesZeros, ty, {v});
            push_temp(b);
            return b;
        }
        // list/tuple/生成器/range 的 int 元素 → 物化成字节序列
        if (at && (at->kind == TyKind::List || at->kind == TyKind::Tuple ||
                   at->kind == TyKind::Generator)) {
            TypePtr se = t_int();
            VReg item = seq_as_list(c.args[0].get(), se);
            VReg b = call_runtime(RtFn::BytesFromList, ty, {item});
            push_temp(b);
            return b;
        }
        if (at && (at->kind == TyKind::Unknown || at->kind == TyKind::None)) {
            // unknown / None：按空字节保守处理（调用方若真传非空会在
            // 别处按类型检查兜底）。
            return retype(const_str(""), ty);
        }
        err(c.args[0]->line, c.args[0]->col,
            std::format("bytes() argument must be a bytes-like or iterable of int, got '{}'",
                        type_name(at)));
        return const_none();
    }

    return kNoReg;
}

}  // namespace pyadamas
