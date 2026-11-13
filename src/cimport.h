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
    // 带字段的 .cpya struct 按值传递；结构体名记在 ptr_struct/ret_struct。
    Struct,
    // C 回调参数（函数指针）：调用点传入 PyAdamas 函数，由生成的跳板适配。
    Callback,
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
    // 引用的 .cpya struct 名：kind == Ptr 时是 ptr[T] 的 T（不接受缓冲区
    // 实参），kind == Struct 时是按值传递的结构体。
    std::string ptr_struct;
    // struct 声明里没有字段（如 FILE）：不透明指针，字段访问报错。
    bool opaque = false;
    // kind == Callback：C 侧函数指针签名（只允许标量/指针）。
    std::vector<CKind> cb_params;
    CKind cb_ret = CKind::None;
};

struct CExtern {
    std::string symbol;   // C 侧符号名
    std::string py_name;  // PyAdamas 侧名字
    std::vector<CParam> params;
    CKind ret = CKind::None;
    // 参数表末尾有 `...`：允许传任意多个额外实参。
    bool varargs = false;
    std::string ret_struct;  // 返回 ptr[Struct] 或按值 Struct 时的结构体名
    // 返回 ptr[T] 的 T（标量）；用于 `p[i]` 解引用。
    CKind ret_elem = CKind::None;
};

// .cpya 里带字段的 struct：字段按 C 规则布局，ptr[Struct] 可直接读写字段。
struct CStructField {
    std::string name;
    CKind kind = CKind::Int;
    int64_t offset = 0;
    int64_t size = 0;
    int64_t align = 1;
};

struct CStruct {
    std::string name;
    std::vector<CStructField> fields;
    int64_t size = 0;
    int64_t align = 1;
};

// `.cpya` 里的 C 全局变量（extern var）。只支持标量/指针，读取时按声明
// 宽度从符号地址加载；不支持按值结构体变量。
struct CVar {
    std::string symbol;   // C 侧符号名
    std::string py_name;  // PyAdamas 侧名字
    CKind kind = CKind::Int;
    // kind == Ptr 且指向 .cpya 里的 struct 时记录名字。
    std::string struct_name;
    // ptr[T] 的标量元素种类（用于解引用）。
    CKind elem = CKind::None;
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
    std::vector<CStruct> structs;   // 带字段的结构体布局
    std::vector<CVar> vars;         // extern 变量
    std::vector<CExtern> externs;
};

// 按 cimport 的名字查找并解析 a/b.cpya。name 形如 a.b 时查找 a/b.cpya。
// 先相对 source_dir，再依次尝试 include_dirs 与当前目录。
std::optional<CImportDecl> load_cpya(const std::string& name, const std::string& source_dir,
                                    const std::vector<std::string>& include_dirs,
                                    std::string& err);

// 类型名 → CKind。无法识别时返回 nullopt，err 里带原因。
std::optional<CKind> parse_ckind(const std::string& text);

// CKind 在调用边界/解引用时的机器宽度（字节）。
inline int ckind_width(CKind k) {
    switch (k) {
        case CKind::Bool: return 1;
        case CKind::Int32:
        case CKind::UInt32: return 4;
        default: return 8;
    }
}

}  // namespace pyadamas
