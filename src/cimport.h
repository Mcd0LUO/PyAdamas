// cimport.h —— C 互操作 v1：.cpya 声明文件的解析与查找
//
// .cpya 是手写的纯文本声明文件，与编译器生成的二进制模块接口 .pya 完全
// 无关：它只描述外部 C 符号、链接参数和编译期常量。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pyadamas {

// 一个外部参数/返回值在调用边界上的种类。指针统一按整数传递。
enum class CKind : uint8_t {
    None,
    Int,      // long (i64)
    Int32,    // int
    UInt32,   // unsigned
    Float,    // double
    Bool,     // _Bool
    Str,      // char*（非空）
    Ptr,      // void* / T*
    OptStr,   // 可空 char*
    // 以下只由 lower 在调用点根据实参类型选择，.cpya 不能声明：
    // 把 list/bytes 的连续 data 指针传给 C。
    BufF64,   // list[float] -> double*
    BufI64,   // list[int] -> long*
    BufBytes, // bytes -> uint8_t*
};

struct CParam {
    std::string name;
    CKind kind = CKind::Int;
    // 仅 kind == Ptr 有意义：ptr[T] 的 T；未写 T 时为 None。
    CKind ptr_elem = CKind::None;
};

struct CExtern {
    std::string symbol;   // C 侧符号名
    std::string py_name;  // py23 侧名字
    std::vector<CParam> params;
    CKind ret = CKind::None;
};

struct CConst {
    std::string name;
    CKind kind = CKind::Int;
    int64_t i = 0;
    double f = 0.0;
    bool b = false;
    std::string s;
};

struct CImportDecl {
    std::string name;
    std::vector<std::string> links;  // 已归一化：-lm 或原始参数
    std::vector<CConst> consts;
    std::vector<CExtern> externs;
};

// 按 cimport 的名字查找并解析 a/b.cpya。name 形如 a.b 时查找 a/b.cpya。
// 先相对 source_dir，再依次尝试 include_dirs 与当前目录。
std::optional<CImportDecl> load_cpya(const std::string& name, const std::string& source_dir,
                                    const std::vector<std::string>& include_dirs,
                                    std::string& err);

// 类型名 → CKind。无法识别时返回 nullopt，err 里带原因。
std::optional<CKind> parse_ckind(const std::string& text);

}  // namespace pyadamas
