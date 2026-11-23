// lower_builtin_container.cpp —— 内置函数：容器构造
#include "lower_impl.h"

namespace pyadamas {

VReg Lowerer::lower_builtin_container(const std::string& name, Call& c) {
    if (name == "list" || name == "tuple") {
        TypePtr ty = tc_.type_of(&c);
        bool as_tuple = name == "tuple";
        auto result_ty = [&]() -> TypePtr {
            if (ty) return ty;
            return as_tuple ? mk_tuple({}) : mk_list(t_unknown());
        };
        if (c.args.empty()) {
            if (as_tuple) {
                Inst mt;
                mt.op = IrOp::MakeTuple;
                mt.imm = list_kind_filled(ty);
                mt.ty = result_ty();
                return def_heap(result_ty(), std::move(mt));
            }
            VReg l = call_runtime(RtFn::ListNew, result_ty(),
                                  {const_int(0), const_int(list_kind(ty))});
            push_temp(l);
            return l;
        }
        if (c.args.size() != 1) {
            err(c.line, c.col, std::format("{}() takes at most one argument", name));
            return const_none();
        }
        TypePtr xt = tc_.type_of(c.args[0].get());
        // list(Color)：enum 类对象物化成成员表。
        if (xt && xt->kind == TyKind::ClassObj && !xt->sub.empty() && xt->sub[0] &&
            xt->sub[0]->cls && xt->sub[0]->cls->is_enum)
            return enum_member_list(xt->sub[0]->cls);
        if (xt && xt->kind == TyKind::Deque) {
            VReg d = expr(*c.args[0]);
            VReg xs = call_runtime(RtFn::DequeToList, mk_list(t_int()), {d});
            push_temp(xs);
            return xs;
        }
        // unknown（动态属性读出的容器等）：运行期按种类物化。
        if (!xt || xt->kind == TyKind::Unknown) {
            VReg x = expr(*c.args[0]);
            VReg l = call_runtime(RtFn::AnyIterList, mk_list(t_unknown()), {x});
            push_temp(l);
            return retype(l, result_ty());
        }
        if (xt->kind != TyKind::List && xt->kind != TyKind::Tuple &&
            xt->kind != TyKind::Str && xt->kind != TyKind::Bytes &&
            xt->kind != TyKind::Dict &&
            xt->kind != TyKind::Set && xt->kind != TyKind::File &&
            xt->kind != TyKind::Range && xt->kind != TyKind::Generator) {
            err(c.args[0]->line, c.args[0]->col,
                std::format("{}() cannot iterate over '{}'", name, type_name(xt)));
            return const_none();
        }
        TypePtr et = t_unknown();
        VReg x = seq_as_list(c.args[0].get(), et);
        // str/dict/set/生成器 走 seq_as_list 时已经是新建的列表，不用再拷。
        bool fresh = xt->kind == TyKind::Str || xt->kind == TyKind::Bytes ||
                     xt->kind == TyKind::Dict ||
                     xt->kind == TyKind::Set || xt->kind == TyKind::Generator;
        VReg l = fresh ? retype(x, result_ty())
                       : call_runtime(RtFn::ListCopy, result_ty(), {x});
        if (!fresh) push_temp(l);
        return as_tuple ? retype(l, result_ty()) : l;
    }

    if (name == "dict") {
        TypePtr ty = tc_.type_of(&c);
        // `d: dict[str, int] = dict()`：同 set()，标注决定 key/value 种类。
        if (pending_ctor_ty_ && pending_ctor_ty_->kind == TyKind::Dict) {
            ty = pending_ctor_ty_;
        }
        if (!ty || ty->kind != TyKind::Dict) ty = mk_dict(t_unknown(), t_unknown());
        TypePtr kt = t_unknown(), vt = t_unknown();
        if (!ty->sub.empty()) kt = ty->sub[0];
        if (ty->sub.size() > 1) vt = ty->sub[1];
        // big 键走 hash_key_kind 的 9（raw-or-big）。
        if (!c.args.empty()) {
            if (c.args.size() != 1 || !c.keywords.empty()) {
                err(c.line, c.col, "dict() takes at most one positional argument");
                return const_none();
            }
            TypePtr xt = tc_.type_of(c.args[0].get());
            if (!xt || (xt->kind != TyKind::List && xt->kind != TyKind::Tuple &&
                        xt->kind != TyKind::Set && xt->kind != TyKind::Generator &&
                        xt->kind != TyKind::File && xt->kind != TyKind::Range)) {
                err(c.args[0]->line, c.args[0]->col,
                    std::format("dict() cannot iterate over '{}'", type_name(xt)));
                return const_none();
            }
            TypePtr se = t_unknown();
            VReg xs = seq_as_list(c.args[0].get(), se);
            auto [hslot, eslot] = dunder_key_slots(kt);
            VReg d = call_runtime(RtFn::DictFromPairs, ty,
                                  {xs, const_int(hash_key_kind(kt)), const_int(own_kind(kt)),
                                   const_int(own_kind(vt)), const_int(hslot),
                                   const_int(eslot)});
            push_temp(d);
            return d;
        }
        if (c.keywords.empty()) {
            auto [hslot, eslot] = dunder_key_slots(kt);
            VReg d = call_runtime(RtFn::DictNew, ty,
                                  {const_int(hash_key_kind(kt)), const_int(own_kind(kt)),
                                   const_int(own_kind(vt)), const_int(hslot),
                                   const_int(eslot)});
            push_temp(d);
            return d;
        }
        // dict(a=1, b=2)：关键字是字符串键。
        int vo = own_kind(vt);
        VReg d = call_runtime(RtFn::DictNew, ty,
                              {const_int(1), const_int(1), const_int(vo),
                               const_int(-1), const_int(-1)});
        push_temp(d);
        for (auto& kw : c.keywords) {
            if (kw.arg.empty()) {
                err(kw.value->line, kw.value->col, "'**' is not supported in dict()");
                continue;
            }
            VReg v = expr(*kw.value);
            v = coerce(v, tc_.type_of(kw.value.get()), vt);
            retain_elem(v, vo);
            call_void(RtFn::DictSet, {d, const_str(kw.arg), v});
        }
        return d;
    }

    if (name == "__pa_any_items") {
        // P2 内部 intrinsic（stdlib/pprint.py）：unknown dict → [(k,v)...]。
        // 第二个实参 0 是「无 tag」哨兵：PYADAMAS_TYPE_TAGS 开时 codegen 会
        // 用运行期 tag 覆盖它；关时按探针口径。
        if (c.args.size() != 1) {
            err(c.line, c.col, "__pa_any_items() takes exactly one argument");
            return const_none();
        }
        VReg v = expr(*c.args[0]);
        TypePtr lt = mk_list(t_unknown());
        VReg r = call_runtime(RtFn::AnyDictItems, lt, {v, const_int(0)});
        push_temp(r);
        return r;
    }

    if (name == "set" || name == "frozenset") {
        TypePtr ty = tc_.type_of(&c);
        // `s: set[str] = set()`：调用推断只有 set[unknown]，标注带来的
        // 元素类型由 AnnAssign 经 pending_ctor_ty_ 挂过来（N17）。
        if (pending_ctor_ty_ && pending_ctor_ty_->kind == TyKind::Set) {
            ty = pending_ctor_ty_;
        }
        if (!ty || ty->kind != TyKind::Set) ty = mk_set(t_unknown());
        TypePtr et = ty->sub.empty() ? t_unknown() : ty->sub[0];
        // big 键走 hash_key_kind 的 9（raw-or-big）。
        int kk = hash_key_kind(et), ko = own_kind(et);
        if (c.args.empty()) {
            auto [hslot, eslot] = dunder_key_slots(et);
            VReg d = call_runtime(RtFn::DictNew, ty,
                                  {const_int(kk), const_int(ko), const_int(0),
                                   const_int(hslot), const_int(eslot)});
            push_temp(d);
            return d;
        }
        if (c.args.size() != 1) {
            err(c.line, c.col, std::format("{}() takes at most one argument", name));
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
                std::format("{}() cannot iterate over '{}'", name, type_name(xt)));
            return const_none();
        }
        TypePtr se = t_unknown();
        VReg x = seq_as_list(c.args[0].get(), se);
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::SetFromList);
        auto [hslot, eslot] = dunder_key_slots(et);
        in.args = {x, const_int(kk), const_int(ko), const_int(hslot), const_int(eslot)};
        in.ty = ty;
        VReg d = def(ty, std::move(in));
        push_temp(d);
        return d;
    }

    if (name == "slice") {
        // 下标位置的形式在脱糖阶段已经折成切片语法，能走到这里说明是当
        // 独立值用——PyAdamas 没有运行期的 slice 对象。
        err(c.line, c.col,
            "slice() is only supported as a subscript, e.g. xs[slice(1, 3)]");
        for (auto& a : c.args) expr(*a);
        return const_none();
    }

    return kNoReg;
}

}  // namespace pyadamas
