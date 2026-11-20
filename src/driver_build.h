// driver_build.h —— 多模块构建驱动
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <functional>
#include <string>
#include <vector>

#include "ir.h"
#include "module.h"
#include "typecheck.h"

namespace pyadamas {

struct ModuleUnit {
    std::string key;           // 内部映射键；入口是 "@entry"
    std::string name;          // 符号前缀；入口是 "__entry__"
    std::string runtime_name;  // __name__
    std::string path;
    std::string source;
    std::unique_ptr<pyadamas::Module> ast;
    pyadamas::ModuleInfoPtr info;
    std::shared_ptr<pyadamas::TypeChecker> tc;
    std::optional<pyadamas::IRModule> ir;
    std::vector<std::string> deps;
    std::string cache_dir;
    std::string asm_path;
    std::string obj_path;
    std::string iface_path;
    std::string manifest_path;
    std::string asm_text;
    bool is_entry = false;
    bool is_package = false;
    std::string parent_name;
    bool from_cache = false;
    // 模块的 IR 是否用到 unknown 保守 retain（含 asyncio/生成器运行时）。
    // 写进缓存清单：命中缓存时也要参与整程序判断。
    bool needs_unknown_index = true;
    // 模块是否用到 from c import（dlsym RTLD_DEFAULT）。写进缓存清单：
    // 命中缓存时也要参与整程序判断（--gc-sections 安全性，见 link_program）。
    bool uses_c_sym_default = false;
    int state = 0;  // 0 未访问 / 1 正在访问 / 2 完成
};

struct Program {
    std::map<std::string, std::unique_ptr<ModuleUnit>> units;
    std::vector<ModuleUnit*> order;
    ModuleUnit* entry = nullptr;
    std::vector<std::string> include_dirs;
    // cimport .cpya 里 link 声明的链接参数（已归一化并去重）。
    std::vector<std::string> link_args;
    bool rebuild = false;
    bool verbose = false;
    int opt = 2;
    bool fast_math = false;
    // 零开销异常（--zero-cost-exc）：编译选项，写进模块缓存清单参与失效。
    // Z5：表驱动展开默认开启；--no-zero-cost-exc 可回退。
    bool zero_cost_exc = true;
    // live range splitting（PYADAMAS_RA_SPLIT）：会改变代码生成结果，
    // 必须写进模块缓存清单参与失效 —— 否则切换开关后仍命中旧产物。
    bool rc_split = false;
    // 图着色分配器（PYADAMAS_RA_GRAPH）：同样会改变代码生成结果，必须
    // 参与模块缓存失效，否则 A/B 测试会命中旧产物拿到假数据。
    bool ra_graph = false;
    // callee-saved 提升增强（PYADAMAS_RA_OPT，默认关）：改变代码生成结果，
    // 必须写进模块缓存清单参与失效。
    bool ra_opt = false;
    // 浮点循环 SIMD 向量化（PYADAMAS_SIMD，默认关）：改变代码生成结果，
    // 必须写进模块缓存清单参与失效，否则切开关命中旧产物拿假数据。
    bool simd = false;
    // 通用循环自动向量化（PYADAMAS_VECT，默认关）：改变代码生成结果，
    // 必须写进模块缓存清单参与失效。
    bool vect = false;
    // 整数循环优化（PYADAMAS_LOOP_OPT，默认 0 全关）：改变 IR 优化结果，
    // 同样必须写进模块缓存清单参与失效。位掩码见 opt.h。
    int loop_opt = 0;
    // 内联/devirt 深化（PYADAMAS_INLINE_OPT，默认 0 关）：改变 IR 优化与
    // 代码生成结果，必须写进模块缓存清单参与失效。
    bool inline_opt = false;
    // 只读借用槽细化（PYADAMAS_BORROW_OPT，默认 0 关）：改变 IR 与代码生成
    // 结果，必须写进模块缓存清单参与失效。
    bool borrow_opt = false;
    // 运行期类型标签（PYADAMAS_TYPE_TAGS，默认关）：在 P3 shadow tag 之上
    // 追加 NONE/BOOL/精确堆子类 tag，解锁 isinstance/repr/pprint。改变 IR
    // 与代码生成结果（也改变 tag 判定），必须参与模块缓存失效。
    bool type_tags = false;
    // 任一模块需要 unknown 索引就保持开启；全都不需要时入口关掉维护。
    bool needs_unknown_index = false;
    // 任一模块用到 from c import（dlsym RTLD_DEFAULT）就置位：链接期
    // --gc-sections 无法静态覆盖运行期符号查找，整程序保守关 gc。
    bool uses_c_sym_default = false;
};

// 内建模块（math/os/json/...）的签名表，实现在 builtin_sigs.cpp。
ModuleInfoPtr builtin_module(const std::string& name);

// check 模式用的 import 图解析：加载依赖模块的接口（不生成代码），
// 返回一个 module_lookup 闭包给 TypeChecker。无法解析的依赖返回 nullptr
// （调用方按 unknown 处理）。diagnostics 里收集真正解析失败的模块。
using CheckLookup = std::function<ModuleInfoPtr(const std::string&)>;
CheckLookup load_check_lookup(const std::string& entry_path,
                               const std::vector<std::string>& include_dirs,
                               std::vector<std::string>& missing);
bool load_program(const std::string& entry_path, const std::vector<std::string>& include_dirs,
                  Program& p);
bool prepare_program(Program& p, bool emit_objects, bool entry_ir);
bool link_program(Program& p, const std::string& output);
// --asan 时改用插桩版运行时（rt_asan.o），让 RC/GC 内部的越界/UAF 也能被
// AddressSanitizer 检查到。默认的 rt.o 不带插桩，只覆盖生成代码那一侧。
void set_asan_runtime(bool on);

}  // namespace pyadamas
