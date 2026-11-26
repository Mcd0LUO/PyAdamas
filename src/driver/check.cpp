// driver/check.cpp —— check 模式的 import 图解析
//
// 从 driver/build.cpp 拆出（纯代码移动，函数体不变）：只建接口不生成代码，
// 与 build 共用 driver/modules.cpp 的模块表/解析。
#include "driver/build.h"
#include "driver/modules.h"
#include "driver/util.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "common/ast.h"
#include "syntax/desugar.h"
#include "syntax/lexer.h"
#include "ir/module.h"
#include "syntax/parser.h"
#include "typecheck/typecheck.h"

namespace pyadamas {

// ── check 模式的 import 图解析 ─────────────────────────────────────
// 和 build 共用 resolve_module_path / scan_imports_body / builtin_module，
// 但只跑到「建出接口」为止，不降级、不生成代码。依赖解析失败时返回
// nullptr（调用方按 unknown 处理），并把失败的模块名记进 missing。
namespace {

struct CheckCtx {
    std::vector<std::string> include_dirs;
    std::map<std::string, pyadamas::ModuleInfoPtr> loaded;
    std::set<std::string> loading;
    std::vector<std::string> missing;
    // 依赖模块的 TypeChecker 拥有 ClassInfo；必须活到 check 结束，
    // 否则接口里的 ClassInfo* 会悬空。
    std::vector<std::shared_ptr<pyadamas::TypeChecker>> keep_alive;
    // 原生模块与系统标准库模块是否按严格接口解析：默认 false = 保持 unknown
    // （0.486 的「前端宽、后端严」），--imports 时为 true（成员缺失会报错）。
    // 本项目自己的模块**始终**严格解析（接口完整，unknown 只会掩盖错误）。
    bool strict_all = false;
};

// 把某个模块的类型检查结果压成扁平 ModuleInfo（类方法/字段按 MRO 拍平），
// 和 build_unit 的接口导出逻辑保持一致，但不做编解码归一。
pyadamas::ModuleInfoPtr check_build_info(pyadamas::TypeChecker& tc,
                                          const std::string& runtime_name) {
    auto info = std::make_shared<pyadamas::ModuleInfo>();
    auto* sc = tc.module_scope();
    if (!sc) return info;
    for (auto& [name, sym] : sc->vars) {
        if (sym.ty && sym.ty->kind == pyadamas::TyKind::Class && sym.ty->cls) {
            const pyadamas::ClassInfo* cls = sym.ty->cls;
            if (cls->imported()) {
                pyadamas::ImportedClass ic;
                ic.cls = std::shared_ptr<pyadamas::ClassInfo>(const_cast<pyadamas::ClassInfo*>(cls), [](pyadamas::ClassInfo*){});
                ic.module = cls->origin_module;
                ic.qualname = cls->origin_qualname;
                info->classes[name] = std::move(ic);
                continue;
            }
            // 扁平类：字段按根→派生拍平，方法按最终槽位。
            std::vector<const pyadamas::ClassInfo*> chain;
            for (auto* cp = cls; cp; cp = cp->base) chain.push_back(cp);
            auto flat = std::make_shared<pyadamas::ClassInfo>();
            flat->name = name;
            flat->complete = true;
            flat->has_init = cls->has_init;
            flat->open = cls->is_open();
            flat->is_exception_class = cls->is_exception_class;
            flat->is_rich_exception = cls->is_rich_exception;
            flat->exc_parent = cls->exc_parent;
            // A8-4：enum 标记与成员表（与 build.cpp 的扁平导出口径一致）。
            flat->is_enum = cls->is_enum;
            flat->enum_members = cls->enum_members;
            // 动态属性槽（setattr/vars 开出来的运行期 dict）：接口必须一起带过去，
            // 否则 check 会比 build 更严 —— argparse.Namespace 这类「成员在运行期
            // 才存在」的类会误报 "'Namespace' object has no attribute 'input'"
            // （0.484 pya-lsp 缺口 2 的误杀，3 个 tests/run 用例回归）。
            flat->needs_dict = cls->needs_dict;
            for (auto it = chain.rbegin(); it != chain.rend(); ++it)
                for (auto& f : (*it)->fields) flat->fields.push_back(f);
            if (const auto* ma = cls->match_args_of()) flat->match_args = *ma;
            else flat->match_args = cls->match_args;
            std::map<std::string, pyadamas::MethodInfo> by_key;
            std::map<std::string, std::vector<int>> key_slots;
            std::vector<const pyadamas::ClassInfo*> mro(cls->mro.begin(), cls->mro.end());
            if (mro.empty()) mro.push_back(cls);
            // A8-3：类属性（与 build.cpp 的扁平导出同一口径，check 侧也不能少，
            // 否则 check 比 build 严：mod.C.X 在 check --imports 下误报）。
            for (const pyadamas::ClassInfo* k : mro) {
                for (auto& ca : k->class_attrs) {
                    if (flat->class_attr(ca.name)) continue;  // 最近声明者胜出
                    bool is_method = false;
                    for (const pyadamas::ClassInfo* mk : mro)
                        if (mk && mk->own_method(ca.name)) { is_method = true; break; }
                    if (is_method) continue;
                    pyadamas::ClassAttrInfo a;
                    a.name = ca.name;
                    a.ty = ca.ty;
                    a.owner = flat.get();
                    a.writable = false;
                    if (!ca.external_symbol.empty()) {
                        a.external_symbol = ca.external_symbol;
                    } else {
                        std::string decl_mod = k->imported() ? k->origin_module : runtime_name;
                        std::string decl_cls = k->imported() ? k->external_name() : k->name;
                        a.external_symbol = pyadamas::module_global_symbol(
                            decl_mod, std::format("__pa_cattr_{}_{}", decl_cls, ca.name));
                    }
                    flat->class_attrs.push_back(std::move(a));
                }
            }
            for (const pyadamas::ClassInfo* k : mro) {
                if (!k) continue;
                for (auto& m : k->methods) {
                    if (!m.call_sig) continue;
                    std::string key = pyadamas::method_slot_key(m.name, m.kind);
                    auto& v = key_slots[key];
                    if (std::find(v.begin(), v.end(), m.slot) == v.end()) v.push_back(m.slot);
                    for (int a : m.aliases)
                        if (std::find(v.begin(), v.end(), a) == v.end()) v.push_back(a);
                    if (!by_key.count(key)) by_key[key] = m;
                }
            }
            for (auto& [key, m] : by_key) {
                pyadamas::MethodInfo nm;
                nm.name = m.name;
                nm.kind = m.kind;
                nm.slot = m.slot;
                for (int slot : key_slots[key])
                    if (slot != m.slot) nm.aliases.push_back(slot);
                nm.call_sig = m.call_sig;
                nm.sig = m.call_sig;
                // 0.536：装饰标记（check 侧接口也要带，否则 check 会放行、build 才报）。
                nm.decorated = m.decorated;
                flat->methods.push_back(std::move(nm));
            }
            for (size_t mi = 0; mi < flat->methods.size(); ++mi) {
                pyadamas::MethodInfo& m = flat->methods[mi];
                if (!m.is_accessor()) continue;
                auto& pi = flat->properties[m.name];
                pi.owner = flat.get();
                switch (m.kind) {
                    case pyadamas::MethodKind::PropertyGetter: pi.getter = static_cast<int>(mi); break;
                    case pyadamas::MethodKind::PropertySetter: pi.setter = static_cast<int>(mi); break;
                    case pyadamas::MethodKind::PropertyDeleter: pi.deleter = static_cast<int>(mi); break;
                    default: break;
                }
            }
            flat->origin_module = runtime_name;
            flat->origin_qualname = runtime_name + "." + name;
            // 祖先链：本地类按 C3 MRO 拍平，导入类沿用它的 origin。
            for (const pyadamas::ClassInfo* k : cls->mro) {
                if (!k || k == cls) continue;
                pyadamas::ClassAncestor a;
                if (k->imported()) { a.module = k->origin_module; a.name = k->external_name(); a.qualname = k->origin_qualname; }
                else { a.module = runtime_name; a.name = k->name; a.qualname = runtime_name + "." + k->name; }
                bool dup = false;
                for (auto& e : flat->ext_ancestors)
                    if (e.module == a.module && e.name == a.name) { dup = true; break; }
                if (!dup) flat->ext_ancestors.push_back(std::move(a));
            }
            pyadamas::ImportedClass ic;
            if (auto* im = flat->method("__init__"); im && im->call_sig) ic.ctor_sig = im->call_sig;
            if (!ic.ctor_sig) ic.ctor_sig = std::make_shared<pyadamas::FuncInfo>();
            ic.cls = flat;
            ic.module = runtime_name;
            ic.qualname = flat->origin_qualname;
            info->classes[name] = std::move(ic);
            continue;
        }
        info->globals[name] = sym.ty;
    }
    info->globals["__name__"] = pyadamas::t_str();
    info->globals["__file__"] = pyadamas::t_str();
    return info;
}

pyadamas::ModuleInfoPtr check_load(CheckCtx& ctx, const std::string& importer,
                                    const std::string& name);

// 按文件解析一个模块、建出接口；ctx.loading 负责环检测。失败时记
// missing 并返回 nullptr（调用方按 unknown 处理）。
pyadamas::ModuleInfoPtr check_load_file(CheckCtx& ctx, const std::string& name,
                                        const std::string& path) {
    auto src = read_file(path);
    if (!src) {
        ctx.loading.erase(name);
        ctx.missing.push_back(name);
        return nullptr;
    }
    try {
        pyadamas::Lexer lexer(*src);
        pyadamas::Parser parser(lexer.run());
        auto mod = parser.parse();
        pyadamas::desugar_lambdas(*mod);
        auto tc = std::make_shared<pyadamas::TypeChecker>(path);
        std::string cap_name = name;
        tc->set_module_lookup([&ctx, path, cap_name](const std::string& dep) {
            return check_load(ctx, path, dep);
        });
        tc->set_lenient_imports(true);
        // 0.555：与 build 同口径 —— 源码出现 dataclass 时按需打开（=0 强制关）。
        const char* lg_env = std::getenv("PYADAMAS_LANG_GAPS");
        const bool lg_forced_off = lg_env && std::string_view(lg_env) == "0";
        const bool lg_env_on = lg_env && std::string_view(lg_env) != "0";
        // 只对项目文件自动开（标准库/CPython Lib 保持原口径，见 build.cpp 同一段注释）。
        const bool project_file = path.find("/python3.") == std::string::npos &&
                                  !path_under_stdlib(path);
        tc->set_lang_gaps(lg_env_on ||
                          (!lg_forced_off && project_file &&
                           src->find("dataclass") != std::string::npos));
        tc->check(*mod);
        ctx.keep_alive.push_back(tc);
        auto info = check_build_info(*tc, name);
        info->name = name;
        info->source_path = path;
        // 与 build 端同口径：接口做一次 encode→decode 归一。检查模式此前直接
        // 用 TypeChecker 里的指针，annotation 解析（classes 表）与函数签名里的
        // ClassInfo* 是不同对象；assignable 用 is_subclass_of 的指针比较，于是
        // 跨模块 `p: a.Point = a.make(...)` 在 check --imports 下误报
        // 「annotated as Point but assigned Point」。解码后类表/签名统一到
        // 同一批 ClassInfo，与 build 行为一致。
        if (auto bytes = pyadamas::encode_module_interface(*info)) {
            // 0.541：跨模块类引用（tag 29）要按模块名取别的模块的接口 —— 与 build 端
            // 同口径（build 用 Program::units + builtin_module）。这里用 check 自己的
            // 加载器，避免 check --imports 把跨模块类引用退化成 unknown。
            auto iface_loader = [&ctx, path](const std::string& dep) -> pyadamas::ModuleInfoPtr {
                return check_load(ctx, path, dep);
            };
            if (auto dec = pyadamas::decode_module_interface(*bytes, path, iface_loader))
                info = *dec;
        }
        ctx.loading.erase(name);
        return info;
    } catch (...) {
        ctx.loading.erase(name);
        ctx.missing.push_back(name);
        return nullptr;
    }
}

pyadamas::ModuleInfoPtr check_load(CheckCtx& ctx, const std::string& importer,
                                    const std::string& name) {
    if (auto it = ctx.loaded.find(name); it != ctx.loaded.end()) return it->second;
    // 0.552：**请求方是不是项目代码**。编译器自带的 stdlib（stdlib/*.py，我们自己写的、
    // 接口完整）在「项目代码导入它」时按完整接口解析；**标准库扫描**（626 文件那套，
    // 请求方也在标准库里）仍保持 unknown —— 那条红线是 0.523 实测 499 → 223 换来的。
    const bool importer_is_project = [] (const std::string& p) {
        if (p.empty() || p.find("/python3.") != std::string::npos || path_under_stdlib(p)) return false;
        return true;
    }(importer);
    // CPython 桥子代理：与 build 端 program_module 同口径。
    if (name.starts_with("python.")) {
        auto m = pyadamas::builtin_module(name);
        ctx.loaded[name] = m;
        return m;
    }
    const ModuleEntry* entry = find_module_entry(name);
    if (is_native_module(entry)) {
        // 默认（非 --imports）：原生模块不参与解析，保持 unknown —— 0.486 的教训是
        // 我们的原生签名表不全，严格解析会在真实代码上误杀。
        if (!ctx.strict_all) return nullptr;
        auto m = pyadamas::builtin_module(name);
        ctx.loaded[name] = m;
        return m;
    }
    // 系统标准库（编译器自带 stdlib 或 CPython 的 Lib 目录）里的模块：默认不解析。
    // 我们的接口对我们的 stdlib 是完整的，但对 CPython 3.14 的整个 Lib 不完整；
    // 强制解析会在真实代码上大面积误杀（0.523 实测：626 文件默认口径 499 → 223）。
    if (!ctx.strict_all && entry) {
        std::string src = std::string(entry->source);
        const std::string stdlib_dir = pyadamas::stdlib_dir();
        const bool in_compiler_stdlib =
            src == stdlib_dir ||
            (src.size() > stdlib_dir.size() && src.compare(0, stdlib_dir.size(), stdlib_dir) == 0 &&
             src[stdlib_dir.size()] == '/');
        const bool in_python_lib = src.find("/python3.") != std::string::npos;
        // CPython 的 Lib 永远不解析（接口不完整，会大面积误杀）。
        if (in_python_lib) return nullptr;
        // 编译器自己的 stdlib：项目代码导入时按完整接口解析（contextlib/io 这类
        // 纯 pya 模块的类才拿得到），标准库扫描时仍按 unknown。
        if (in_compiler_stdlib && !importer_is_project) return nullptr;
    }
    if (entry && entry->kind == ModuleKind::NativePython) {
        // 我们自己的 stdlib 扩展模块（functools 等）接口还不完整（`partial`、
        // `itertools.count` 这类未建模）：非 --imports 时保持 unknown，避免误杀。
        if (!ctx.strict_all) return nullptr;
        // 原生 + stdlib 扩展源：接口 = 原生接口 + 扩展新增名字。
        std::string path = module_source_path(*entry);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ctx.loading.count(name)) {
            ctx.missing.push_back(name);
            ctx.loaded[name] = nullptr;
            return nullptr;
        }
        ctx.loading.insert(name);
        auto ext = check_load_file(ctx, name, path);
        if (!ext) {
            ctx.loaded[name] = nullptr;
            return nullptr;
        }
        auto info = merge_extension_module(pyadamas::builtin_module(name), ext);
        info->name = name;
        info->source_path = path;
        ctx.loaded[name] = info;
        return info;
    }
    if (ctx.loading.count(name)) return nullptr;  // 环：按 unknown
    auto found = resolve_import_source(importer, name, ctx.include_dirs);
    if (!found) {
        ctx.missing.push_back(name);
        ctx.loaded[name] = nullptr;
        return nullptr;
    }
    std::string path = canonical_path(found->path);
    // 非 --imports：**CPython 标准库**（/usr/lib/python3.x、dist-packages）里的模块按
    // unknown 处理——我们的接口对自己的 stdlib 完整，对 CPython 整个 Lib 不完整，
    // 严格解析会大面积误杀（实测 626 文件默认口径 499 → 223）。本项目自己的模块
    // 始终严格解析（跨模块调用不再退化成 unknown）。
    if (!ctx.strict_all && (path.find("/python3.") != std::string::npos ||
                            (path_under_stdlib(path) && !importer_is_project))) {
        ctx.loaded[name] = nullptr;
        return nullptr;
    }
    // 单一数据源：stdlib 目录里的 Python 源必须在模块表里登记。
    if (!entry && path_under_stdlib(path)) {
        ctx.missing.push_back(name);
        ctx.loaded[name] = nullptr;
        return nullptr;
    }
    ctx.loading.insert(name);
    auto info = check_load_file(ctx, name, path);
    ctx.loaded[name] = info;
    return info;
}

}  // namespace

CheckLookup load_check_lookup(const std::string& entry_path,
                               const std::vector<std::string>& include_dirs,
                               std::vector<std::string>& missing, bool strict_all) {
    auto ctx = std::make_shared<CheckCtx>();
    ctx->strict_all = strict_all;
    ctx->include_dirs = include_dirs;
    ctx->include_dirs.emplace_back(pyadamas::stdlib_dir());
    std::string entry = canonical_path(entry_path);
    auto lookup = [ctx, entry](const std::string& name) -> ModuleInfoPtr {
        return check_load(*ctx, entry, name);
    };
    // 预加载一次，收集 missing。
    auto src = read_file(entry);
    if (src) {
        pyadamas::Lexer lexer(*src);
        pyadamas::Parser parser(lexer.run());
        auto mod = parser.parse();
        if (mod) {
            std::vector<ImportReq> reqs;
            std::vector<ImportIssue> issues;
            scan_imports_body(mod->body, reqs, issues, std::string());
            for (auto& r : reqs) lookup(r.name);
        }
    }
    missing = ctx->missing;
    return lookup;
}

}  // namespace pyadamas
