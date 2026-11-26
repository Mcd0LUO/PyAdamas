// lower/stmt_for.cpp —— for 的各个形态（range/enumerate/zip/序列）
#include "lower/impl.h"
#include "lower/idiom.h"


namespace pyadamas {

// zip / enumerate 迭代：成对序列的解包与索引迭代

void Lowerer::lower_for_enumerate(For& f, Call& c) {
    VReg seq = expr(*c.args[0]);
    TypePtr sty = tc_.type_of(c.args[0].get());
    TypePtr ety = t_unknown();
    if (sty->kind == TyKind::Str) ety = t_str();
    else if (!sty->sub.empty()) ety = sty->sub[0];

    // 生成器里 vreg 不跨 yield：序列落槽。
    int seq_slot = -1;
    if (st_->is_generator) {
        seq_slot = new_temp_slot(sty);
        store_slot(seq_slot, seq);
    }
    auto seq_val = [&]() { return seq_slot >= 0 ? load_slot(seq_slot, sty) : seq; };

    Inst ln;
    ln.op = IrOp::Len;
    ln.a = seq;
    int n_slot = new_temp_slot(t_int());
    store_slot(n_slot, def(t_int(), std::move(ln)));

    int idx = new_temp_slot(t_int());
    store_slot(idx, const_int(0));
    int str_cur = sty->kind == TyKind::Str ? new_str_cursor() : -1;

    int cond = new_block("for.enum.cond");
    int body = new_block("for.enum.body");
    int incr = new_block("for.enum.incr");
    int end = new_block("for.enum.end");
    int orelse = f.orelse.empty() ? end : new_block("for.enum.else");

    int mark_block = -1;
    size_t mark_pos = 0;
    VReg mark = st_->is_generator ? kNoReg : rc_mark_at(mark_block, mark_pos);
    auto clean = [&]() {
        if (st_->is_generator) cleanup_gen_mark();
        else rc_cleanup(mark);
    };
    size_t pushes_before = st_->rc_pushes;

    Inst j;
    j.op = IrOp::Jump;
    int ver_slot = -1;
    if (sty->kind == TyKind::Dict) {
        ver_slot = new_temp_slot(t_int());
        store_slot(ver_slot, call_runtime(RtFn::DictVersion, t_int(), {seq_val()}));
    }
    j.target = cond;
    emit(std::move(j));

    switch_to(cond);
    // 迭代期间容器大小变化 → 可捕获的 RuntimeError，且在「还有没有下一个」之前
    // 检查：删空之后仍要在下一次取元素时报错，而不是被下标上界悄悄结束。
    if (sty->kind == TyKind::Dict || sty->kind == TyKind::Set) {
        Inst lc;
        lc.op = IrOp::Len;
        lc.a = seq_val();
        VReg cur_len = def(t_int(), std::move(lc));
        Inst ne;
        ne.op = IrOp::Cmp;
        ne.cop = CmpKind::Ne;
        ne.a = cur_len;
        ne.b = load_slot(n_slot, t_int());
        VReg ne_cc = def(t_bool(), std::move(ne));
        int raise_b = new_block("for.iter.changed");
        int ok_b = new_block("for.iter.ok");
        Inst br2;
        br2.op = IrOp::Branch;
        br2.a = ne_cc;
        br2.target = raise_b;
        br2.target2 = ok_b;
        emit(std::move(br2));
        switch_to(raise_b);
        call_void(raise_rt(RtFn::Raise),
                  {const_str("RuntimeError"),
                   const_str(sty->kind == TyKind::Dict
                                 ? "dictionary changed size during iteration"
                                 : "Set changed size during iteration")});
        switch_to(ok_b);
        if (sty->kind == TyKind::Dict && ver_slot >= 0) {
            VReg cur_v = call_runtime(RtFn::DictVersion, t_int(), {seq_val()});
            Inst ve;
            ve.op = IrOp::Cmp;
            ve.cop = CmpKind::Ne;
            ve.a = cur_v;
            ve.b = load_slot(ver_slot, t_int());
            int kchanged = new_block("for.iter.keys");
            int kok = new_block("for.iter.keys_ok");
            Inst vbr;
            vbr.op = IrOp::Branch;
            vbr.a = def(t_bool(), std::move(ve));
            vbr.target = kchanged;
            vbr.target2 = kok;
            emit(std::move(vbr));
            switch_to(kchanged);
            call_void(raise_rt(RtFn::Raise),
                      {const_str("RuntimeError"),
                       const_str("dictionary keys changed during iteration")});
            switch_to(kok);
        }
    }
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
    br.target2 = orelse;
    emit(std::move(br));

    switch_to(body);
    clean();
    VReg iv = load_slot(idx, t_int());
    VReg ev;
    if (sty->kind == TyKind::Dict || sty->kind == TyKind::Set) {
        ev = call_runtime(RtFn::DictKeyAt, ety, {seq_val(), iv});
    } else if (sty->kind == TyKind::Str) {
        ev = str_char_at(seq_val(), iv, str_cur);
    } else {
        Inst get;
        get.op = IrOp::IndexLoad;
        get.a = seq_val();
        get.b = iv;
        ev = def(ety, std::move(get));
    }
    bind_iter_pair(*f.target, iv, t_int(), ev, ety);

    st_->loops.push_back({end, incr});
    st_->loop_cleanup_depth.push_back(st_->cleanups.size());
    stmts(f.body);
    st_->loops.pop_back();
    st_->loop_cleanup_depth.pop_back();
    if (!terminated()) {
        Inst jj;
        jj.op = IrOp::Jump;
        jj.target = incr;
        emit(std::move(jj));
    }

    switch_to(incr);
    clean();
    Inst add;
    add.op = IrOp::Bin;
    add.bop = BinKind::Add;
    add.a = load_slot(idx, t_int());
    add.b = const_int(1);
    store_slot(idx, def(t_int(), std::move(add)));
    Inst ji;
    ji.op = IrOp::Jump;
    ji.target = cond;
    emit(std::move(ji));

    switch_to(orelse);
    if (!f.orelse.empty()) {
        stmts(f.orelse);
        if (!terminated()) {
            Inst jo;
            jo.op = IrOp::Jump;
            jo.target = end;
            emit(std::move(jo));
        }
    }

    switch_to(end);
    clean();
    if (!st_->is_generator && st_->rc_pushes == pushes_before) {
        erase_cleanup(body, mark);
        erase_cleanup(incr, mark);
        erase_cleanup(end, mark);
        erase_mark(mark_block, mark_pos, mark);
    }
}

// for a, b in zip(xs, ys)：双游标，不建元组表。长度取两者较小值。
void Lowerer::lower_for_zip(For& f, Call& c) {
    VReg a = expr(*c.args[0]);
    VReg b = expr(*c.args[1]);
    TypePtr at = tc_.type_of(c.args[0].get());
    TypePtr bt = tc_.type_of(c.args[1].get());
    TypePtr ea = t_unknown();
    TypePtr eb = t_unknown();
    if (at->kind == TyKind::Str) ea = t_str();
    else if (!at->sub.empty()) ea = at->sub[0];
    if (bt->kind == TyKind::Str) eb = t_str();
    else if (!bt->sub.empty()) eb = bt->sub[0];

    int as = -1;
    int bs = -1;
    if (st_->is_generator) {
        as = new_temp_slot(at);
        store_slot(as, a);
        bs = new_temp_slot(bt);
        store_slot(bs, b);
    }
    auto aval = [&]() { return as >= 0 ? load_slot(as, at) : a; };
    auto bval = [&]() { return bs >= 0 ? load_slot(bs, bt) : b; };

    // n = min(len(a), len(b))
    Inst la;
    la.op = IrOp::Len;
    la.a = a;
    VReg na = def(t_int(), std::move(la));
    Inst lb;
    lb.op = IrOp::Len;
    lb.a = b;
    VReg nb = def(t_int(), std::move(lb));
    int n_slot = new_temp_slot(t_int());
    // 迭代期间容器大小变化要抛可捕获的 RuntimeError（与 for k in d 同口径）。
    // 存下两个操作数**各自**的入口长度：zip 用的是 min，删短其中一个会让两者
    // 长度相对关系变化，只盯 min 发现不了。
    int na_slot = new_temp_slot(t_int());
    int nb_slot = new_temp_slot(t_int());
    store_slot(na_slot, na);
    store_slot(nb_slot, nb);
    // 版本号：捕捉「删一个再加一个」（大小不变、键集合变了）。
    int va_slot = -1, vb_slot = -1;
    if (at->kind == TyKind::Dict) {
        va_slot = new_temp_slot(t_int());
        store_slot(va_slot, call_runtime(RtFn::DictVersion, t_int(), {aval()}));
    }
    if (bt->kind == TyKind::Dict) {
        vb_slot = new_temp_slot(t_int());
        store_slot(vb_slot, call_runtime(RtFn::DictVersion, t_int(), {bval()}));
    }
    int pick_a = new_block("for.zip.a");
    int pick_b = new_block("for.zip.b");
    int after = new_block("for.zip.min");
    Inst cm;
    cm.op = IrOp::Cmp;
    cm.cop = CmpKind::Lt;
    cm.a = na;
    cm.b = nb;
    VReg lt = def(t_bool(), std::move(cm));
    Inst br0;
    br0.op = IrOp::Branch;
    br0.a = lt;
    br0.target = pick_a;
    br0.target2 = pick_b;
    emit(std::move(br0));
    switch_to(pick_a);
    store_slot(n_slot, na);
    Inst ja;
    ja.op = IrOp::Jump;
    ja.target = after;
    emit(std::move(ja));
    switch_to(pick_b);
    store_slot(n_slot, nb);
    Inst jb;
    jb.op = IrOp::Jump;
    jb.target = after;
    emit(std::move(jb));
    switch_to(after);

    int idx = new_temp_slot(t_int());
    store_slot(idx, const_int(0));
    int a_cur = at->kind == TyKind::Str ? new_str_cursor() : -1;
    int b_cur = bt->kind == TyKind::Str ? new_str_cursor() : -1;

    int cond = new_block("for.zip.cond");
    int body = new_block("for.zip.body");
    int incr = new_block("for.zip.incr");
    int end = new_block("for.zip.end");
    int orelse = f.orelse.empty() ? end : new_block("for.zip.else");

    int mark_block = -1;
    size_t mark_pos = 0;
    VReg mark = st_->is_generator ? kNoReg : rc_mark_at(mark_block, mark_pos);
    auto clean = [&]() {
        if (st_->is_generator) cleanup_gen_mark();
        else rc_cleanup(mark);
    };
    size_t pushes_before = st_->rc_pushes;

    Inst j;
    j.op = IrOp::Jump;
    j.target = cond;
    emit(std::move(j));

    switch_to(cond);
    if (at->kind == TyKind::Dict || at->kind == TyKind::Set ||
        bt->kind == TyKind::Dict || bt->kind == TyKind::Set) {
        auto guard = [&](const TypePtr& ty, auto&& val, int entry_slot, int ver_slot) {
            if (!ty || (ty->kind != TyKind::Dict && ty->kind != TyKind::Set)) return;
            // 先比大小，再比版本：大小变了报 size，只有「大小不变但键变了」
            // 才报 keys（del + add）。顺序反了会把 size 变形报成 keys。
            int size_changed = new_block("for.zip.changed");
            int after_size = new_block("for.zip.after_size");
            Inst lc;
            lc.op = IrOp::Len;
            lc.a = val();
            VReg cur = def(t_int(), std::move(lc));
            Inst ne;
            ne.op = IrOp::Cmp;
            ne.cop = CmpKind::Ne;
            ne.a = cur;
            ne.b = load_slot(entry_slot, t_int());
            Inst bg;
            bg.op = IrOp::Branch;
            bg.a = def(t_bool(), std::move(ne));
            bg.target = size_changed;
            bg.target2 = after_size;
            emit(std::move(bg));
            switch_to(size_changed);
            call_void(raise_rt(RtFn::Raise),
                      {const_str("RuntimeError"),
                       const_str(ty->kind == TyKind::Dict
                                     ? "dictionary changed size during iteration"
                                     : "Set changed size during iteration")});
            switch_to(after_size);
            if (ty->kind == TyKind::Dict && ver_slot >= 0) {
                VReg cur_v = call_runtime(RtFn::DictVersion, t_int(), {val()});
                Inst ve;
                ve.op = IrOp::Cmp;
                ve.cop = CmpKind::Ne;
                ve.a = cur_v;
                ve.b = load_slot(ver_slot, t_int());
                int kchanged = new_block("for.zip.keys_changed");
                int kok = new_block("for.zip.keys_ok");
                Inst vbr;
                vbr.op = IrOp::Branch;
                vbr.a = def(t_bool(), std::move(ve));
                vbr.target = kchanged;
                vbr.target2 = kok;
                emit(std::move(vbr));
                switch_to(kchanged);
                call_void(raise_rt(RtFn::Raise),
                          {const_str("RuntimeError"),
                           const_str("dictionary keys changed during iteration")});
                switch_to(kok);
            }
        };
        // **顺序有意义**：zip 先问 a 再问 b。a 已经耗尽就直接结束，
        // 这时 b 里改东西不该报错（CPython 的 zip 根本不会碰 b）。
        // 0.366 第一版两个一起检查，于是「最后一次取元素之后改 b」误报。
        guard(at, aval, na_slot, va_slot);
        {
            int next_ab = new_block("for.zip.chk_b");
            Inst c1;
            c1.op = IrOp::Cmp;
            c1.cop = CmpKind::Lt;
            c1.a = load_slot(idx, t_int());
            c1.b = load_slot(na_slot, t_int());
            Inst b1;
            b1.op = IrOp::Branch;
            b1.a = def(t_bool(), std::move(c1));
            b1.target = next_ab;
            b1.target2 = orelse;
            emit(std::move(b1));
            switch_to(next_ab);
            guard(bt, bval, nb_slot, vb_slot);
            {
                int body_ab = new_block("for.zip.chk_ok");
                Inst c2;
                c2.op = IrOp::Cmp;
                c2.cop = CmpKind::Lt;
                c2.a = load_slot(idx, t_int());
                c2.b = load_slot(nb_slot, t_int());
                Inst b2;
                b2.op = IrOp::Branch;
                b2.a = def(t_bool(), std::move(c2));
                b2.target = body_ab;
                b2.target2 = orelse;
                emit(std::move(b2));
                switch_to(body_ab);
            }
        }
    }
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
    br.target2 = orelse;
    emit(std::move(br));

    switch_to(body);
    clean();
    VReg iv = load_slot(idx, t_int());
    VReg va;
    if (at->kind == TyKind::Dict || at->kind == TyKind::Set) {
        va = call_runtime(RtFn::DictKeyAt, ea, {aval(), iv});
    } else if (at->kind == TyKind::Str) {
        va = str_char_at(aval(), iv, a_cur);
    } else {
        Inst g1;
        g1.op = IrOp::IndexLoad;
        g1.a = aval();
        g1.b = iv;
        va = def(ea, std::move(g1));
    }

    VReg vb;
    if (bt->kind == TyKind::Dict || bt->kind == TyKind::Set) {
        vb = call_runtime(RtFn::DictKeyAt, eb, {bval(), iv});
    } else if (bt->kind == TyKind::Str) {
        vb = str_char_at(bval(), iv, b_cur);
    } else {
        Inst g2;
        g2.op = IrOp::IndexLoad;
        g2.a = bval();
        g2.b = iv;
        vb = def(eb, std::move(g2));
    }
    bind_iter_pair(*f.target, va, ea, vb, eb);

    st_->loops.push_back({end, incr});
    st_->loop_cleanup_depth.push_back(st_->cleanups.size());
    stmts(f.body);
    st_->loops.pop_back();
    st_->loop_cleanup_depth.pop_back();
    if (!terminated()) {
        Inst jj;
        jj.op = IrOp::Jump;
        jj.target = incr;
        emit(std::move(jj));
    }

    switch_to(incr);
    clean();
    Inst add;
    add.op = IrOp::Bin;
    add.bop = BinKind::Add;
    add.a = load_slot(idx, t_int());
    add.b = const_int(1);
    store_slot(idx, def(t_int(), std::move(add)));
    Inst ji;
    ji.op = IrOp::Jump;
    ji.target = cond;
    emit(std::move(ji));

    switch_to(orelse);
    if (!f.orelse.empty()) {
        stmts(f.orelse);
        if (!terminated()) {
            Inst jo;
            jo.op = IrOp::Jump;
            jo.target = end;
            emit(std::move(jo));
        }
    }

    switch_to(end);
    clean();
    if (!st_->is_generator && st_->rc_pushes == pushes_before) {
        erase_cleanup(body, mark);
        erase_cleanup(incr, mark);
        erase_cleanup(end, mark);
        erase_mark(mark_block, mark_pos, mark);
    }
}


// `for kv in enumerate(d)` / `for p in zip(a, b)` 这种**单个名字**的目标：
// 快速路径要能把两个值绑成一个元组。可解构的目标仍走原来的逐元素赋值。
void Lowerer::bind_iter_pair(Expr& target, VReg a, TypePtr aty, VReg b, TypePtr bty) {
    if (target.kind == ExprKind::Name) {
        TypePtr tt = mk_tuple({aty, bty});
        std::vector<VReg> vals{a, b};
        std::vector<TypePtr> tys{aty, bty};
        VReg tupv = build_seq_vregs(std::move(vals), std::move(tys), tt, true);
        assign_to(target, tupv, tt);
        return;
    }
    auto& tup = static_cast<TupleExpr&>(target);
    assign_to(*tup.elts[0], a, aty);
    assign_to(*tup.elts[1], b, bty);
}

}  // namespace pyadamas
