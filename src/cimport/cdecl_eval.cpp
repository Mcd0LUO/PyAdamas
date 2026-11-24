// cimport/cdecl_eval.cpp —— C 常量表达式求值（struct 定长数组维度、enum 初值）
//
// 0.483 从 cdecl_parse.cpp 拆出（规模检查 1200 行硬限）。递归下降 + 优先级攀爬，
// 只认整数常量表达式与已收集的宏/枚举值；sizeof、类型转换等一律按「不是常量」
// 返回 nullopt，由调用方报错，不在这一层猜。
#include "cimport/cdecl_internal.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pyadamas {
namespace {

struct Eval {
    const std::vector<Tok>& t;
    size_t i = 0;
    size_t end = 0;
    const std::map<std::string, int64_t>& env;
    bool ok = true;

    int64_t number(const std::string& raw) {
        std::string s = raw;
        while (!s.empty() && (s.back() == 'u' || s.back() == 'U' || s.back() == 'l' ||
                              s.back() == 'L'))
            s.pop_back();
        if (s.empty()) {
            ok = false;
            return 0;
        }
        char* endp = nullptr;
        long long v = std::strtoll(s.c_str(), &endp, 0);
        if (!endp || *endp != '\0') {
            ok = false;
            return 0;
        }
        return static_cast<int64_t>(v);
    }

    int64_t primary() {
        if (i >= end) {
            ok = false;
            return 0;
        }
        const Tok& tok = t[i];
        if (tok.k == Tok::Num) {
            ++i;
            return number(tok.s);
        }
        if (tok.k == Tok::Id) {
            ++i;
            auto it = env.find(tok.s);
            if (it == env.end()) {
                ok = false;
                return 0;
            }
            return it->second;
        }
        if (tok.k == Tok::Punc && tok.s == "(") {
            ++i;
            int64_t v = expr();
            if (i < end && t[i].k == Tok::Punc && t[i].s == ")") ++i;
            else ok = false;
            return v;
        }
        ok = false;
        return 0;
    }

    int64_t unary() {
        if (i < end && t[i].k == Tok::Punc) {
            const std::string& op = t[i].s;
            if (op == "-") {
                ++i;
                return -unary();
            }
            if (op == "+") {
                ++i;
                return unary();
            }
            if (op == "~") {
                ++i;
                return ~unary();
            }
            if (op == "!") {
                ++i;
                return unary() == 0 ? 1 : 0;
            }
        }
        return primary();
    }

    int64_t mul() {
        int64_t v = unary();
        while (i < end && t[i].k == Tok::Punc &&
               (t[i].s == "*" || t[i].s == "/" || t[i].s == "%")) {
            std::string op = t[i++].s;
            int64_t r = unary();
            if (op == "*") v *= r;
            else if (r == 0) {
                ok = false;
                return 0;
            } else if (op == "/") v /= r;
            else v %= r;
        }
        return v;
    }

    int64_t add() {
        int64_t v = mul();
        while (i < end && t[i].k == Tok::Punc && (t[i].s == "+" || t[i].s == "-")) {
            std::string op = t[i++].s;
            int64_t r = mul();
            v = op == "+" ? v + r : v - r;
        }
        return v;
    }

    int64_t shift() {
        int64_t v = add();
        while (i < end && t[i].k == Tok::Punc && (t[i].s == "<<" || t[i].s == ">>")) {
            std::string op = t[i++].s;
            int64_t r = add();
            if (r < 0 || r > 63) {
                ok = false;
                return 0;
            }
            v = op == "<<" ? (v << r) : (v >> r);
        }
        return v;
    }

    int64_t band() {
        int64_t v = shift();
        while (i < end && t[i].k == Tok::Punc && t[i].s == "&") {
            ++i;
            v &= shift();
        }
        return v;
    }

    int64_t bxor() {
        int64_t v = band();
        while (i < end && t[i].k == Tok::Punc && t[i].s == "^") {
            ++i;
            v ^= band();
        }
        return v;
    }

    int64_t bor() {
        int64_t v = bxor();
        while (i < end && t[i].k == Tok::Punc && t[i].s == "|") {
            ++i;
            v |= bxor();
        }
        return v;
    }

    int64_t expr() { return bor(); }
};
}  // namespace

std::optional<int64_t> eval_const(const std::vector<Tok>& t, size_t b, size_t e,
                                  const std::map<std::string, int64_t>& env) {
    Eval ev{t, b, e, env};
    int64_t v = ev.expr();
    if (!ev.ok || ev.i != e) return std::nullopt;
    return v;
}

}  // namespace pyadamas
