// module.h —— 跨模块接口与 .pya
//
// 分模块编译时，导入方只依赖接口，不读对方的 AST。接口用二进制编码：
// 头 + 模块名/源路径 + 全局名与类型，尾部 FNV-1a 校验。类型里暂时只
// 支持 0.14a 需要的标量、容器、Func；Class 等 0.14b 再接。
#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "typecheck.h"

namespace pyadamas {

struct ImportedClass {
    ClassInfoPtr cls;
    FuncInfoPtr ctor_sig;
    std::string ctor_symbol;
    std::string module;  // 定义类的模块名（类对象限定名用）
    std::string qualname;  // 定义模块发出的运行期限定名
};

struct ModuleInfo {
    std::string name;
    std::string source_path;
    bool builtin = false;
    // 原生模块带 stdlib Python 扩展源（当前是 functools）：导入时必须跑
    // 扩展模块的 init，把扩展全局初始化出来。
    bool source_init = false;
    // 扩展源补进来的（原生没有的）全局名：降级时这些名字按普通外部全局
    // 处理，其余名字继续走原生降级路径。原生名字优先，扩展不覆盖。
    std::set<std::string> source_globals;
    // CPython 桥模块代理（from python import <module>）：没有编译期签名表，
    // 任意属性/调用都放行成 unknown，降级成 rt_cpy 的子进程 IPC 调用。
    // 名字是 "python"（根，只用于 from-import 解析）或 "python.<module>"。
    bool cpy_proxy = false;
    std::string interface_hash;  // 二进制接口的 FNV-1a 64（hex）
    // 模块顶层名字 → 类型。函数本身就是 Func 类型的全局。
    // 类名单独放在 classes 里，不重复出现在 globals。
    std::map<std::string, TypePtr> globals;
    std::map<std::string, ImportedClass> classes;
};

// 11：类记录追加 is_exception_class / is_rich_exception / exc_parent。
// 跨模块的 except/raise 名字位置要靠它判断「这个导入类是不是异常类」，
// 否则只能在导入方报 except requires an exception name。
// 12：类记录追加 needs_dict（动态属性槽是否开启）；签名形参追加
// ann_unknown（显式 object/Any 标注）。导入方据此放行 args.x 直读/
// 直写/getattr，并保持 object 形参的 unknown 域（float 实参装箱）。
// 13：接口不再编码绝对 source_path（只影响诊断的字段不该进 interface_hash，
//     否则换目录/换机器即哈希变化、可复现性不成立）。旧 .pya 在新编译器下
//     会因 fmt 不匹配判失效并重编。
inline constexpr uint16_t kModuleFormatVersion = 13;
inline constexpr uint16_t kModuleAbiVersion = 1;

std::expected<std::string, std::string> encode_module_interface(const ModuleInfo& info);
std::expected<ModuleInfoPtr, std::string> decode_module_interface(const std::string& bytes,
                                                                  const std::string& source_path);

uint64_t fnv1a64(const void* data, size_t n);
inline uint64_t fnv1a64(const std::string& s) { return fnv1a64(s.data(), s.size()); }
std::string hex64(uint64_t v);
std::string module_symbol_prefix(const std::string& module);
std::string module_global_symbol(const std::string& module, const std::string& name);
std::string module_ready_symbol(const std::string& module);
std::string module_init_symbol(const std::string& module);
std::string module_roots_symbol(const std::string& module);
std::string module_class_new_symbol(const std::string& module, const std::string& cls);
// 导出类的静态 vtable 与本类声明的槽表地址。跨模块继承时导入方用它们
// 填子类 vtable / super 查找。
std::string module_class_vtable_symbol(const std::string& module, const std::string& cls);
std::string module_class_own_slots_symbol(const std::string& module, const std::string& cls);
std::string module_class_method_symbol(const std::string& module, const std::string& cls,
                                       int slot);

}  // namespace pyadamas
