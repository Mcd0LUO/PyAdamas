// lexer.h —— 缩进敏感的 Python 词法分析器
//
// 负责三件 Python 特有的事：
//   1. 逻辑行：括号内的换行不结束语句（隐式续行），行尾 \ 也续行
//   2. 缩进：行首宽度变化产生 INDENT / DEDENT token
//   3. 字符串前缀：r"" / f"" / fr"" 等组合
#pragma once

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <format>

#include "token.h"

namespace pyadamas {

struct LexError : std::runtime_error {
    int line, col;
    LexError(std::string msg, int ln, int cl)
        : std::runtime_error(std::move(msg)), line(ln), col(cl) {}
};

// ── 转义解码 ──────────────────────────────────────────────────────
// 词法层（普通字符串）和语法层（f-string 的字面量片段）都要用，
// 所以放在命名空间里，而不是 Lexer 的私有成员。

inline void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// 解码一个转义序列。i 指向反斜杠，返回时指向序列之后。
// 非法序列（\x 后不足两位十六进制等）返回 false，out 保持不变。
// 说明：\N{NAME} 需要完整的 Unicode 名称表，这里按「未知转义」原样保留。
//
// is_bytes 决定 \xNN 与八进制转义的语义：
//   str  ：\xNN 是 U+00NN，必须编成 UTF-8（保证 str 永远是合法 UTF-8）
//   bytes：\xNN 与 \NNN 都是原始字节（CPython 语义）
// \u/\U/\N 在 bytes 字面量里按 CPython 报错。
inline bool decode_escape(std::string_view s, size_t& i, std::string& out,
                          bool is_bytes = false) {
    if (i >= s.size() || s[i] != '\\') return false;
    size_t p = i + 1;
    if (p >= s.size()) {  // 末尾孤立的反斜杠
        out.push_back('\\');
        i = p;
        return true;
    }
    char e = s[p++];
    auto read_hex = [&](int n, uint32_t& v) -> bool {
        if (p + static_cast<size_t>(n) > s.size()) return false;
        v = 0;
        for (int k = 0; k < n; ++k) {
            char h = s[p + k];
            int d;
            if (h >= '0' && h <= '9') d = h - '0';
            else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
            else return false;
            v = v * 16 + static_cast<uint32_t>(d);
        }
        p += static_cast<size_t>(n);
        return true;
    };
    switch (e) {
        case '\n': break;  // 续行：反斜杠和换行都丢弃
        case '\\': out.push_back('\\'); break;
        case '\'': out.push_back('\''); break;
        case '"': out.push_back('"'); break;
        case 'a': out.push_back('\a'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'v': out.push_back('\v'); break;
        case 'x': {
            uint32_t v;
            if (!read_hex(2, v)) return false;
            if (is_bytes) out.push_back(static_cast<char>(v));
            else append_utf8(out, v);  /* str：U+00NN 编成 UTF-8 */
            break;
        }
        case 'u': {
            uint32_t v;
            if (!read_hex(4, v)) return false;
            if (is_bytes) return false;  /* bytes 字面量不允许 \u */
            append_utf8(out, v);
            break;
        }
        case 'U': {
            uint32_t v;
            if (!read_hex(8, v) || v > 0x10FFFF) return false;
            if (is_bytes) return false;  /* bytes 字面量不允许 \U */
            append_utf8(out, v);
            break;
        }
        case 'N': {
            if (is_bytes) return false;  /* bytes 字面量不允许 \N */
            // \N{NAME}：按名取 Unicode 字符。名字表太大，这里保留原文
            // （与「未知转义保留反斜杠」一致），但必须整体吃掉，
            // 否则 f-string 里 \N{...} 的花括号会被当成替换字段。
            if (p < s.size() && s[p] == '{') {
                size_t close = s.find('}', p);
                if (close == std::string_view::npos) return false;
                out.append(s.substr(i, close - i + 1));
                i = close + 1;
                return true;
            }
            out.push_back('\\');
            out.push_back(e);
            break;
        }
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
            uint32_t v = static_cast<uint32_t>(e - '0');
            for (int k = 0; k < 2 && p < s.size() && s[p] >= '0' && s[p] <= '7'; ++k)
                v = v * 8 + static_cast<uint32_t>(s[p++] - '0');
            if (is_bytes) {
                if (v > 0xFF) return false;  /* CPython：bytes 里 \NNN 必须 <= 0o377 */
                out.push_back(static_cast<char>(v));
            } else {
                append_utf8(out, v);
            }
            break;
        }
        default:
            // 未知转义：Python 保留反斜杠
            out.push_back('\\');
            out.push_back(e);
    }
    i = p;
    return true;
}

inline bool decode_escapes(std::string_view raw, std::string& out,
                           bool is_bytes = false) {
    out.reserve(raw.size());
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] == '\\') {
            if (!decode_escape(raw, i, out, is_bytes)) return false;
        } else {
            out.push_back(raw[i++]);
        }
    }
    return true;
}

class Lexer {
public:
    // 换行统一成 \n：源码里的 \r\n / \r 会让行号、字符串内容与列号全部错位。
    // 大多数文件不含 \r，走零拷贝的 string_view 路径。
    explicit Lexer(std::string_view src) {
        if (src.find('\r') != std::string_view::npos) {
            owned_.reserve(src.size());
            for (size_t i = 0; i < src.size(); ++i) {
                char c = src[i];
                if (c == '\r') {
                    owned_.push_back('\n');
                    if (i + 1 < src.size() && src[i + 1] == '\n') ++i;
                } else {
                    owned_.push_back(c);
                }
            }
            src_ = strip_bom(owned_);
        } else {
            src_ = strip_bom(src);
        }
    }

    std::vector<Token> run() {
        tokens_.reserve(src_.size() / 6 + 16);
        while (!at_end()) {
            if (line_start_) {
                if (paren_depth_ == 0) {
                    if (!handle_line_start()) break;
                } else {
                    // 括号内的续行不参与缩进计算，但行首标记必须清掉，
                    // 否则 ')' 闭合后同行剩下的 token 会被当成新行行首
                    line_start_ = false;
                }
            }
            skip_hspace();
            if (at_end()) break;

            char c = peek();
            if (c == '\n') {
                advance();
                if (paren_depth_ == 0) emit_logical_newline();
                ++line_;
                line_begin_ = pos_;
                line_start_ = true;
                continue;
            }
            if (c == '#') {
                while (!at_end() && peek() != '\n') advance();
                continue;
            }
            scan_token();
        }

        finish();
        return std::move(tokens_);
    }

private:
    std::string owned_;       // 仅在需要归一化换行时使用
    std::string_view src_;
    size_t pos_ = 0;
    int line_ = 1;
    size_t line_begin_ = 0;   // 本行第一个字节的位置，用来算列号
    bool line_start_ = true;
    int paren_depth_ = 0;
    int tok_col_ = 1;         // 当前 token 的起始列
    int tok_line_ = 1;        // 当前 token 的起始行（多行字符串要用）
    std::vector<int> indents_{0};
    std::vector<Token> tokens_;

    // ---- 基础 ----
    static std::string_view strip_bom(std::string_view s) {
        if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
            static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF)
            return s.substr(3);
        return s;
    }

    bool at_end() const { return pos_ >= src_.size(); }
    char peek(size_t off = 0) const { return pos_ + off < src_.size() ? src_[pos_ + off] : '\0'; }
    char advance() { return src_[pos_++]; }
    int col() const { return static_cast<int>(pos_ - line_begin_) + 1; }

    void emit(Tok k, std::string text = {}, int line = -1, int col = -1) {
        Token t;
        t.kind = k;
        t.text = std::move(text);
        t.line = (line < 0) ? tok_line_ : line;
        t.col = (col < 0) ? tok_col_ : col;
        tokens_.push_back(std::move(t));
    }

    void emit_logical_newline() {
        // 连续的空行/注释行不重复产生 NEWLINE
        if (!tokens_.empty()) {
            Tok last = tokens_.back().kind;
            if (last == Tok::NEWLINE || last == Tok::INDENT || last == Tok::DEDENT) return;
        } else {
            return;  // 文件开头的空行
        }
        emit(Tok::NEWLINE, {}, line_, col());
    }

    void skip_hspace() {
        while (!at_end() && (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\f'))
            advance();
    }

    // ---- 缩进 ----
    // 返回值：false 表示已到文件末尾
    bool handle_line_start() {
        int width = 0;
        size_t start_pos = pos_;
        while (!at_end() && (peek() == ' ' || peek() == '\t' || peek() == '\f')) {
            if (peek() == '\t') width = (width / 8 + 1) * 8;  // CPython 语义：tab 展开到 8 的倍数
            else if (peek() == '\f') width = 0;
            else ++width;
            advance();
        }
        line_start_ = false;

        if (at_end()) { pos_ = start_pos; return false; }
        char c = peek();
        if (c == '\n' || c == '#') return true;   // 空行 / 注释行，不影响缩进栈

        if (width > indents_.back()) {
            indents_.push_back(width);
            emit(Tok::INDENT, {}, line_, 1);
            tok_col_ = col();
            return true;
        }
        while (width < indents_.back()) {
            indents_.pop_back();
            emit(Tok::DEDENT, {}, line_, 1);
            if (indents_.empty()) {
                indents_.push_back(0);
                break;
            }
        }
        if (width != indents_.back())
            throw LexError("unindent does not match any outer indentation level", line_, 1);
        tok_col_ = col();
        return true;
    }

    void finish() {
        if (!tokens_.empty()) {
            Tok last = tokens_.back().kind;
            if (last != Tok::NEWLINE && last != Tok::INDENT && last != Tok::DEDENT)
                emit(Tok::NEWLINE, {}, line_, col());
        }
        while (indents_.size() > 1) {
            indents_.pop_back();
            emit(Tok::DEDENT, {}, line_, 1);
        }
        emit(Tok::END, {}, line_, 1);
    }

    // ---- 字符分类 ----
    static bool is_digit(char c) { return c >= '0' && c <= '9'; }
    static bool is_quote(char c) { return c == '"' || c == '\''; }
    static bool is_ident_start(unsigned char c) {
        return std::isalpha(c) || c == '_' || c >= 0x80;
    }
    static bool is_ident_char(unsigned char c) { return is_ident_start(c) || std::isdigit(c); }
    static bool is_string_prefix_char(char c) {
        switch (std::tolower(static_cast<unsigned char>(c))) {
            case 'r': case 'b': case 'f': case 'u': return true;
            default: return false;
        }
    }

    // ---- 扫描入口 ----
    void scan_token() {
        tok_col_ = col();
        tok_line_ = line_;
        char c = peek();

        // 字符串前缀（r/f/b/u 及两两组合）
        if (is_ident_start(static_cast<unsigned char>(c))) {
            size_t n = 0;
            while (n < 2 && is_string_prefix_char(peek(n))) ++n;
            if (n > 0 && is_quote(peek(n))) {
                std::string prefix(src_.substr(pos_, n));
                pos_ += n;
                scan_string(prefix);
                return;
            }
            scan_name();
            return;
        }

        if (is_quote(c)) { scan_string(""); return; }
        if (is_digit(c) || (c == '.' && is_digit(peek(1)))) { scan_number(); return; }

        switch (c) {
            case '+': advance(); if (peek() == '=') { advance(); emit(Tok::PLUSEQUAL); } else emit(Tok::PLUS); return;
            case '-': advance();
                if (peek() == '=') { advance(); emit(Tok::MINUSEQUAL); }
                else if (peek() == '>') { advance(); emit(Tok::RARROW); }
                else emit(Tok::MINUS);
                return;
            case '*': advance();
                if (peek() == '*') { advance(); if (peek() == '=') { advance(); emit(Tok::DOUBLESTAREQUAL); } else emit(Tok::DOUBLESTAR); }
                else if (peek() == '=') { advance(); emit(Tok::STAREQUAL); }
                else emit(Tok::STAR);
                return;
            case '/': advance();
                if (peek() == '/') { advance(); if (peek() == '=') { advance(); emit(Tok::DOUBLESLASHEQUAL); } else emit(Tok::DOUBLESLASH); }
                else if (peek() == '=') { advance(); emit(Tok::SLASHEQUAL); }
                else emit(Tok::SLASH);
                return;
            case '%': advance(); if (peek() == '=') { advance(); emit(Tok::PERCENTEQUAL); } else emit(Tok::PERCENT); return;
            case '@': advance(); if (peek() == '=') { advance(); emit(Tok::ATEQUAL); } else emit(Tok::AT); return;
            case '&': advance(); if (peek() == '=') { advance(); emit(Tok::AMPEREQUAL); } else emit(Tok::AMPER); return;
            case '|': advance(); if (peek() == '=') { advance(); emit(Tok::VBAREQUAL); } else emit(Tok::VBAR); return;
            case '^': advance(); if (peek() == '=') { advance(); emit(Tok::CIRCUMFLEXEQUAL); } else emit(Tok::CIRCUMFLEX); return;
            case '~': advance(); emit(Tok::TILDE); return;
            case '=': advance(); if (peek() == '=') { advance(); emit(Tok::EQUALEQUAL); } else emit(Tok::EQUAL); return;
            case '!': advance();
                if (peek() == '=') { advance(); emit(Tok::NOTEQUAL); }
                else throw LexError("invalid syntax: expected '!='", line_, col());
                return;
            case '<': advance();
                if (peek() == '=') { advance(); emit(Tok::LESSEQUAL); }
                else if (peek() == '<') { advance(); if (peek() == '=') { advance(); emit(Tok::LSHIFTEQUAL); } else emit(Tok::LSHIFT); }
                else emit(Tok::LESS);
                return;
            case '>': advance();
                if (peek() == '=') { advance(); emit(Tok::GREATEREQUAL); }
                else if (peek() == '>') { advance(); if (peek() == '=') { advance(); emit(Tok::RSHIFTEQUAL); } else emit(Tok::RSHIFT); }
                else emit(Tok::GREATER);
                return;
            case '(': advance(); ++paren_depth_; emit(Tok::LPAREN); return;
            case ')': advance();
                if (paren_depth_ > 0) --paren_depth_;
                emit(Tok::RPAREN);
                return;
            case '[': advance(); ++paren_depth_; emit(Tok::LBRACKET); return;
            case ']': advance();
                if (paren_depth_ > 0) --paren_depth_;
                emit(Tok::RBRACKET);
                return;
            case '{': advance(); ++paren_depth_; emit(Tok::LBRACE); return;
            case '}': advance();
                if (paren_depth_ > 0) --paren_depth_;
                emit(Tok::RBRACE);
                return;
            case ',': advance(); emit(Tok::COMMA); return;
            case ':': advance();
                if (peek() == '=') { advance(); emit(Tok::COLONEQUAL); }
                else emit(Tok::COLON);
                return;
            case ';': advance(); emit(Tok::SEMICOLON); return;
            case '.': advance();
                if (peek() == '.' && peek(1) == '.') { advance(); advance(); emit(Tok::ELLIPSIS); }
                else emit(Tok::DOT);
                return;
            case '\\':
                advance();
                if (peek() == '\r') advance();
                if (peek() == '\n') { advance(); ++line_; line_begin_ = pos_; line_start_ = false; }
                else throw LexError("unexpected character after line continuation", line_, col());
                return;
            default:
                throw LexError(std::format("invalid character '{}'", printable(c)), line_, col());
        }
    }

    static std::string printable(char c) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u < 0x7F) return std::string(1, c);
        return std::format("\\x{:02x}", u);
    }

    // ---- 标识符与关键字 ----
    void scan_name() {
        size_t start = pos_;
        while (!at_end() && is_ident_char(static_cast<unsigned char>(peek()))) advance();
        std::string name(src_.substr(start, pos_ - start));

        static const std::unordered_map<std::string, Tok> kws = {
            {"False", Tok::KW_FALSE}, {"None", Tok::KW_NONE}, {"True", Tok::KW_TRUE},
            {"and", Tok::KW_AND}, {"as", Tok::KW_AS}, {"assert", Tok::KW_ASSERT},
            {"async", Tok::KW_ASYNC}, {"await", Tok::KW_AWAIT}, {"break", Tok::KW_BREAK},
            {"cimport", Tok::KW_CIMPORT},
            {"class", Tok::KW_CLASS}, {"continue", Tok::KW_CONTINUE}, {"def", Tok::KW_DEF},
            {"del", Tok::KW_DEL}, {"elif", Tok::KW_ELIF}, {"else", Tok::KW_ELSE},
            {"except", Tok::KW_EXCEPT}, {"finally", Tok::KW_FINALLY}, {"for", Tok::KW_FOR},
            {"from", Tok::KW_FROM}, {"global", Tok::KW_GLOBAL}, {"if", Tok::KW_IF},
            {"import", Tok::KW_IMPORT}, {"in", Tok::KW_IN}, {"is", Tok::KW_IS},
            {"lambda", Tok::KW_LAMBDA}, {"nonlocal", Tok::KW_NONLOCAL}, {"not", Tok::KW_NOT},
            {"or", Tok::KW_OR}, {"pass", Tok::KW_PASS}, {"raise", Tok::KW_RAISE},
            {"return", Tok::KW_RETURN}, {"try", Tok::KW_TRY}, {"while", Tok::KW_WHILE},
            {"with", Tok::KW_WITH}, {"yield", Tok::KW_YIELD},
        };
        if (auto it = kws.find(name); it != kws.end()) emit(it->second);
        else emit(Tok::NAME, std::move(name));
    }

    // ---- 数字 ----
    // Python 允许数字之间用下划线分隔，但不允许出现在首尾或紧贴小数点、
    // 指数符号。CPython 报 "invalid decimal literal"，这里做同样的约束。
    static bool valid_underscores(std::string_view text, int base) {
        auto digit_value = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        auto is_base_digit = [&](char c) {
            int d = digit_value(c);
            return d >= 0 && d < base;
        };
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] != '_') continue;
            if (i == 0 || i + 1 >= text.size()) return false;
            char prev = text[i - 1], next = text[i + 1];
            if (!is_base_digit(next)) return false;
            if (is_base_digit(prev)) continue;
            // 0x_FF / 0b_1010 / 0o_777：下划线允许紧跟在进制前缀后
            if (base != 10 && i == 2 && text[0] == '0' &&
                (prev == 'x' || prev == 'X' || prev == 'o' || prev == 'O' ||
                 prev == 'b' || prev == 'B'))
                continue;
            return false;
        }
        return true;
    }

    void scan_number() {
        size_t start = pos_;
        bool is_float = false;

        // 进制前缀
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'o' || peek(1) == 'O' ||
                              peek(1) == 'b' || peek(1) == 'B')) {
            char base_ch = static_cast<char>(std::tolower(static_cast<unsigned char>(peek(1))));
            pos_ += 2;
            int base = (base_ch == 'x') ? 16 : (base_ch == 'o') ? 8 : 2;
            int64_t value = 0;
            bool any = false;
            while (!at_end()) {
                char d = peek();
                if (d == '_') { advance(); continue; }
                int dv = -1;
                if (d >= '0' && d <= '9') dv = d - '0';
                else if (d >= 'a' && d <= 'f') dv = d - 'a' + 10;
                else if (d >= 'A' && d <= 'F') dv = d - 'A' + 10;
                if (dv < 0 || dv >= base) break;
                value = value * base + dv;
                any = true;
                advance();
            }
            std::string_view text = src_.substr(start, pos_ - start);
            if (!any) throw LexError("invalid number literal", tok_line_, tok_col_);
            if (!valid_underscores(text, base))
                throw LexError("invalid underscore in number literal", tok_line_, tok_col_);
            // 0b12 / 0o8 这类：数字本身合法，但紧跟的字符不是合法数字
            if (std::isalnum(static_cast<unsigned char>(peek())))
                throw LexError(
                    std::format("invalid digit '{}' in base-{} literal{}", peek(), base,
                                peek() == 'L' || peek() == 'l'
                                    ? " (Python 2 long suffix is not supported)"
                                    : ""),
                    tok_line_, tok_col_);
            Token t;
            t.kind = Tok::INT;
            t.text = std::string(text);
            t.ival = value;
            t.line = tok_line_;
            t.col = tok_col_;
            tokens_.push_back(std::move(t));
            return;
        }

        while (!at_end() && (is_digit(peek()) || peek() == '_')) advance();
        if (peek() == '.') {
            is_float = true;
            advance();
            while (!at_end() && (is_digit(peek()) || peek() == '_')) advance();
        }
        if (peek() == 'e' || peek() == 'E') {
            is_float = true;
            advance();
            if (peek() == '+' || peek() == '-') advance();
            if (!is_digit(peek()))
                throw LexError("invalid exponent in number literal", tok_line_, tok_col_);
            while (!at_end() && (is_digit(peek()) || peek() == '_')) advance();
        }
        bool is_complex = false;
        if (peek() == 'j' || peek() == 'J') {
            is_complex = true;
            advance();
        }

        std::string text(src_.substr(start, pos_ - start));
        if (!valid_underscores(text, 10))
            throw LexError("invalid underscore in number literal", tok_line_, tok_col_);

        Token t;
        t.text = text;
        t.line = tok_line_;
        t.col = tok_col_;

        // 去掉下划线后解析
        std::string clean;
        clean.reserve(text.size());
        for (char ch : text) if (ch != '_') clean.push_back(ch);

        if (is_complex) {
            t.kind = Tok::COMPLEX;
            // 虚部；实部固定为 0（`1+2j` 由二元加法构造）。
            clean.pop_back();
            t.fval = std::strtod(clean.c_str(), nullptr);
        } else if (is_float) {
            t.kind = Tok::FLOAT;
            // 不能用 std::stod：它在溢出时抛 out_of_range，1e400 这种
            // 合法字面量（Python 里就是 inf）会把编译器打成内部错误。
            t.fval = std::strtod(clean.c_str(), nullptr);
        } else {
            t.kind = Tok::INT;
            try {
                t.ival = std::stoll(clean);
            } catch (const std::out_of_range&) {
                // u64 范围（INT64_MAX < v <= 2^64-1）的十进制字面量：按无符号
                // 解析成位模式并打 u64_lit 标记，交给类型检查在 u64 上下文
                // 消费（默认 int 上下文仍报 too large，行为不变）。超 u64
                // 范围照旧报错。进制前缀路径保持原样（既有回绕行为不动）。
                errno = 0;
                char* endp = nullptr;
                unsigned long long uv = std::strtoull(clean.c_str(), &endp, 10);
                if (errno == ERANGE || uv > UINT64_MAX || !endp || *endp != '\0')
                    throw LexError("integer literal too large for int64", tok_line_, tok_col_);
                t.ival = static_cast<int64_t>(uv);
                t.u64_lit = true;
            }
        }
        tokens_.push_back(std::move(t));
    }

    // ---- 字符串 ----
    void scan_string(const std::string& prefix) {
        bool is_raw = false, is_f = false, is_bytes = false;
        for (char c : prefix) {
            switch (std::tolower(static_cast<unsigned char>(c))) {
                case 'r': is_raw = true; break;
                case 'f': is_f = true; break;
                case 'b': is_bytes = true; break;
                default: break;
            }
        }
        char quote = advance();
        bool triple = (peek() == quote && peek(1) == quote);
        if (triple) { advance(); advance(); }
        is_bytes_ = is_bytes;

        // f-string 保留原文（字面量片段由 parser 解码，才能区分 rf"" 与 f""），
        // 普通字符串在词法层直接反转义。
        std::string body;
        Tok kind;
        if (is_f) {
            body = read_fstring_body(quote, triple);
            kind = Tok::FSTRING;
        } else if (is_raw) {
            body = read_raw_body(quote, triple);
            kind = is_bytes ? Tok::BYTES : Tok::STRING;
        } else {
            body = read_escaped_body(quote, triple);
            kind = is_bytes ? Tok::BYTES : Tok::STRING;
        }

        Token t;
        t.kind = kind;
        t.text = std::move(body);
        t.raw = is_raw;
        t.line = tok_line_;
        t.col = tok_col_;
        tokens_.push_back(std::move(t));
    }

    // 检测三引号结束
    bool at_triple_end(char quote, bool triple) const {
        if (!triple) return peek() == quote;
        return peek() == quote && peek(1) == quote && peek(2) == quote;
    }

    void consume_quote(char /*quote*/, bool triple) {
        advance();
        if (triple) { advance(); advance(); }
    }

    // 当前正在扫的字符串是否是 bytes 字面量（决定转义解码语义）。
    bool is_bytes_ = false;

    std::string read_raw_body(char quote, bool triple) {
        std::string out;
        while (!at_end()) {
            if (at_triple_end(quote, triple)) { consume_quote(quote, triple); return out; }
            char c = advance();
            if (c == '\\' && !at_end()) {
                // raw 不解释转义，但反斜杠会保护紧随的引号：r'\'' 是合法的
                out.push_back(c);
                char n = advance();
                if (n == '\n') {
                    // 反斜杠换行在 raw 串里也是续行：Python 允许 r"a\<换行>b"，
                    // 反斜杠和换行都原样保留，只是行号要跟着走
                    ++line_;
                    line_begin_ = pos_;
                }
                out.push_back(n);
                continue;
            }
            if (c == '\n') {
                if (!triple)
                    throw LexError("EOL while scanning string literal", tok_line_, tok_col_);
                ++line_;
                line_begin_ = pos_;
            }
            out.push_back(c);
        }
        throw LexError("unterminated string literal", tok_line_, tok_col_);
    }

    // 先按原文收集（同时维护行号），再统一解码。这样续行、八进制、
    // \x/\u/\U 都只有一份实现，f-string 也能复用。
    // is_bytes_ 决定 \xNN 编成 UTF-8 还是原始字节（见 decode_escape）。
    std::string read_escaped_body(char quote, bool triple) {
        std::string raw;
        while (!at_end()) {
            if (at_triple_end(quote, triple)) {
                consume_quote(quote, triple);
                std::string out;
                if (!decode_escapes(raw, out, is_bytes_))
                    throw LexError("invalid escape sequence in string literal", tok_line_, tok_col_);
                return out;
            }
            char c = advance();
            if (c == '\n') {
                if (!triple)
                    throw LexError("EOL while scanning string literal", tok_line_, tok_col_);
                ++line_;
                line_begin_ = pos_;
                raw.push_back('\n');
                continue;
            }
            if (c == '\\') {
                raw.push_back(c);
                if (!at_end()) {
                    char n = advance();
                    raw.push_back(n);
                    if (n == '\n') {
                        ++line_;
                        line_begin_ = pos_;
                    }
                }
                continue;
            }
            raw.push_back(c);
        }
        throw LexError("unterminated string literal", tok_line_, tok_col_);
    }

    // f-string：保留原文，但正确处理引号与反斜杠以避免提前结束
    // f-string 的 body 要在表达式嵌套区之外才认结束引号（PEP 701），
    // 否则 f"{d["k"]}" 会在内层引号处提前断开。
    std::string read_fstring_body(char quote, bool triple) {
        std::string out;
        int brace_depth = 0;
        // 表达式里可以再写字符串（f'{"{"}'），里面的花括号和引号都
        // 不参与外层的括号计数与结束判断
        char inner_q = 0;
        bool inner_triple = false;
        while (!at_end()) {
            if (inner_q == 0 && brace_depth == 0 && at_triple_end(quote, triple)) {
                consume_quote(quote, triple);
                return out;
            }
            char c = advance();
            if (inner_q != 0) {
                out.push_back(c);
                if (c == '\\' && !at_end()) {
                    out.push_back(advance());
                    continue;
                }
                if (c == '\n') { ++line_; line_begin_ = pos_; }
                if (inner_triple) {
                    if (c == inner_q && peek() == inner_q && peek(1) == inner_q) {
                        out.push_back(advance());
                        out.push_back(advance());
                        inner_q = 0;
                        inner_triple = false;
                    }
                } else if (c == inner_q) {
                    inner_q = 0;
                }
                continue;
            }
            if (brace_depth > 0 && (c == '\'' || c == '"')) {
                inner_q = c;
                inner_triple = (peek() == c && peek(1) == c);
                out.push_back(c);
                if (inner_triple) {
                    out.push_back(advance());
                    out.push_back(advance());
                }
                continue;
            }
            if (c == '\\' && peek() == 'N' && peek(1) == '{') {
                // \N{NAME} 是一个整体，里面的花括号不是替换字段
                out.push_back(c);
                out.push_back(advance());
                out.push_back(advance());
                while (!at_end() && peek() != '}') out.push_back(advance());
                if (!at_end()) out.push_back(advance());
                continue;
            }
            if (c == '\\' && !at_end()) {
                out.push_back(c);
                // 反斜杠只对引号和反斜杠有「保护」作用。\{ 不是转义：
                // 花括号仍然要按语法处理，否则 rf'(\{{)' 会把后面的
                // 引号当成表达式的一部分，一直扫到文件尾。
                char n = peek();
                if (n != quote && n != '\\' && n != '\n') continue;
                advance();
                if (n == '\n') { ++line_; line_begin_ = pos_; }
                out.push_back(n);
                continue;
            }
            if (c == '{') {
                // {{ 是转义的字面量，不进入表达式区
                if (brace_depth == 0 && peek() == '{') {
                    out.push_back(c);
                    out.push_back(advance());
                    continue;
                }
                ++brace_depth;
                out.push_back(c);
                continue;
            }
            if (c == '}') {
                if (brace_depth == 0 && peek() == '}') {
                    out.push_back(c);
                    out.push_back(advance());
                    continue;
                }
                if (brace_depth > 0) --brace_depth;
                out.push_back(c);
                continue;
            }
            if (c == '\n') {
                if (!triple) throw LexError("EOL while scanning f-string", tok_line_, tok_col_);
                ++line_;
                line_begin_ = pos_;
            }
            out.push_back(c);
        }
        throw LexError("unterminated f-string literal", tok_line_, tok_col_);
    }
};

}  // namespace pyadamas
