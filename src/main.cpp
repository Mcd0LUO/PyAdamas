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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
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
#include "parser.h"
#include "typecheck.h"

#ifndef PY23_RT_OBJ
#define PY23_RT_OBJ "build/dev/rt.o"
#endif

namespace {

// 用 std::expected 表达"可能失败"的文件读取，避免异常穿透
std::expected<std::string, std::string> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected(std::format("cannot open '{}'", path));
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) return std::unexpected(std::format("failed while reading '{}'", path));
    return ss.str();
}

// 打印出错行，并在指定列插入 ^。
// 超长行（生成代码常见几千字符一行）只显示插入符附近的片段，
// 否则一条错误会把终端刷爆。
void print_snippet(const std::string& source, int line, int col) {
    if (line <= 0) return;
    size_t cur = 1;
    size_t begin = 0;
    for (size_t i = 0; i < source.size(); ++i) {
        if (cur == static_cast<size_t>(line)) break;
        if (source[i] == '\n') {
            ++cur;
            begin = i + 1;
        }
    }
    if (cur != static_cast<size_t>(line)) return;
    size_t end = source.find('\n', begin);
    if (end == std::string::npos) end = source.size();
    if (end <= begin) return;

    std::string_view text(source.data() + begin, end - begin);
    constexpr size_t kMaxCols = 120;
    size_t caret = (col > 0) ? static_cast<size_t>(col - 1) : 0;
    size_t start = 0;
    if (text.size() > kMaxCols) {
        if (caret > kMaxCols / 3) start = caret - kMaxCols / 3;
        if (start + kMaxCols > text.size()) start = text.size() - kMaxCols;
        if (caret < start) caret = start;
    }
    size_t len = std::min(kMaxCols, text.size() - start);

    // 展开 tab，否则插入符对不齐
    std::string shown;
    size_t caret_pad = 0;
    for (size_t i = start; i < start + len; ++i) {
        if (i == caret) caret_pad = shown.size();
        char c = text[i];
        if (c == '\t') {
            size_t n = 4 - shown.size() % 4;
            shown.append(n, ' ');
        } else {
            shown.push_back(c);
        }
    }
    if (caret >= start + len) caret_pad = shown.size();

    const char* ellipsis = start ? "..." : "";
    std::fprintf(stderr, "  %5d | %s%.*s\n", line, ellipsis, static_cast<int>(shown.size()),
                 shown.data());
    std::string pad(std::strlen(ellipsis) + caret_pad, ' ');
    std::fprintf(stderr, "        | %s^\n", pad.c_str());
}

void report(const std::string& path, const std::string& source, int line, int col,
            std::string_view msg) {
    std::fprintf(stderr, "%s:%d:%d: error: %.*s\n", path.c_str(), line, col,
                 static_cast<int>(msg.size()), msg.data());
    print_snippet(source, line, col);
}

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

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// 建一个唯一临时文件，返回路径。调用方负责 unlink。
std::expected<std::string, std::string> make_temp(const char* suffix) {
    std::string tmpl = std::format("/tmp/py23-XXXXXX{}", suffix);
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = ::mkstemps(buf.data(), static_cast<int>(std::strlen(suffix)));
    if (fd < 0) return std::unexpected("cannot create temporary file");
    ::close(fd);
    return std::string(buf.data());
}

// 生成汇编 → 写临时文件 → 交给 cc 汇编链接。
std::expected<std::string, std::string> compile_to_binary(const std::string& asm_text,
                                                          const std::string& output) {
    auto tmp = make_temp(".s");
    if (!tmp) return std::unexpected(tmp.error());

    {
        std::ofstream out(*tmp, std::ios::binary);
        if (!out) return std::unexpected(std::format("cannot write '{}'", *tmp));
        out << asm_text;
    }

    std::string cmd = std::format("cc -o {} {} {} -lm", shell_quote(output), shell_quote(*tmp),
                                  shell_quote(PY23_RT_OBJ));
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::fprintf(stderr, "error: assembler/linker failed (runtime: %s)\n", PY23_RT_OBJ);
        std::fprintf(stderr, "       command: %s\n", cmd.c_str());
        return std::unexpected("cc failed");
    }
    if (std::getenv("PY23_KEEP_ASM")) std::fprintf(stderr, "note: assembly kept at %s\n", tmp->c_str());
    else ::unlink(tmp->c_str());
    return output;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "py23 —— Python 语法外壳的编译器\n"
                     "\n"
                     "usage: %s [tokens|ast|types|check|ir|asm|build|run] [--syntax] [-o out] <file.py>\n",
                     argv[0]);
        return 2;
    }

    std::string mode;
    std::string path;
    std::string output;
    bool syntax_only = false;
    bool verbose = false;

    static const std::vector<std::string> kModes = {"tokens", "ast",     "check", "types",
                                                    "ir",     "asm",     "build", "run"};
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--syntax") syntax_only = true;
        else if (a == "-v" || a == "--verbose") verbose = true;
        else if (a == "-o" && i + 1 < argc) output = argv[++i];
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

        auto fe = run_frontend(path, *src, mode == "check" && syntax_only);
        if (!fe) return 1;

        if (mode == "ast") {
            fe->mod->dump(std::cout);
            return 0;
        }
        if (mode == "check" && syntax_only) {
            std::printf("ok: %s (%zu statements)\n", path.c_str(), fe->mod->body.size());
            return 0;
        }
        if (mode == "types") {
            for (auto& [name, ty] : fe->tc.top_symbols())
                std::printf("%-24s %s\n", name.c_str(), ty.c_str());
            return 0;
        }
        if (mode == "check") {
            if (verbose && !fe->tc.unsupported.empty()) {
                std::ranges::sort(fe->tc.unsupported);
                auto uniq = std::ranges::unique(fe->tc.unsupported);
                fe->tc.unsupported.erase(uniq.begin(), uniq.end());
                std::fprintf(stderr, "note: %zu construct(s) parsed but not yet compiled:",
                             fe->tc.unsupported.size());
                for (auto& u : fe->tc.unsupported) std::fprintf(stderr, " %s", u.c_str());
                std::fprintf(stderr, "\n");
            }
            std::printf("ok: %s (%zu statements, %zu functions, %zu expressions)\n", path.c_str(),
                        fe->mod->body.size(), fe->tc.n_func, fe->tc.n_expr);
            return 0;
        }

        // ── IR 之后 ──────────────────────────────────────────
        auto lr = py23::lower_module(*fe->mod, fe->tc, path);
        if (!lr.errors.empty()) {
            for (const auto& e : lr.errors) report(path, *src, e.line, e.col, e.msg);
            std::fprintf(stderr, "%zu error%s in code generation\n", lr.errors.size(),
                         lr.errors.size() == 1 ? "" : "s");
            return 1;
        }

        if (mode == "ir") {
            py23::dump_ir(lr.mod, std::cout);
            return 0;
        }

        std::string asm_text = py23::generate_asm(lr.mod);
        if (mode == "asm") {
            std::cout << asm_text;
            return 0;
        }

        // build / run
        std::string out = output;
        bool ephemeral = false;
        if (out.empty()) {
            if (mode == "run") {
                // run 就是「编译完立刻跑」，不该在源码旁边留下一个可执行文件。
                // 临时名留给 cc 自己创建，先删掉空文件，否则 ld 会沿用它 0600 的权限。
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
        auto built = compile_to_binary(asm_text, out);
        if (!built) return 1;

        if (mode == "run") {
            // 相对路径要加 ./，否则 shell 只会在 PATH 里找
            std::string run_path = *built;
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
