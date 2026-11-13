// lower_method_containers.cpp —— dict / set / list / tuple 的方法调用降级
#include "lower_impl.h"

namespace pyadamas {

VReg Lowerer::lower_method_dict(Attribute& at, Call& c, VReg recv, TypePtr recv_ty) {
    TypePtr kt = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
    TypePtr vt = recv_ty->sub.size() > 1 ? recv_ty->sub[1] : t_unknown();
    if (at.attr == "get") {
        if (!c.keywords.empty()) {
            err(c.line, c.col, "dict.get() does not take keyword arguments");
            return const_none();
        }
        if (c.args.size() != 1 && c.args.size() != 2) {
            err(c.line, c.col, "dict.get() takes 1 or 2 arguments");
            return const_none();
        }
        VReg key = expr(*c.args[0]);
        key = coerce(key, tc_.type_of(c.args[0].get()), kt);
        if (c.args.size() == 1)
            // 缺失时返回 None（PyAdamas 里 None 就是 0）
            return call_runtime(RtFn::DictGetOpt, mk_optional(vt), {recv, key});
        VReg def = expr(*c.args[1]);
        def = coerce(def, tc_.type_of(c.args[1].get()), vt);
        return call_runtime(RtFn::DictGetOr, vt, {recv, key, def});
    }
    if (!c.keywords.empty()) {
        err(c.line, c.col,
            std::format("dict.{}() does not take keyword arguments", at.attr));
        return const_none();
    }
    if (at.attr == "most_common") {
        if (c.args.size() > 1) {
            err(c.line, c.col, "dict.most_common() takes at most one argument");
            return const_none();
        }
        VReg n = const_int(-1);
        if (c.args.size() == 1)
            n = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
        TypePtr rt = mk_list(mk_tuple({kt, t_int()}));
        VReg v = call_runtime(RtFn::CollMostCommon, rt, {recv, n});
        push_temp(v);
        return v;
    }
    if (at.attr == "keys" || at.attr == "values" || at.attr == "items") {
        if (!c.args.empty()) {
            err(c.line, c.col, std::format("dict.{}() takes no arguments", at.attr));
            return const_none();
        }
        TypePtr rt = tc_.type_of(&c);
        if (!rt || rt->kind != TyKind::List) {
            rt = at.attr == "keys" ? mk_list(kt)
                 : at.attr == "values" ? mk_list(vt)
                                       : mk_list(mk_tuple({kt, vt}));
        }
        RtFn fn = at.attr == "keys" ? RtFn::DictKeys
                  : at.attr == "values" ? RtFn::DictValues
                                        : RtFn::DictItems;
        VReg v = call_runtime(fn, rt, {recv});
        push_temp(v);
        return v;
    }
    if (at.attr == "update") {
        if (c.args.empty()) return const_none();
        if (c.args.size() != 1) {
            err(c.line, c.col, "dict.update() takes at most one argument");
            return const_none();
        }
        VReg other = expr(*c.args[0]);
        TypePtr ot = tc_.type_of(c.args[0].get());
        if (!ot || (ot->kind != TyKind::Dict && ot->kind != TyKind::Unknown)) {
            err(c.args[0]->line, c.args[0]->col,
                "dict.update() expects a dict argument");
            return const_none();
        }
        call_void(RtFn::DictUpdate, {recv, other});
        return const_none();
    }
    if (at.attr == "pop") {
        if (c.args.size() != 1 && c.args.size() != 2) {
            err(c.line, c.col, "dict.pop() takes 1 or 2 arguments");
            return const_none();
        }
        VReg key = expr(*c.args[0]);
        key = coerce(key, tc_.type_of(c.args[0].get()), kt);
        VReg has = const_int(0);
        VReg def = const_int(0);
        if (c.args.size() == 2) {
            has = const_int(1);
            def = expr(*c.args[1]);
            def = coerce(def, tc_.type_of(c.args[1].get()), vt);
            // 运行时把默认值原样返回；补一份引用，让返回值统一按
            // 「新引用」进临时栈。
            retain_elem(def, own_kind(vt));
        }
        VReg v = call_runtime(RtFn::DictPop, vt, {recv, key, has, def});
        if (ir_is_heap(vt)) push_temp(v);
        return v;
    }
    if (at.attr == "popitem") {
        if (!c.args.empty()) {
            err(c.line, c.col, "dict.popitem() takes no arguments");
            return const_none();
        }
        TypePtr rt = tc_.type_of(&c);
        if (!rt || rt->kind != TyKind::Tuple) rt = mk_tuple({kt, vt});
        VReg v = call_runtime(RtFn::DictPopItem, rt, {recv});
        push_temp(v);
        return v;
    }
    if (at.attr == "clear") {
        if (!c.args.empty()) {
            err(c.line, c.col, "dict.clear() takes no arguments");
            return const_none();
        }
        call_void(RtFn::DictClear, {recv});
        return const_none();
    }
    if (at.attr == "setdefault") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "dict.setdefault() takes exactly two arguments");
            return const_none();
        }
        VReg key = expr(*c.args[0]);
        key = coerce(key, tc_.type_of(c.args[0].get()), kt);
        VReg def = expr(*c.args[1]);
        def = coerce(def, tc_.type_of(c.args[1].get()), vt);
        int add_b = new_block("dict.setdefault.add");
        int done_b = new_block("dict.setdefault.done");
        VReg has = call_runtime(RtFn::DictContains, t_bool(), {recv, key});
        Inst br;
        br.op = IrOp::Branch;
        br.a = has;
        br.target = done_b;
        br.target2 = add_b;
        emit(std::move(br));
        switch_to(add_b);
        retain_elem(key, own_kind(kt));
        retain_elem(def, own_kind(vt));
        call_void(RtFn::DictSet, {recv, key, def});
        Inst j;
        j.op = IrOp::Jump;
        j.target = done_b;
        emit(std::move(j));
        switch_to(done_b);
        VReg v = call_runtime(RtFn::DictGet, vt, {recv, key});
        if (ir_is_heap(vt)) push_temp(v);
        return v;
    }
    if (at.attr == "copy") {
        if (!c.args.empty()) {
            err(c.line, c.col, "dict.copy() takes no arguments");
            return const_none();
        }
        VReg v = call_runtime(RtFn::DictCopy, recv_ty, {recv});
        push_temp(v);
        return v;
    }
    err(c.line, c.col,
        std::format("dict method '{}' is not supported by the code generator yet", at.attr));
    return const_none();
}

VReg Lowerer::lower_method_set(Attribute& at, Call& c, VReg recv, TypePtr recv_ty) {
    TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
    if (!c.keywords.empty()) {
        err(c.line, c.col,
            std::format("set.{}() does not take keyword arguments", at.attr));
        return const_none();
    }
    if (at.attr == "add") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "set.add() takes exactly one argument");
            return const_none();
        }
        VReg v = expr(*c.args[0]);
        v = coerce(v, tc_.type_of(c.args[0].get()), et);
        retain_elem(v, own_kind(et));
        call_void(RtFn::DictSet, {recv, v, const_int(0)});
        return const_none();
    }
    if (at.attr == "update") {
        if (c.args.empty()) return const_none();
        if (c.args.size() != 1) {
            err(c.line, c.col, "set.update() takes at most one argument");
            return const_none();
        }
        VReg other = expr(*c.args[0]);
        TypePtr ot = tc_.type_of(c.args[0].get());
        if (!ot || (ot->kind != TyKind::Set && ot->kind != TyKind::Unknown)) {
            err(c.args[0]->line, c.args[0]->col, "set.update() expects a set argument");
            return const_none();
        }
        call_void(RtFn::DictUpdate, {recv, other});
        return const_none();
    }
    if (at.attr == "remove" || at.attr == "discard") {
        if (c.args.size() != 1) {
            err(c.line, c.col,
                std::format("set.{}() takes exactly one argument", at.attr));
            return const_none();
        }
        VReg v = expr(*c.args[0]);
        v = coerce(v, tc_.type_of(c.args[0].get()), et);
        call_void(RtFn::DictRemove,
                  {recv, v, const_int(at.attr == "discard" ? 1 : 0)});
        return const_none();
    }
    if (at.attr == "pop") {
        if (!c.args.empty()) {
            err(c.line, c.col, "set.pop() takes no arguments");
            return const_none();
        }
        VReg v = call_runtime(RtFn::DictPopLast, et, {recv});
        if (ir_is_heap(et)) push_temp(v);
        return v;
    }
    if (at.attr == "clear") {
        if (!c.args.empty()) {
            err(c.line, c.col, "set.clear() takes no arguments");
            return const_none();
        }
        call_void(RtFn::DictClear, {recv});
        return const_none();
    }
    if (at.attr == "copy") {
        if (!c.args.empty()) {
            err(c.line, c.col, "set.copy() takes no arguments");
            return const_none();
        }
        VReg v = call_runtime(RtFn::DictCopy, recv_ty, {recv});
        push_temp(v);
        return v;
    }
    err(c.line, c.col,
        std::format("set method '{}' is not supported by the code generator yet", at.attr));
    return const_none();
}

VReg Lowerer::lower_method_seq(Attribute& at, Call& c, VReg recv, TypePtr recv_ty) {
    TypePtr et = recv_ty->sub.empty() ? t_unknown() : recv_ty->sub[0];
    int ek = key_kind(et);        // in/index/count 的比较种类
    int lk = list_kind(recv_ty);  // 容器元素的堆种类
    bool is_list = recv_ty->kind == TyKind::List;
    if (!c.keywords.empty()) {
        err(c.line, c.col,
            std::format("{}.{}() does not take keyword arguments",
                        is_list ? "list" : "tuple", at.attr));
        return const_none();
    }
    auto elem_arg = [&](size_t i) {
        VReg v = expr(*c.args[i]);
        return coerce(v, tc_.type_of(c.args[i].get()), et);
    };
    if (at.attr == "append" && is_list) {
        if (c.args.size() != 1) {
            err(c.line, c.col, "list.append() takes exactly one argument");
            return const_none();
        }
        // 运行时不再做隐式 retain：容器元素的所有权由降级器按静态类型决定。
        VReg v = expr(*c.args[0]);
        bool owned = move_fresh(v, et);
        store_list_elem(recv, recv_ty, v, owned);
        call_void(RtFn::ListAppend, {recv, v});
        return const_none();
    }
    if (at.attr == "extend" && is_list) {
        if (c.args.size() != 1) {
            err(c.line, c.col, "list.extend() takes exactly one argument");
            return const_none();
        }
        VReg other = expr(*c.args[0]);
        TypePtr ot = tc_.type_of(c.args[0].get());
        if (!ot || (ot->kind != TyKind::List && ot->kind != TyKind::Tuple)) {
            err(c.args[0]->line, c.args[0]->col, "list.extend() expects a list");
            return const_none();
        }
        call_void(RtFn::ListExtend, {recv, other});
        return const_none();
    }
    if (at.attr == "insert" && is_list) {
        if (c.args.size() != 2) {
            err(c.line, c.col, "list.insert() takes exactly two arguments");
            return const_none();
        }
        VReg idx = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
        VReg v = elem_arg(1);
        bool owned = move_fresh(v, et);
        store_list_elem(recv, recv_ty, v, owned);
        call_void(RtFn::ListInsert, {recv, idx, v});
        return const_none();
    }
    if (at.attr == "remove" && is_list) {
        if (c.args.size() != 1) {
            err(c.line, c.col, "list.remove() takes exactly one argument");
            return const_none();
        }
        VReg v = elem_arg(0);
        VReg idx = call_runtime(RtFn::ListIndex, t_int(), {recv, v, const_int(ek)});
        call_void(RtFn::ListDelIndex, {recv, idx});
        return const_none();
    }
    if (at.attr == "pop" && is_list) {
        if (c.args.size() > 1) {
            err(c.line, c.col, "list.pop() takes at most one argument");
            return const_none();
        }
        VReg idx;
        if (c.args.empty()) {
            Inst len;
            len.op = IrOp::Len;
            len.a = recv;
            VReg n = def(t_int(), std::move(len));
            Inst sub;
            sub.op = IrOp::Bin;
            sub.bop = BinKind::Sub;
            sub.a = n;
            sub.b = const_int(1);
            idx = def(t_int(), std::move(sub));
        } else {
            idx = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
        }
        Inst get;
        get.op = IrOp::IndexLoad;
        get.a = recv;
        get.b = idx;
        VReg v = def(et, std::move(get));
        // kind 1/2/3 的列表都持有元素引用，删除时会 dec；把返回值领
        // 出来，否则返回值会随删除变成悬空指针（3 由运行期定型，也要领）。
        if (lk != 0 && ir_is_heap(et)) {
            rc_inc(v);
            push_temp(v);
        } else if (lk != 0 && (!et || et->kind == TyKind::Unknown)) {
            rc_inc_unknown(v);
            call_void(RtFn::RcTempUnknown, {v});
        }
        call_void(RtFn::ListDelIndex, {recv, idx});
        return v;
    }
    if (at.attr == "clear" && is_list) {
        if (!c.args.empty()) {
            err(c.line, c.col, "list.clear() takes no arguments");
            return const_none();
        }
        call_void(RtFn::ListSetLen, {recv, const_int(0)});
        return const_none();
    }
    if (at.attr == "index") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "index() takes exactly one argument");
            return const_none();
        }
        VReg v = elem_arg(0);
        return call_runtime(RtFn::ListIndex, t_int(), {recv, v, const_int(ek)});
    }
    if (at.attr == "count") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "count() takes exactly one argument");
            return const_none();
        }
        VReg v = elem_arg(0);
        return call_runtime(RtFn::ListCount, t_int(), {recv, v, const_int(ek)});
    }
    if (at.attr == "copy" && is_list) {
        if (!c.args.empty()) {
            err(c.line, c.col, "list.copy() takes no arguments");
            return const_none();
        }
        VReg v = call_runtime(RtFn::ListCopy, recv_ty, {recv});
        push_temp(v);
        return v;
    }
    if (at.attr == "reverse" && is_list) {
        if (!c.args.empty()) {
            err(c.line, c.col, "list.reverse() takes no arguments");
            return const_none();
        }
        call_void(RtFn::ListReverse, {recv});
        return const_none();
    }
    if (at.attr == "sort" && is_list) {
        if (!c.args.empty() || !c.keywords.empty()) {
            err(c.line, c.col, "list.sort() key=/reverse= are not supported yet");
            return const_none();
        }
        call_void(RtFn::ListSort, {recv, const_int(sort_kind(et))});
        return const_none();
    }
    err(c.line, c.col, std::format("method '{}' is not supported by the code generator yet",
                                   at.attr));
    return const_none();
}

}  // namespace pyadamas
