// driver_check.cpp —— check 模式的 import 图解析
//
// 从 driver_build.cpp 拆出（纯代码移动，函数体不变）：只建接口不生成代码，
// 与 build 共用 driver_modules.cpp 的模块表/解析。
#include "driver_build.h"
#include "driver_modules.h"
#include "driver_util.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "ast.h"
#include "desugar.h"
#include "lexer.h"
#include "module.h"
#include "parser.h"
#include "typecheck.h"

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
            for (auto it = chain.rbegin(); it != chain.rend(); ++it)
                for (auto& f : (*it)->fields) flat->fields.push_back(f);
            if (const auto* ma = cls->match_args_of()) flat->match_args = *ma;
            else flat->match_args = cls->match_args;
            std::map<std::string, pyadamas::MethodInfo> by_key;
            std::map<std::string, std::vector<int>> key_slots;
            std::vector<const pyadamas::ClassInfo*> mro(cls->mro.begin(), cls->mro.end());
            if (mro.empty()) mro.push_back(cls);
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
        tc->set_lang_gaps(std::getenv("PYADAMAS_LANG_GAPS") != nullptr &&
                          std::string_view(std::getenv("PYADAMAS_LANG_GAPS")) != "0");
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
            if (auto dec = pyadamas::decode_module_interface(*bytes, path))
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
    // CPython 桥子代理：与 build 端 program_module 同口径。
    if (name.starts_with("python.")) {
        auto m = pyadamas::builtin_module(name);
        ctx.loaded[name] = m;
        return m;
    }
    const ModuleEntry* entry = find_module_entry(name);
    if (is_native_module(entry)) {
        auto m = pyadamas::builtin_module(name);
        ctx.loaded[name] = m;
        return m;
    }
    if (entry && entry->kind == ModuleKind::NativePython) {
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
                               std::vector<std::string>& missing) {
    auto ctx = std::make_shared<CheckCtx>();
    ctx->include_dirs = include_dirs;
    ctx->include_dirs.emplace_back(PYADAMAS_STDLIB_DIR);
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
