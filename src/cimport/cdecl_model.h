#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pyadamas {
namespace cdetail {  // 与 common/ast.h 的 StructDef（AST 节点）区分开

// cdecl 解析产物的数据模型。顺序固定：CType -> StructFieldDef -> StructDef。
// ── 类型 ───────────────────────────────────────────────────────
struct CType {
    enum K {
        Void, Bool, I8, U8, I16, U16, I32, U32, I64, U64, Double, Float,
        Ptr, Struct, Union, Func, Unknown
    } k = Unknown;
    std::string name;               // Struct/Union
    std::shared_ptr<CType> inner;   // Ptr
    std::vector<CType> ps;          // Func
    std::shared_ptr<CType> ret;     // Func
    bool varargs = false;
    bool is_const = false;
};

// ── 结构体 / 枚举定义 ──────────────────────────────────────────
struct StructFieldDef {
    std::string name;
    CType type;
    std::vector<int64_t> dims;  // 非空 = 定长数组（行优先）
};

struct StructDef {
    std::string name;
    std::vector<StructFieldDef> fields;
    bool is_union = false;
    bool packed = false;
    bool ok = true;
    std::string why;
    int line = 0;
};

inline constexpr const char* kInt32 = "int32";
inline constexpr const char* kUInt32 = "uint32";

// C 类型到 .cpya 类型表达式
std::optional<std::string> cpya_type(const CType& t, bool param, std::string& why);
std::optional<std::string> cpya_field_type(const CType& t, std::string& why);

}  // namespace cdetail
}  // namespace pyadamas
