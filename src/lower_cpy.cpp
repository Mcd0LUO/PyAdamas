// lower_cpy.cpp —— CPython 桥（from python import <module>）的调用降级
//
// 语义与协议见 docs/cpybridge-design.md。要点：
//   - 模块代理没有编译期签名：任意属性调用都降级成一次 rt_cpy IPC。
//   - 参数按静态类型生成自定界描述符（rt_json 同款语法，扩展 y=bytes、
//     ?=unknown 动态探针、dict 带键/值双描述符），运行时按描述符把实参
//     marshalling 成带标签的 JSON 帧。
//   - 结果是 unknown：堆/标量都有可能，用 RcTempUnknown 接管初始引用。
#include "lower_impl.h"

namespace pyadamas {

bool Lowerer::cpy_desc(const TypePtr& t, std::string& out, int line, int col) {
    // unknown 不是错误：运行时按堆注册表动态探针 marshalling
    // （int/bool/None 与 str/bytes 在 unknown 里分不开，见设计文档）。
    if (!t || t->kind == TyKind::Unknown) {
        out = "?";
        return true;
    }
    switch (t->kind) {
        case TyKind::None: out = "n"; return true;
        case TyKind::Bool: out = "b"; return true;
        case TyKind::Int: out = "i"; return true;
        case TyKind::Float: out = "f"; return true;
        case TyKind::Str: out = "s"; return true;
        case TyKind::Bytes: out = "y"; return true;
        case TyKind::List: {
            std::string inner;
            TypePtr el = t->sub.empty() ? t_unknown() : t->sub[0];
            if (!cpy_desc(el, inner, line, col)) return false;
            out = "(l" + inner + ")";
            return true;
        }
        case TyKind::Tuple: {
            if (t->variadic) {
                err(line, col, "cpybridge: variable-length tuples cannot be marshalled");
                return false;
            }
            std::string s = "(t";
            for (auto& sub : t->sub) {
                std::string inner;
                if (!cpy_desc(sub, inner, line, col)) return false;
                s += inner;
            }
            out = s + ")";
            return true;
        }
        case TyKind::Dict: {
            if (t->sub.size() < 2) {
                err(line, col, "cpybridge: dict type is unknown");
                return false;
            }
            std::string k, v;
            if (!cpy_desc(t->sub[0], k, line, col)) return false;
            if (!cpy_desc(t->sub[1], v, line, col)) return false;
            out = "(d" + k + v + ")";
            return true;
        }
        default:
            err(line, col,
                std::format("cannot marshal '{}' to CPython (cpybridge supports "
                            "None/bool/int/float/str/bytes/list/tuple/dict)",
                            type_name(t)));
            return false;
    }
}

VReg Lowerer::lower_cpy_call(const std::string& proxy_name, const std::string& attr, Call& c) {
    if (!c.keywords.empty()) {
        err(c.line, c.col,
            std::format("CPython bridge call '{}()' does not support keyword arguments yet",
                        attr));
        return const_none();
    }
    // 代理名 "python.<module>" → 真实 CPython 模块名。
    std::string module = proxy_name.starts_with("python.") ? proxy_name.substr(7) : proxy_name;

    // 实参装进一个 kind 0（不持有引用）的临时打包列表，与 itertools
    // pack_seqs 同口径：元素引用仍归原主（槽/临时栈），pa_cpy_call 在
    // 同一表达式内消费完，销毁时不 dec。描述符逐个拼接（自定界）。
    VReg args = call_runtime(RtFn::ListNew, mk_list(t_unknown()),
                             {const_int(static_cast<int64_t>(c.args.size())), const_int(0)});
    push_temp(args);
    std::string descs;
    for (auto& arg : c.args) {
        TypePtr at = tc_.type_of(arg.get());
        std::string d;
        if (!cpy_desc(at, d, arg->line, arg->col)) return const_none();
        descs += d;
        VReg v = expr(*arg);
        // heap 恒传 0（而非 heap_arg(at)）：打包列表是 kind 0 借用表，
        // 销毁绝不能 dec 元素。0.303 的 pa_list_append 会做种类维护——
        // kind0 表 append 堆值（heap=1，或 heap=2 探针命中）会升级成
        // kind2，销毁遂按注册表 dec 借来的引用 → 过度释放。itertools
        // pack_seqs 传 heap=2 之所以无恙，是那些模块走 index_readonly
        // （探针恒 0、不升级）；而桥模块被两级分析强制保留 unknown 索引
        // （CpyCall 拉黑），探针真命中，必升级。传 heap=0 使 kind==heap，
        // 跳过升级、保持 kind0 借用语义。marshalling 由描述符驱动
        // （pa_cpy_call 读 args->data[i] + descs），与 elem_kind 无关。
        call_void(RtFn::ListAppend, {args, v, const_int(0)});
    }
    VReg r = call_runtime(RtFn::CpyCall, t_unknown(),
                          {const_str(module), const_str(attr), args, const_str(descs)});
    // 结果可能是堆对象（str/list/dict/float 盒）也可能是裸标量：
    // 按注册表接管初始引用，标量空操作。
    call_void(RtFn::RcTempUnknown, {r});
    return r;
}

}  // namespace pyadamas
