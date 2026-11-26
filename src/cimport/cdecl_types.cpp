#include "cimport/cdecl_model.h"

#include <format>
#include <optional>
#include <string>

namespace pyadamas {
namespace cdetail {

std::optional<std::string> cpya_type(const CType& t, bool param, std::string& why) {
    switch (t.k) {
        case CType::Void:
            if (param) {
                why = "void parameter";
                return std::nullopt;
            }
            return std::string("None");
        case CType::Bool: return std::string("bool");
        case CType::I8:
        case CType::I16:
        case CType::I32: return std::string(kInt32);
        case CType::U8:
        case CType::U16:
        case CType::U32: return std::string(kUInt32);
        case CType::U64:
        case CType::I64: return std::string("int");
        case CType::Double: return std::string("float");
        case CType::Float: return std::string("float32");
        case CType::Struct: return t.name;
        case CType::Union:
            why = "union by value is not supported";
            return std::nullopt;
        case CType::Ptr: {
            if (!t.inner) return std::string("ptr");
            const CType& in = *t.inner;
            if (in.k == CType::Void) return std::string("ptr");
            if (in.k == CType::Struct) return std::format("ptr[{}]", in.name);
            if (in.k == CType::Union) return std::string("ptr");
            if (in.k == CType::Func) return std::string("ptr");
            if (in.k == CType::I8 && in.is_const) return std::string("str");
            if (in.k == CType::I8 || in.k == CType::U8) return std::string("ptr");
            if (in.k == CType::I16 || in.k == CType::U16) return std::string("ptr");
            if (in.k == CType::Double) return std::string("ptr[float]");
            if (in.k == CType::Bool) return std::string("ptr[bool]");
            if (in.k == CType::I32) return std::string("ptr[int32]");
            if (in.k == CType::U32) return std::string("ptr[uint32]");
            if (in.k == CType::I64 || in.k == CType::U64) return std::string("ptr[int]");
            return std::string("ptr");
        }
        case CType::Func: {
            // 回调跳板暂不做单精度提升：含 C float 的回调签名整体跳过，
            // 避免生成 .cpya 无法解析的 fn(float32, ...)。
            for (const auto& p : t.ps)
                if (p.k == CType::Float) {
                    why = "C float in callback signature";
                    return std::nullopt;
                }
            if (t.ret && t.ret->k == CType::Float) {
                why = "C float callback return";
                return std::nullopt;
            }
            // C 回调 → Python 的 Callable[[params], ret]（.cpya 注解里没有 -> 语法）。
            std::string s = "Callable[[";
            for (size_t i = 0; i < t.ps.size(); ++i) {
                std::string why2;
                auto p = cpya_type(t.ps[i], true, why2);
                if (!p) {
                    why = "callback parameter: " + why2;
                    return std::nullopt;
                }
                if (i) s += ", ";
                // cimport 的回调参数只接受标量或 ptr；`const char*` 生成 str
                // 会让整个 .cpya 解析失败，这里退化成 ptr。
                s += (*p == "str") ? "ptr" : *p;
            }
            if (t.varargs) {
                why = "varargs callback";
                return std::nullopt;
            }
            s += "], ";
            if (t.ret && t.ret->k != CType::Void) {
                std::string why2;
                auto r = cpya_type(*t.ret, false, why2);
                if (!r) {
                    why = "callback return: " + why2;
                    return std::nullopt;
                }
                s += *r;
            } else {
                s += "None";
            }
            s += "]";
            return s;
        }
        default:
            why = "unknown type";
            return std::nullopt;
    }
}

std::optional<std::string> cpya_field_type(const CType& t, std::string& why) {
    switch (t.k) {
        case CType::Bool: return std::string("bool");
        case CType::I8: return std::string("char");
        case CType::I16: return std::string("short");
        case CType::U8: return std::string("uchar");
        case CType::U16: return std::string("ushort");
        case CType::I32: return std::string(kInt32);
        case CType::U32: return std::string(kUInt32);
        case CType::I64:
        case CType::U64: return std::string("int");
        case CType::Double: return std::string("float");
        case CType::Float: return std::string("float32");
        case CType::Ptr: return std::string("ptr");
        // .cpya 现在支持 struct/union 按值字段与数组（布局按 C 规则算），
        // 生成器只要把类型名写出来即可。
        case CType::Struct:
        case CType::Union:
            if (!t.name.empty()) return t.name;
            why = "anonymous struct/union field";
            return std::nullopt;
        default:
            why = "field type is not a fixed-width scalar/pointer";
            return std::nullopt;
    }
}

}  // namespace cdetail
}  // namespace pyadamas
