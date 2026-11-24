// lower/print.cpp —— print 与值输出降级
#include "lower/impl.h"

namespace pyadamas {

void Lowerer::print_value(VReg v, TypePtr ty, int line, int col) {
    if (print_file_slot_ >= 0) {
        print_value_to_file(v, ty, line, col);
        return;
    }
    if (!ty) ty = t_unknown();
    switch (ty->kind) {
        case TyKind::None: call_void(RtFn::PrintNone, {}); break;
        case TyKind::Bool: call_void(RtFn::PrintBool, {v}); break;
        case TyKind::Float: call_void(RtFn::PrintF64, {v}); break;
        case TyKind::Str: call_void(RtFn::PrintStr, {v}); break;
        case TyKind::Bytes: call_void(RtFn::PrintBytes, {v}); break;
        case TyKind::Int:
        case TyKind::I8:
        case TyKind::I16:
        case TyKind::I32:
        case TyKind::U8:
        case TyKind::U16:
        case TyKind::U32: call_void(RtFn::PrintI64, {v}); break;
        // u64 必须按无符号打印：≥ 2^63 的值走 pa_print_i64 会打出负数。
        case TyKind::U64: call_void(RtFn::PrintU64, {v}); break;
        case TyKind::List: print_seq(v, ty, false, false, line, col); break;
        case TyKind::Range: {
            // range 打印成 "range(0, 10)"，和 CPython 一样
            VReg s = call_runtime(RtFn::RangeRepr, t_str(), {v});
            push_temp(s);
            call_void(RtFn::PrintStr, {s});
            break;
        }
        case TyKind::Tuple: print_seq(v, ty, true, false, line, col); break;
        case TyKind::TypeObj:
            call_void(RtFn::PrintStr, {const_str("<class '")});
            call_void(RtFn::PrintStr, {v});
            call_void(RtFn::PrintStr, {const_str("'>")});
            break;
        case TyKind::ClassObj: {
            VReg s = call_runtime(RtFn::ClassRepr, t_str(), {v});
            push_temp(s);
            call_void(RtFn::PrintStr, {s});
            break;
        }
        case TyKind::Dict: print_dict(v, ty, false, line, col); break;
        case TyKind::Set: print_set(v, ty, false, line, col); break;
        case TyKind::Random:
            call_void(RtFn::PrintStr, {const_str("<random.Random object>")});
            break;
        case TyKind::CompletedProcess:
            call_void(RtFn::PrintStr, {const_str("<subprocess.CompletedProcess object>")});
            break;
        case TyKind::Popen:
            call_void(RtFn::PrintStr, {const_str("<subprocess.Popen object>")});
            break;
        case TyKind::DateTime: {
            VReg s = call_runtime(RtFn::DateTimeIsoformat, t_str(), {v});
            push_temp(s);
            call_void(RtFn::PrintStr, {s});
            break;
        }
        case TyKind::TimeDelta: {
            VReg s = call_runtime(RtFn::TimeDeltaStr, t_str(), {v});
            push_temp(s);
            call_void(RtFn::PrintStr, {s});
            break;
        }
        case TyKind::Path: {
            VReg s = call_runtime(RtFn::PathStr, t_str(), {v});
            push_temp(s);
            call_void(RtFn::PrintStr, {s});
            break;
        }
        case TyKind::Deque:
            call_void(RtFn::PrintStr, {const_str("<deque object>")});
            break;
        case TyKind::DYLIB:
            call_void(RtFn::PrintStr, {const_str("<dylib handle>")});
            break;
        case TyKind::Exception: {
            VReg s = call_runtime(RtFn::ExcValueStr, t_str(), {v});
            push_temp(s);
            call_void(RtFn::PrintStr, {s});
            break;
        }
        case TyKind::Complex: {
            VReg s = call_runtime(RtFn::ComplexStr, t_str(), {v});
            push_temp(s);
            call_void(RtFn::PrintStr, {s});
            break;
        }
        case TyKind::Big: {
            // P3 big：十进制输出（pa_big_str，任意长）。
            VReg s = call_runtime(RtFn::BigStr, t_str(), {v});
            push_temp(s);
            call_void(RtFn::PrintStr, {s});
            break;
        }
        case TyKind::Optional: {
            TypePtr inner = ty->sub.empty() ? t_unknown() : ty->sub[0];
            int some = new_block("print.opt.some");
            int none = new_block("print.opt.none");
            int end = new_block("print.opt.end");
            Inst br;
            br.op = IrOp::Branch;
            br.a = v;
            br.target = some;
            br.target2 = none;
            emit(std::move(br));
            switch_to(none);
            call_void(RtFn::PrintNone, {});
            {
                Inst j;
                j.op = IrOp::Jump;
                j.target = end;
                emit(std::move(j));
            }
            switch_to(some);
            print_value(v, inner, line, col);
            {
                Inst j;
                j.op = IrOp::Jump;
                j.target = end;
                emit(std::move(j));
            }
            switch_to(end);
            break;
        }
        case TyKind::Class: {
            // 有 __str__（或只有 __repr__）就调它；否则按 CPython 的形状
            // 打一个 `<Point object>`。
            VReg s = dunder_str(v, ty, false);
            if (s != kNoReg) {
                call_void(RtFn::PrintStr, {s});
                break;
            }
            std::string name = ty->cls ? ty->cls->name : "object";
            call_void(RtFn::PrintStr, {const_str("<" + name + " object>")});
            break;
        }
        default:
            // 推断不出来时交给运行时：先查 GC 的地址索引，是堆对象就按
            // 真实类型分派（目前认 str），否则按整数输出。
            (void)line;
            (void)col;
            call_void(RtFn::PrintAny, {v});
            break;
    }
}

// print(..., file=f)：值先按 Print 的类型规则转成字符串，再写到文件。
void Lowerer::print_value_to_file(VReg v, TypePtr ty, int line, int col) {
    if (!ty) ty = t_unknown();
    VReg f = load_slot(print_file_slot_, t_file());
    switch (ty->kind) {
        case TyKind::List: print_seq(v, ty, false, true, line, col); return;
        case TyKind::Range: {
            VReg s = call_runtime(RtFn::RangeRepr, t_str(), {v});
            push_temp(s);
            call_void(RtFn::FileWrite, {f, s});
            return;
        }
        case TyKind::Tuple: print_seq(v, ty, true, true, line, col); return;
        case TyKind::TypeObj: {
            call_void(RtFn::FileWrite, {f, const_str("<class '")});
            call_void(RtFn::FileWrite, {f, v});
            call_void(RtFn::FileWrite, {f, const_str("'>")});
            return;
        }
        case TyKind::ClassObj: {
            VReg s = call_runtime(RtFn::ClassRepr, t_str(), {v});
            push_temp(s);
            call_void(RtFn::FileWrite, {f, s});
            return;
        }
        case TyKind::Dict: print_dict(v, ty, true, line, col); return;
        case TyKind::Set: print_set(v, ty, true, line, col); return;
        case TyKind::Random:
            call_void(RtFn::FileWrite, {f, const_str("<random.Random object>")});
            return;
        case TyKind::CompletedProcess:
            call_void(RtFn::FileWrite, {f, const_str("<subprocess.CompletedProcess object>")});
            return;
        case TyKind::Popen:
            call_void(RtFn::FileWrite, {f, const_str("<subprocess.Popen object>")});
            return;
        case TyKind::DateTime: {
            VReg s = call_runtime(RtFn::DateTimeIsoformat, t_str(), {v});
            push_temp(s);
            call_void(RtFn::FileWrite, {f, s});
            return;
        }
        case TyKind::TimeDelta: {
            VReg s = call_runtime(RtFn::TimeDeltaStr, t_str(), {v});
            push_temp(s);
            call_void(RtFn::FileWrite, {f, s});
            return;
        }
        case TyKind::Path: {
            VReg s = call_runtime(RtFn::PathStr, t_str(), {v});
            push_temp(s);
            call_void(RtFn::FileWrite, {f, s});
            return;
        }
        case TyKind::Deque:
            call_void(RtFn::FileWrite, {f, const_str("<deque object>")});
            return;
        case TyKind::Class: {
            VReg s = dunder_str(v, ty, false);
            if (s != kNoReg) {
                call_void(RtFn::FileWrite, {f, s});
                return;
            }
            std::string name = ty->cls ? ty->cls->name : "object";
            call_void(RtFn::FileWrite, {f, const_str("<" + name + " object>")});
            return;
        }
        case TyKind::Optional: {
            TypePtr inner = ty->sub.empty() ? t_unknown() : ty->sub[0];
            int some = new_block("print.opt.some");
            int none = new_block("print.opt.none");
            int end = new_block("print.opt.end");
            Inst br;
            br.op = IrOp::Branch;
            br.a = v;
            br.target = some;
            br.target2 = none;
            emit(std::move(br));
            switch_to(none);
            call_void(RtFn::FileWrite, {load_slot(print_file_slot_, t_file()),
                                        const_str("None")});
            {
                Inst j;
                j.op = IrOp::Jump;
                j.target = end;
                emit(std::move(j));
            }
            switch_to(some);
            print_value_to_file(v, inner, line, col);
            {
                Inst j;
                j.op = IrOp::Jump;
                j.target = end;
                emit(std::move(j));
            }
            switch_to(end);
            return;
        }
        default: {
            VReg s = convert_to(v, ty, t_str());
            call_void(RtFn::FileWrite, {load_slot(print_file_slot_, t_file()), s});
            return;
        }
    }
}

void Lowerer::print_seq(VReg v, TypePtr ty, bool tuple, bool to_file, int line, int col) {
    auto write = [&](const std::string& s) { print_write(s, to_file); };
    write(tuple ? "(" : "[");

    // 定长 tuple 的元素类型可以逐个不同（d.items() 返回 (str, int) 这种）。
    // 循环体里只能用一个静态类型；类型不一致就按常量下标展开，每个元素用
    // 自己的类型打印。否则 int 会被当成 str 交给 repr，直接段错误。
    if (tuple && !ty->variadic && ty->sub.size() > 1) {
        bool same = true;
        for (size_t i = 1; i < ty->sub.size(); ++i)
            if (!type_eq(ty->sub[0], ty->sub[i])) {
                same = false;
                break;
            }
        if (!same) {
            for (size_t i = 0; i < ty->sub.size(); ++i) {
                if (i) write(", ");
                Inst get;
                get.op = IrOp::IndexLoad;
                get.a = v;
                get.b = const_int(static_cast<int64_t>(i));
                VReg ev = def(ty->sub[i], std::move(get));
                if (ty->sub[i] && ty->sub[i]->kind == TyKind::Str) {
                    Inst rp;
                    rp.op = IrOp::CallRt;
                    rp.imm = static_cast<int64_t>(RtFn::ReprStr);
                    rp.args = {ev};
                    VReg rs = def_heap(t_str(), std::move(rp));
                    if (to_file)
                        call_void(RtFn::FileWrite,
                                  {load_slot(print_file_slot_, t_file()), rs});
                    else
                        call_void(RtFn::PrintStr, {rs});
                } else if (to_file) {
                    print_value_to_file(ev, ty->sub[i], line, col);
                } else {
                    print_value(ev, ty->sub[i], line, col);
                }
            }
            // 单元素元组在 CPython 里带尾逗号：(1,) 不是 (1)
            if (tuple && ty->sub.size() == 1) write(",");
            write(")");
            return;
        }
    }

    TypePtr et = ty->sub.empty() ? t_unknown() : ty->sub[0];
    print_each(v, et, to_file, [&](VReg ev, VReg) {
        print_elem(ev, et, to_file, line, col);
    });
    if (tuple) {
        // 单元素元组在 CPython 里带尾逗号：(1,) 不是 (1)
        if (!ty->variadic && ty->sub.size() == 1) {
            write(",");
        } else if (ty->variadic) {
            // 变长元组的长度运行期才知道
            Inst ln;
            ln.op = IrOp::Len;
            ln.a = v;
            VReg n = def(t_int(), std::move(ln));
            Inst cmp;
            cmp.op = IrOp::Cmp;
            cmp.cop = CmpKind::Eq;
            cmp.a = n;
            cmp.b = const_int(1);
            VReg is_one = def(t_bool(), std::move(cmp));
            int one_b = new_block("print.tuple1");
            int done_b = new_block("print.tuple.end");
            Inst br;
            br.op = IrOp::Branch;
            br.a = is_one;
            br.target = one_b;
            br.target2 = done_b;
            emit(std::move(br));
            switch_to(one_b);
            write(",");
            Inst j;
            j.op = IrOp::Jump;
            j.target = done_b;
            emit(std::move(j));
            switch_to(done_b);
        }
    }
    write(tuple ? ")" : "]");
}

// 往 stdout 或 print(..., file=f) 写一段字面量。
void Lowerer::print_write(const std::string& s, bool to_file) {
    if (to_file)
        call_void(RtFn::FileWrite, {load_slot(print_file_slot_, t_file()), const_str(s)});
    else
        call_void(RtFn::PrintStr, {const_str(s)});
}

// 容器里的元素按 repr 语义打印（CPython 打印 list/tuple/dict 用元素 repr）：
// 字符串带引号，类实例走 __repr__，其余按值打印。
void Lowerer::print_elem(VReg ev, TypePtr et, bool to_file, int line, int col) {
    if (et && et->kind == TyKind::Str) {
        // 容器内部的字符串按 repr 打印（带引号），和 CPython 一致。
        Inst rp;
        rp.op = IrOp::CallRt;
        rp.imm = static_cast<int64_t>(RtFn::ReprStr);
        rp.args = {ev};
        VReg rs = def_heap(t_str(), std::move(rp));
        if (to_file)
            call_void(RtFn::FileWrite, {load_slot(print_file_slot_, t_file()), rs});
        else
            call_void(RtFn::PrintStr, {rs});
    } else if (et && et->kind == TyKind::Class) {
        // 容器内部同样用 repr 语义（CPython 打印 list/tuple 用元素 repr）。
        VReg cs = dunder_str(ev, et, true);
        if (cs == kNoReg) cs = class_default_str(et);
        if (to_file)
            call_void(RtFn::FileWrite, {load_slot(print_file_slot_, t_file()), cs});
        else
            call_void(RtFn::PrintStr, {cs});
    } else if (!et || et->kind == TyKind::Unknown) {
        // unknown 元素按 repr 语义打印：字符串要带引号（CPython 打印
        // 容器用元素 repr）。原来落到 pa_print_any 的「str 原样输出」，
        // 于是 [1, "a"] 打成 [1, a]。
        if (to_file) {
            print_value_to_file(ev, et, line, col);
        } else {
            call_void(RtFn::PrintAnyRepr, {ev});
        }
    } else if (to_file) {
        print_value_to_file(ev, et, line, col);
    } else {
        print_value(ev, et, line, col);
    }
}

// 遍历一个列表逐元素打印，元素之间用 ", " 分隔。回调拿到元素和下标。
void Lowerer::print_each(VReg list, TypePtr et, bool to_file,
                         const std::function<void(VReg, VReg)>& body) {
    int idx = new_temp_slot(t_int());
    store_slot(idx, const_int(0));
    Inst ln;
    ln.op = IrOp::Len;
    ln.a = list;
    VReg n = def(t_int(), std::move(ln));

    int cond = new_block("print.each.cond");
    int bodyb = new_block("print.each.body");
    int comma = new_block("print.each.comma");
    int elem = new_block("print.each.elem");
    int end = new_block("print.each.end");

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
    br.target = bodyb;
    br.target2 = end;
    emit(std::move(br));

    switch_to(bodyb);
    Inst nz;
    nz.op = IrOp::Cmp;
    nz.cop = CmpKind::Gt;
    nz.a = load_slot(idx, t_int());
    nz.b = const_int(0);
    VReg nz_v = def(t_bool(), std::move(nz));
    Inst need_sep;
    need_sep.op = IrOp::Branch;
    need_sep.a = nz_v;
    need_sep.target = comma;
    need_sep.target2 = elem;
    emit(std::move(need_sep));

    switch_to(comma);
    print_write(", ", to_file);
    {
        Inst jj;
        jj.op = IrOp::Jump;
        jj.target = elem;
        emit(std::move(jj));
    }

    switch_to(elem);
    Inst get;
    get.op = IrOp::IndexLoad;
    get.a = list;
    get.b = load_slot(idx, t_int());
    VReg ev = def(et, std::move(get));
    body(ev, load_slot(idx, t_int()));

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
}

// 空容器打印 empty_text；非空但内容看不清时打印 placeholder。
void Lowerer::print_empty_or_placeholder(VReg v, const std::string& empty_text,
                                         const std::string& placeholder, bool to_file) {
    Inst ln;
    ln.op = IrOp::Len;
    ln.a = v;
    VReg n = def(t_int(), std::move(ln));

    int empty = new_block("print.empty");
    int nonempty = new_block("print.nonempty");
    int end = new_block("print.empty.end");
    Inst c;
    c.op = IrOp::Cmp;
    c.cop = CmpKind::Eq;
    c.a = n;
    c.b = const_int(0);
    VReg cv = def(t_bool(), std::move(c));
    Inst br;
    br.op = IrOp::Branch;
    br.a = cv;
    br.target = empty;
    br.target2 = nonempty;
    emit(std::move(br));

    switch_to(empty);
    print_write(empty_text, to_file);
    {
        Inst jj;
        jj.op = IrOp::Jump;
        jj.target = end;
        emit(std::move(jj));
    }

    switch_to(nonempty);
    print_write(placeholder, to_file);
    {
        Inst jj;
        jj.op = IrOp::Jump;
        jj.target = end;
        emit(std::move(jj));
    }

    switch_to(end);
}

// dict 打印：{key: value, ...}。键和值都按 repr 语义（和 CPython 一致）。
void Lowerer::print_dict(VReg v, TypePtr ty, bool to_file, int line, int col) {
    if (!ty) ty = t_unknown();
    TypePtr kt = ty->sub.empty() ? t_unknown() : ty->sub[0];
    TypePtr vt = ty->sub.size() > 1 ? ty->sub[1] : t_unknown();
    // 元素类型未知（异构容器）：交给运行时的通用 repr（堆探针分派）。
    // int/str 混合能正确打印（0.322 修 `<dict object>`）；unknown 里分不开
    // int/float，float 值仍按位模式打印——项目既有的类型标签边界。
    if (kt->kind == TyKind::Unknown || vt->kind == TyKind::Unknown) {
        VReg s = call_runtime(RtFn::ReprDictAny, t_str(), {v});
        push_temp(s);
        if (to_file)
            call_void(RtFn::FileWrite, {load_slot(print_file_slot_, t_file()), s});
        else
            call_void(RtFn::PrintStr, {s});
        return;
    }
    print_write("{", to_file);
    VReg keys = call_runtime(RtFn::DictKeys, mk_list(kt), {v});
    push_temp(keys);
    VReg vals = call_runtime(RtFn::DictValues, mk_list(vt), {v});
    push_temp(vals);
    print_each(keys, kt, to_file, [&](VReg k, VReg i) {
        print_elem(k, kt, to_file, line, col);
        print_write(": ", to_file);
        Inst gv;
        gv.op = IrOp::IndexLoad;
        gv.a = vals;
        gv.b = i;
        VReg vv = def(vt, std::move(gv));
        print_elem(vv, vt, to_file, line, col);
    });
    print_write("}", to_file);
}

// set 打印：{elem, ...}。空集打印成 set()——{} 在 Python 里是空字典。
void Lowerer::print_set(VReg v, TypePtr ty, bool to_file, int line, int col) {
    if (!ty) ty = t_unknown();
    TypePtr kt = ty->sub.empty() ? t_unknown() : ty->sub[0];
    if (kt->kind == TyKind::Unknown) {
        print_empty_or_placeholder(v, "set()", "<set object>", to_file);
        return;
    }
    VReg keys = call_runtime(RtFn::DictKeys, mk_list(kt), {v});
    push_temp(keys);

    Inst ln;
    ln.op = IrOp::Len;
    ln.a = keys;
    VReg n = def(t_int(), std::move(ln));

    int empty = new_block("print.set.empty");
    int nonempty = new_block("print.set.nonempty");
    int end = new_block("print.set.end");
    Inst c;
    c.op = IrOp::Cmp;
    c.cop = CmpKind::Eq;
    c.a = n;
    c.b = const_int(0);
    VReg cv = def(t_bool(), std::move(c));
    Inst br;
    br.op = IrOp::Branch;
    br.a = cv;
    br.target = empty;
    br.target2 = nonempty;
    emit(std::move(br));

    switch_to(empty);
    print_write("set()", to_file);
    {
        Inst jj;
        jj.op = IrOp::Jump;
        jj.target = end;
        emit(std::move(jj));
    }

    switch_to(nonempty);
    print_write("{", to_file);
    print_each(keys, kt, to_file, [&](VReg k, VReg) {
        print_elem(k, kt, to_file, line, col);
    });
    print_write("}", to_file);
    {
        // end 块建在循环块之前，回落会落进 LICM 的 preheader，必须显式跳。
        Inst jj;
        jj.op = IrOp::Jump;
        jj.target = end;
        emit(std::move(jj));
    }

    switch_to(end);
}

VReg Lowerer::lower_print(Call& c) {
    // print 没有对应的运行时函数，在降级期就地展开成一串输出调用。
    // sep / end 是字符串值，直接交给 PrintStr 原样写出，不再经过格式化。
    Keyword* kw_sep = nullptr;
    Keyword* kw_end = nullptr;
    Keyword* kw_file = nullptr;
    Keyword* kw_flush = nullptr;
    for (auto& kw : c.keywords) {
        if (kw.arg.empty()) {
            err(kw.value->line, kw.value->col, "print() does not take '**kwargs'");
            continue;
        }
        if (kw.arg == "file") {
            kw_file = &kw;
            continue;
        }
        if (kw.arg == "flush") {
            kw_flush = &kw;
            continue;
        }
        if (kw.arg != "sep" && kw.arg != "end") {
            err(kw.value->line, kw.value->col,
                std::format("print() got an unexpected keyword argument '{}'", kw.arg));
            continue;
        }
        TypePtr kt = tc_.type_of(kw.value.get());
        if (kt && kt->kind != TyKind::Str && kt->kind != TyKind::Unknown) {
            err(kw.value->line, kw.value->col,
                std::format("print() '{}' must be a string, got '{}'", kw.arg, type_name(kt)));
            continue;
        }
        (kw.arg == "sep" ? kw_sep : kw_end) = &kw;
    }

    // file=：求值一次落槽，之后的输出都写到它上面。
    int file_slot = -1;
    if (kw_file) {
        TypePtr ft = tc_.type_of(kw_file->value.get());
        // P4（lang_gaps）：print(file=...) 收 FileLike（read/write 方法面
        // 的类实例，io.StringIO 等）；运行期 pa_file_write/flush 按值分派。
        bool filelike = opts_.lang_gaps && ft && ft->kind == TyKind::Class &&
                        ft->cls && ft->cls->method("read") && ft->cls->method("write");
        if (!ft || (ft->kind != TyKind::File && !filelike)) {
            err(kw_file->value->line, kw_file->value->col,
                "print() file must be an open file object");
        } else {
            VReg fv = expr(*kw_file->value);
            file_slot = new_temp_slot(t_file());
            store_slot(file_slot, fv);
        }
    }
    int saved_file_slot = print_file_slot_;
    print_file_slot_ = file_slot;

    // sep 在参数前求值一次并落槽；end 在参数都输出完才求值。
    int sep_slot = -1;
    if (kw_sep) {
        VReg s = expr(*kw_sep->value);
        sep_slot = new_temp_slot(t_str());
        store_slot(sep_slot, s);
    }
    int first = new_temp_slot(t_int());
    store_slot(first, const_int(1));

    auto before_item = [&]() {
        int need = new_block("print.sep");
        int done = new_block("print.item");
        Inst br;
        br.op = IrOp::Branch;
        br.a = load_slot(first, t_int());
        br.target = done;
        br.target2 = need;
        emit(std::move(br));
        switch_to(need);
        if (file_slot >= 0) {
            VReg f = load_slot(file_slot, t_file());
            if (kw_sep)
                call_void(RtFn::FileWrite, {f, load_slot(sep_slot, t_str())});
            else
                call_void(RtFn::FileWrite, {f, const_str(" ")});
        } else if (kw_sep) {
            call_void(RtFn::PrintStr, {load_slot(sep_slot, t_str())});
        } else {
            call_void(RtFn::PrintSep, {});
        }
        Inst j;
        j.op = IrOp::Jump;
        j.target = done;
        emit(std::move(j));
        switch_to(done);
    };
    auto after_item = [&]() { store_slot(first, const_int(0)); };

    for (auto& arg : c.args) {
        if (arg->kind == ExprKind::Starred) {
            auto& st = static_cast<Starred&>(*arg);
            TypePtr sty = tc_.type_of(st.value.get());
            bool ok = sty && (sty->kind == TyKind::List || sty->kind == TyKind::Tuple ||
                              sty->kind == TyKind::Str || sty->kind == TyKind::Dict ||
                              sty->kind == TyKind::Set);
            if (!ok) {
                err(arg->line, arg->col,
                    std::format("cannot expand '{}' in print()", type_name(sty)));
                continue;
            }
            TypePtr ety = t_unknown();
            if (sty->kind == TyKind::Str) ety = t_str();
            else if (!sty->sub.empty()) ety = sty->sub[0];

            VReg seq = expr(*st.value);
            int seq_slot = new_temp_slot(sty);
            store_slot(seq_slot, seq);
            Inst ln;
            ln.op = IrOp::Len;
            ln.a = seq;
            VReg n = def(t_int(), std::move(ln));
            int n_slot = new_temp_slot(t_int());
            store_slot(n_slot, n);
            int idx = new_temp_slot(t_int());
            store_slot(idx, const_int(0));
            int str_cur = sty->kind == TyKind::Str ? new_str_cursor() : -1;

            int cond = new_block("print.star.cond");
            int body = new_block("print.star.body");
            int end = new_block("print.star.end");
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
            before_item();
            VReg sv = load_slot(seq_slot, sty);
            VReg iv = load_slot(idx, t_int());
            VReg ev;
            if (sty->kind == TyKind::Dict || sty->kind == TyKind::Set) {
                ev = call_runtime(RtFn::DictKeyAt, ety, {sv, iv});
            } else if (sty->kind == TyKind::Str) {
                ev = str_char_at(sv, iv, str_cur);
            } else {
                Inst get;
                get.op = (opts_.opt_level >= 1 &&
                          (sty->kind == TyKind::List || sty->kind == TyKind::Tuple))
                             ? IrOp::IndexLoadUnchecked
                             : IrOp::IndexLoad;
                get.a = sv;
                get.b = iv;
                ev = def(ety, std::move(get));
            }
            print_value(ev, ety, arg->line, arg->col);
            after_item();
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
            continue;
        }
        before_item();
        TypePtr at = tc_.type_of(arg.get());
        VReg v = expr(*arg);
        print_value(v, at, arg->line, arg->col);
        after_item();
    }
    if (file_slot >= 0) {
        VReg f = load_slot(file_slot, t_file());
        if (kw_end) {
            call_void(RtFn::FileWrite, {f, expr(*kw_end->value)});
        } else {
            call_void(RtFn::FileWrite, {f, const_str("\n")});
        }
        if (kw_flush) {
            expr(*kw_flush->value);  // 求值保留副作用；按字面量决定是否 flush
            bool skip = false;
            if (kw_flush->value->kind == ExprKind::Constant) {
                const auto& cc = static_cast<const Constant&>(*kw_flush->value);
                if (cc.ck == Constant::CK::Bool && !cc.b) skip = true;
            }
            if (!skip) call_void(RtFn::FileFlush, {f});
        }
    } else if (kw_end) {
        call_void(RtFn::PrintStr, {expr(*kw_end->value)});
    } else {
        call_void(RtFn::PrintEnd, {});
    }
    print_file_slot_ = saved_file_slot;
    return const_none();
}

}  // namespace pyadamas
