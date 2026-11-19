// lower_c_lazy.cpp —— 免写直调：调用点推断 C 编组，dlsym 后端解析符号
//
// from c import X（默认句柄）与 dylib 句柄的属性调用 lib.sym(...) 共享
// 这条路径：没有 .cpya 声明，参数种类按实参静态类型推断，返回种类按
// 调用点推断/标注。契约是 ctypes 式的——调用方对 ABI 负责。
#include "lower_impl.h"

namespace pyadamas {

namespace {

// 实参静态类型 → C 编组种类。None 表示无法推断（调用方报错）。
CKind lazy_arg_kind(TypePtr aty) {
    if (!aty) return CKind::None;
    switch (aty->kind) {
        case TyKind::Float: return CKind::Float;
        case TyKind::Int: return CKind::Int;
        case TyKind::Bool: return CKind::Int;  // 0/1 按机器字传，C 侧截低字节
        case TyKind::Str: return CKind::Str;
        case TyKind::Bytes: return CKind::BufBytes;
        case TyKind::Optional:
            if (!aty->sub.empty() && aty->sub[0]) {
                if (aty->sub[0]->kind == TyKind::Str) return CKind::OptStr;
                if (aty->sub[0]->kind == TyKind::Int) return CKind::Ptr;
            }
            return CKind::None;
        case TyKind::List:
            if (!aty->sub.empty() && aty->sub[0]) {
                if (aty->sub[0]->kind == TyKind::Float) return CKind::BufF64;
                if (aty->sub[0]->kind == TyKind::Int) return CKind::BufI64;
            }
            return CKind::None;
        default: return CKind::None;
    }
}

// 调用点静态类型 → 返回编组种类。typecheck 已保证是标量/可空标量。
CKind lazy_ret_kind(TypePtr rt) {
    if (!rt) return CKind::Int;
    switch (rt->kind) {
        case TyKind::Float: return CKind::Float;
        case TyKind::Str: return CKind::Str;
        case TyKind::Bool: return CKind::Bool;
        case TyKind::None: return CKind::None;
        case TyKind::Optional:
            if (!rt->sub.empty() && rt->sub[0] && rt->sub[0]->kind == TyKind::Str)
                return CKind::OptStr;
            return CKind::Ptr;
        default: return CKind::Int;
    }
}

}  // namespace

VReg Lowerer::lower_c_lazy_call(Call& c, const std::string& symbol, VReg handle) {
    std::vector<VReg> args;
    std::vector<uint8_t> cargs;
    args.reserve(c.args.size());
    cargs.reserve(c.args.size());
    for (auto& ap : c.args) {
        TypePtr aty = tc_.type_of(ap.get());
        VReg v = expr(*ap);
        CKind k = lazy_arg_kind(aty);
        if (k == CKind::None) {
            err(ap->line, ap->col,
                std::format("cannot infer C ABI for argument of type '{}' to '{}'; "
                            "annotate it or use cimport with a .cpya declaration",
                            type_name(aty), symbol));
            return const_none();
        }
        switch (k) {
            case CKind::Float: v = coerce(v, aty, t_float()); break;
            case CKind::Str: v = coerce(v, aty, t_str()); break;
            case CKind::OptStr:
                if (aty && aty->kind != TyKind::None) v = coerce(v, aty, t_str());
                break;
            case CKind::Ptr:
                if (aty && aty->kind != TyKind::None) v = coerce(v, aty, t_int());
                break;
            case CKind::Int: v = coerce(v, aty, t_int()); break;
            default: break;  // Buf*：对象指针原样交给 codegen 取 data
        }
        args.push_back(v);
        cargs.push_back(static_cast<uint8_t>(k));
    }
    for (auto& kw : c.keywords) expr(*kw.value);  // typecheck 已拒绝，防御求值

    CKind rk = lazy_ret_kind(tc_.type_of(&c));
    const int lazy = handle == kNoReg ? 1 : 2;
    // 按 (符号, 绑定方式, 返回种类) 去重；实参编组逐调用点记在 cargs，
    // IRExtern.params 留空（codegen 的 kind_at 优先读 cargs）。
    int idx = -1;
    for (size_t i = 0; i < mod_.externs.size(); ++i) {
        const IRExtern& e = mod_.externs[i];
        if (e.lazy == lazy && e.symbol == symbol && e.ret == rk) {
            idx = static_cast<int>(i);
            break;
        }
    }
    if (idx < 0) {
        IRExtern ent;
        ent.symbol = symbol;
        ent.ret = rk;
        ent.lazy = lazy;
        int ni = -1;
        for (size_t i = 0; i < mod_.c_lazy_syms.size(); ++i)
            if (mod_.c_lazy_syms[i] == symbol) { ni = static_cast<int>(i); break; }
        if (ni < 0) {
            mod_.c_lazy_syms.push_back(symbol);
            ni = static_cast<int>(mod_.c_lazy_syms.size()) - 1;
        }
        ent.sym_name = ni;
        mod_.externs.push_back(std::move(ent));
        idx = static_cast<int>(mod_.externs.size()) - 1;
    }

    Inst in;
    in.op = IrOp::CallExtern;
    in.idx = Operand::ext(idx);
    in.a = handle;  // lazy==2：运行期 dlopen 句柄
    in.args = std::move(args);
    in.cargs = std::move(cargs);
    // 同 lower_cimport_call：类型先取出来，别在 std::move(in) 的同一实参
    // 列表里读 in.ty（求值顺序未指定，GCC 会读到被移走的空指针）。
    TypePtr out_ty = ckind_type(rk);
    in.ty = out_ty;
    return def_heap(out_ty, std::move(in));
}

}  // namespace pyadamas
