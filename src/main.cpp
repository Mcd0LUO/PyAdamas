// main.cpp —— 命令行入口
//
// 用法:
//   py23 tokens <file.py>   打印 token 流
//   py23 ast    <file.py>   打印语法树
//   py23 types  <file.py>   打印每个顶层名字的推断类型
//   py23 check  <file.py>   词法 + 语法 + 类型检查
//   py23 check --syntax <f> 只做词法/语法检查
//   py23 ir     <file.py>   打印降级后的 IR
//   py23 asm    <file.py>   打印生成的 x86-64 汇编
//   py23 build  <file.py> [-o out]   编译并链接成可执行文件
//   py23 run    <file.py>   编译、链接并运行
//   py23        <file.py>   等价于 ast
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "ast.h"
#include "codegen.h"
#include "desugar.h"
#include "ir.h"
#include "lexer.h"
#include "lower.h"
#include "module.h"
#include "parser.h"
#include "typecheck.h"

#ifndef PY23_RT_OBJ
#define PY23_RT_OBJ "build/dev/rt.o"
#endif
#ifndef PY23_MODCACHE_DIR
#define PY23_MODCACHE_DIR "build/dev/modcache"
#endif

#include "driver_build.h"
#include "driver_util.h"

using py23::read_file;
using py23::report;
using py23::shell_quote;
using py23::make_temp;
using py23::Program;
using py23::load_program;
using py23::prepare_program;
using py23::link_program;

namespace {

void dump_tokens(const std::vector<py23::Token>& toks) {
    for (const auto& t : toks) {
        std::string detail;
        switch (t.kind) {
            case py23::Tok::INT: detail = std::format("  value={}", t.ival); break;
            case py23::Tok::FLOAT: detail = std::format("  value={}", t.fval); break;
            case py23::Tok::NAME:
            case py23::Tok::STRING:
            case py23::Tok::FSTRING: detail = "  \"" + t.text + "\""; break;
            default: break;
        }
        std::printf("%4d:%-3d %-12s%s\n", t.line, t.col, py23::tok_name(t.kind), detail.c_str());
    }
}

// ── 编译流水线 ──────────────────────────────────────────────────

struct Frontend {
    std::unique_ptr<py23::Module> mod;
    py23::TypeChecker tc;

    explicit Frontend(std::string file) : tc(std::move(file)) {}
};

// 解析 + 类型检查。有错误时打印并返回 nullopt。
std::optional<Frontend> run_frontend(const std::string& path, const std::string& source,
                                     bool syntax_only) {
    py23::Lexer lexer(source);
    auto tokens = lexer.run();

    py23::Parser parser(std::move(tokens));
    auto mod = parser.parse();
    py23::desugar_lambdas(*mod);

    Frontend fe(path);
    fe.mod = std::move(mod);

    if (syntax_only) return fe;

    fe.tc.check(*fe.mod);
    if (!fe.tc.diags().empty()) {
        for (const auto& d : fe.tc.diags()) report(path, source, d.line, d.col, d.msg);
        std::fprintf(stderr, "%zu error%s\n", fe.tc.diags().size(),
                     fe.tc.diags().size() == 1 ? "" : "s");
        return std::nullopt;
    }
    return fe;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "py23 —— Python 语法外壳的编译器\n"
                     "\n"
                     "usage: %s [tokens|ast|types|check|ir|asm|build|run] [-O0|-O1|-O2] "
                     "[--fast-math] [--syntax] [-o out] <file.py>\n",
                     argv[0]);
        return 2;
    }

    std::string mode;
    std::string path;
    std::string output;
    std::vector<std::string> include_dirs;
    bool syntax_only = false;
    bool verbose = false;
    bool rebuild = false;
    int opt_level = 2;
    bool fast_math = false;

    static const std::vector<std::string> kModes = {"tokens", "ast",     "check", "types",
                                                    "ir",     "asm",     "build", "run"};
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--syntax") syntax_only = true;
        else if (a == "-v" || a == "--verbose") verbose = true;
        else if (a == "--rebuild") rebuild = true;
        else if (a == "--fast-math") fast_math = true;
        else if (a == "-O0") opt_level = 0;
        else if (a == "-O1") opt_level = 1;
        else if (a == "-O2") opt_level = 2;
        else if (a.starts_with("--opt=")) {
            std::string v = a.substr(6);
            if (v == "0") opt_level = 0;
            else if (v == "1") opt_level = 1;
            else if (v == "2") opt_level = 2;
            else {
                std::fprintf(stderr, "error: invalid optimization level '%s' (expected 0,1,2)\n",
                             v.c_str());
                return 2;
            }
        }
        else if (a == "-o" && i + 1 < argc) output = argv[++i];
        else if (a == "-I" && i + 1 < argc) include_dirs.push_back(argv[++i]);
        else if (a.starts_with("-I") && a.size() > 2) include_dirs.push_back(a.substr(2));
        else if (mode.empty() && std::ranges::find(kModes, a) != kModes.end()) mode = a;
        else path = a;
    }
    if (mode.empty()) mode = "ast";
    if (path.empty()) {
        std::fprintf(stderr, "error: no input file\n");
        return 2;
    }

    auto src = read_file(path);
    if (!src) {
        std::fprintf(stderr, "error: %s\n", src.error().c_str());
        return 1;
    }

    try {
        if (mode == "tokens") {
            py23::Lexer lexer(*src);
            dump_tokens(lexer.run());
            return 0;
        }

        if (mode == "ast" || (mode == "check" && syntax_only)) {
            auto fe = run_frontend(path, *src, mode == "check");
            if (!fe) return 1;
            if (mode == "ast") {
                fe->mod->dump(std::cout);
                return 0;
            }
            std::printf("ok: %s (%zu statements)\n", path.c_str(), fe->mod->body.size());
            return 0;
        }

        if (mode == "check" || mode == "types") {
            // check/types 定位是通用前端：不解析 import 图，缺失/相对/
            // 星号导入只记 unsupported，交给 build 阶段判死。
            auto fe = run_frontend(path, *src, false);
            if (!fe) return 1;
            if (mode == "types") {
                for (auto& [name, ty] : fe->tc.top_symbols())
                    std::printf("%-24s %s\n", name.c_str(), ty.c_str());
                return 0;
            }
            if (verbose && !fe->tc.unsupported.empty()) {
                auto& uns = fe->tc.unsupported;
                std::ranges::sort(uns);
                auto uniq = std::ranges::unique(uns);
                uns.erase(uniq.begin(), uniq.end());
                std::fprintf(stderr, "note: %zu construct(s) parsed but not yet compiled:",
                             uns.size());
                for (auto& u : uns) std::fprintf(stderr, " %s", u.c_str());
                std::fprintf(stderr, "\n");
            }
            std::printf("ok: %s (%zu statements, %zu functions, %zu expressions)\n", path.c_str(),
                        fe->mod->body.size(), fe->tc.n_func, fe->tc.n_expr);
            return 0;
        }

        Program prog;
        prog.rebuild = rebuild;
        prog.verbose = verbose;
        prog.opt = opt_level;
        prog.fast_math = fast_math;
        if (!load_program(path, include_dirs, prog)) return 1;

        if (mode == "ir" || mode == "asm") {
            if (!prepare_program(prog, false, true)) return 1;
            if (!prog.entry->ir) return 1;
            if (mode == "ir") {
                py23::dump_ir(*prog.entry->ir, std::cout);
            } else {
                std::cout << py23::generate_asm(*prog.entry->ir);
            }
            return 0;
        }

        // build / run：模块各自汇编成 .o，再链接。
        if (!prepare_program(prog, true, false)) return 1;

        std::string out = output;
        bool ephemeral = false;
        if (out.empty()) {
            if (mode == "run") {
                auto tmp = make_temp("");
                if (!tmp) {
                    std::fprintf(stderr, "error: %s\n", tmp.error().c_str());
                    return 1;
                }
                out = *tmp;
                ::unlink(out.c_str());
                ephemeral = true;
            } else {
                out = path;
                if (out.ends_with(".py")) out.resize(out.size() - 3);
            }
        }
        if (!link_program(prog, out)) return 1;

        if (mode == "run") {
            std::string run_path = out;
            if (run_path.find('/') == std::string::npos) run_path = "./" + run_path;
            std::string cmd = shell_quote(run_path);
            int rc = std::system(cmd.c_str());
            if (ephemeral) ::unlink(out.c_str());
            if (rc < 0) return 1;
            return WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
        }
        return 0;
    } catch (const py23::LexError& e) {
        report(path, *src, e.line, e.col, e.what());
    } catch (const py23::ParseError& e) {
        report(path, *src, e.line, e.col, e.what());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: internal error: %s\n", path.c_str(), e.what());
    }
    return 1;
}
