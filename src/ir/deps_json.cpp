// ir/deps_json.cpp —— 模块图的确定性 JSON 输出
#include "ir/deps_json.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "driver/modules.h"
#include "ir/module.h"
#include "common/target_abi.h"

namespace pyadamas {
namespace {

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

std::string rel_or_empty(const std::string& abs_path, const std::string& base) {
    std::error_code ec;
    const std::filesystem::path a = std::filesystem::weakly_canonical(abs_path, ec);
    std::filesystem::path rel = a.lexically_relative(base);
    if (rel.empty()) return {};
    return rel.generic_string();
}

}  // namespace

std::string module_graph_json(const Program& p, const std::string& root_abs,
                              const std::string& cwd) {
    std::string root_rel = rel_or_empty(root_abs, cwd);
    if (root_rel.empty()) root_rel = ".";
    std::vector<const ModuleUnit*> units;
    units.reserve(p.units.size());
    for (const auto& [key, up] : p.units) {
        (void)key;
        units.push_back(up.get());
    }
    std::sort(units.begin(), units.end(), [](const ModuleUnit* a, const ModuleUnit* b) {
        return a->name < b->name;
    });
    auto display_name = [](const ModuleUnit* u) {
        return u->is_entry ? std::string("__main__") : u->name;
    };
    auto module_path = [&root_abs](const ModuleUnit* u, std::string& source) {
        if (path_under_stdlib(u->path)) {
            source = "stdlib";
            return "stdlib/" + stdlib_relative_path(u->path);
        }
        std::string rel = rel_or_empty(u->path, root_abs);
        if (rel.empty()) rel = std::filesystem::path(u->path).filename().string();
        source = rel.starts_with("..") ? "external" : "project";
        return rel;
    };

    std::string out;
    out += "{\n";
    out += "  \"format\": 1,\n";
    out += "  \"kind\": \"pyadamas-deps\",\n";
    out += std::format("  \"compiler\": {{\"version\": \"{}\", \"module_format\": {}, "
                       "\"module_abi\": {}, \"module_version\": \"{}\", \"target\": \"{}\"}},\n",
                       json_escape(kCompilerVersion), kModuleFormatVersion, kModuleAbiVersion,
                       json_escape(module_version()), json_escape(target_abi().name));
    out += std::format("  \"root\": \"{}\",\n", json_escape(root_rel));
    out += std::format("  \"entry\": \"{}\",\n", json_escape(display_name(p.entry)));
    out += "  \"order\": [";
    for (std::size_t i = 0; i < p.order.size(); ++i) {
        if (i != 0) out += ", ";
        out += "\"" + json_escape(display_name(p.order[i])) + "\"";
    }
    out += "],\n";
    out += "  \"modules\": [\n";
    for (std::size_t i = 0; i < units.size(); ++i) {
        const ModuleUnit* u = units[i];
        std::string source;
        const std::string rel = module_path(u, source);
        std::vector<std::string> deps = u->deps;
        std::sort(deps.begin(), deps.end());
        deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
        std::string iface;
        if (!u->is_entry && u->info && !u->info->interface_hash.empty())
            iface = "\"" + json_escape(u->info->interface_hash) + "\"";
        else
            iface = "null";
        out += "    {";
        out += "\"name\": \"" + json_escape(display_name(u)) + "\", ";
        out += "\"path\": \"" + json_escape(rel) + "\", ";
        out += "\"source\": \"" + json_escape(source) + "\", ";
        out += "\"version\": \"" + json_escape(module_version()) + "\", ";
        out += "\"mangled\": \"" + json_escape(module_symbol_prefix(u->is_entry ? u->name : u->name)) +
               "\", ";
        out += "\"interface_hash\": " + iface + ", ";
        out += "\"entry\": " + std::string(u->is_entry ? "true" : "false") + ", ";
        out += "\"deps\": [";
        for (std::size_t d = 0; d < deps.size(); ++d) {
            if (d != 0) out += ", ";
            out += "\"" + json_escape(deps[d]) + "\"";
        }
        out += "]}";
        if (i + 1 != units.size()) out += ",";
        out += "\n";
    }
    out += "  ]\n}\n";
    return out;
}

}  // namespace pyadamas
