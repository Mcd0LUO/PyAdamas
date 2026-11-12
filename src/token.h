// token.h —— 词法单元定义
//
// 覆盖 Python 3 的完整 token 集合（含暂不支持的语法对应的 token），
// 这样后续扩展语法时不用再动词法器。
#pragma once

#include <cstdint>
#include <string>

namespace pyadamas {

enum class Tok : uint8_t {
    END,       // 文件结束
    NEWLINE,   // 逻辑行结束
    INDENT,    // 缩进增加
    DEDENT,    // 缩进减少

    // 字面量
    INT,
    FLOAT,
    COMPLEX,
    STRING,    // 普通字符串（text 为反转义后的内容）
    BYTES,     // 字节串（lexer 只负责切分，语义层不支持）
    FSTRING,   // f-string（text 为引号内原文，反转义与插值由 parser 处理）
    NAME,

    // 关键字（按字母序，便于核对完整性）
    KW_AND, KW_AS, KW_ASSERT, KW_ASYNC, KW_AWAIT,
    KW_BREAK, KW_CLASS, KW_CONTINUE,
    KW_DEF, KW_DEL, KW_ELIF, KW_ELSE, KW_EXCEPT,
    KW_FALSE, KW_FINALLY, KW_FOR, KW_FROM,
    KW_GLOBAL, KW_IF, KW_IMPORT, KW_IN, KW_IS,
    KW_LAMBDA, KW_NONE, KW_NONLOCAL, KW_NOT,
    KW_OR, KW_PASS, KW_RAISE, KW_RETURN,
    KW_TRUE, KW_TRY, KW_WHILE, KW_WITH, KW_YIELD,

    // 算术 / 位运算
    PLUS, MINUS, STAR, DOUBLESTAR, SLASH, DOUBLESLASH, PERCENT, AT,
    LSHIFT, RSHIFT, AMPER, VBAR, CIRCUMFLEX, TILDE,

    // 比较
    LESS, GREATER, LESSEQUAL, GREATEREQUAL, EQUALEQUAL, NOTEQUAL,

    // 分隔符
    LPAREN, RPAREN, LBRACKET, RBRACKET, LBRACE, RBRACE,
    COMMA, COLON, COLONEQUAL, DOT, SEMICOLON, ELLIPSIS,

    // 赋值
    EQUAL, PLUSEQUAL, MINUSEQUAL, STAREQUAL, SLASHEQUAL, DOUBLESLASHEQUAL,
    PERCENTEQUAL, ATEQUAL, AMPEREQUAL, VBAREQUAL, CIRCUMFLEXEQUAL,
    LSHIFTEQUAL, RSHIFTEQUAL, DOUBLESTAREQUAL,

    // 其他
    RARROW,   // ->
};

struct Token {
    Tok kind = Tok::END;
    std::string text;   // NAME 的标识符、STRING/FSTRING 的内容、数字原文
    int64_t ival = 0;   // INT 的值
    double fval = 0.0;  // FLOAT 的值
    // 字符串是否带 r 前缀。FSTRING 需要它来决定字面量部分是否解码转义；
    // 普通字符串在词法层已经解码完，这个标记只对 f-string 有意义。
    bool raw = false;
    int line = 1;
    int col = 1;
};

// 调试输出用的可读名称
inline const char* tok_name(Tok t) {
    switch (t) {
        case Tok::END: return "END";
        case Tok::NEWLINE: return "NEWLINE";
        case Tok::INDENT: return "INDENT";
        case Tok::DEDENT: return "DEDENT";
        case Tok::INT: return "INT";
        case Tok::FLOAT: return "FLOAT";
        case Tok::COMPLEX: return "COMPLEX";
        case Tok::STRING: return "STRING";
        case Tok::BYTES: return "BYTES";
        case Tok::FSTRING: return "FSTRING";
        case Tok::NAME: return "NAME";
        case Tok::KW_AND: return "and";
        case Tok::KW_AS: return "as";
        case Tok::KW_ASSERT: return "assert";
        case Tok::KW_ASYNC: return "async";
        case Tok::KW_AWAIT: return "await";
        case Tok::KW_BREAK: return "break";
        case Tok::KW_CLASS: return "class";
        case Tok::KW_CONTINUE: return "continue";
        case Tok::KW_DEF: return "def";
        case Tok::KW_DEL: return "del";
        case Tok::KW_ELIF: return "elif";
        case Tok::KW_ELSE: return "else";
        case Tok::KW_EXCEPT: return "except";
        case Tok::KW_FALSE: return "False";
        case Tok::KW_FINALLY: return "finally";
        case Tok::KW_FOR: return "for";
        case Tok::KW_FROM: return "from";
        case Tok::KW_GLOBAL: return "global";
        case Tok::KW_IF: return "if";
        case Tok::KW_IMPORT: return "import";
        case Tok::KW_IN: return "in";
        case Tok::KW_IS: return "is";
        case Tok::KW_LAMBDA: return "lambda";
        case Tok::KW_NONE: return "None";
        case Tok::KW_NONLOCAL: return "nonlocal";
        case Tok::KW_NOT: return "not";
        case Tok::KW_OR: return "or";
        case Tok::KW_PASS: return "pass";
        case Tok::KW_RAISE: return "raise";
        case Tok::KW_RETURN: return "return";
        case Tok::KW_TRUE: return "True";
        case Tok::KW_TRY: return "try";
        case Tok::KW_WHILE: return "while";
        case Tok::KW_WITH: return "with";
        case Tok::KW_YIELD: return "yield";
        case Tok::PLUS: return "+";
        case Tok::MINUS: return "-";
        case Tok::STAR: return "*";
        case Tok::DOUBLESTAR: return "**";
        case Tok::SLASH: return "/";
        case Tok::DOUBLESLASH: return "//";
        case Tok::PERCENT: return "%";
        case Tok::AT: return "@";
        case Tok::LSHIFT: return "<<";
        case Tok::RSHIFT: return ">>";
        case Tok::AMPER: return "&";
        case Tok::VBAR: return "|";
        case Tok::CIRCUMFLEX: return "^";
        case Tok::TILDE: return "~";
        case Tok::LESS: return "<";
        case Tok::GREATER: return ">";
        case Tok::LESSEQUAL: return "<=";
        case Tok::GREATEREQUAL: return ">=";
        case Tok::EQUALEQUAL: return "==";
        case Tok::NOTEQUAL: return "!=";
        case Tok::LPAREN: return "(";
        case Tok::RPAREN: return ")";
        case Tok::LBRACKET: return "[";
        case Tok::RBRACKET: return "]";
        case Tok::LBRACE: return "{";
        case Tok::RBRACE: return "}";
        case Tok::COMMA: return ",";
        case Tok::COLON: return ":";
        case Tok::COLONEQUAL: return ":=";
        case Tok::DOT: return ".";
        case Tok::SEMICOLON: return ";";
        case Tok::ELLIPSIS: return "...";
        case Tok::EQUAL: return "=";
        case Tok::PLUSEQUAL: return "+=";
        case Tok::MINUSEQUAL: return "-=";
        case Tok::STAREQUAL: return "*=";
        case Tok::SLASHEQUAL: return "/=";
        case Tok::DOUBLESLASHEQUAL: return "//=";
        case Tok::PERCENTEQUAL: return "%=";
        case Tok::ATEQUAL: return "@=";
        case Tok::AMPEREQUAL: return "&=";
        case Tok::VBAREQUAL: return "|=";
        case Tok::CIRCUMFLEXEQUAL: return "^=";
        case Tok::LSHIFTEQUAL: return "<<=";
        case Tok::RSHIFTEQUAL: return ">>=";
        case Tok::DOUBLESTAREQUAL: return "**=";
        case Tok::RARROW: return "->";
    }
    return "?";
}

inline bool is_keyword(Tok t) {
    return t >= Tok::KW_AND && t <= Tok::KW_YIELD;
}

// 是否是复合赋值运算符（用于 parser 判断）
inline bool is_augassign(Tok t) {
    return t >= Tok::PLUSEQUAL && t <= Tok::DOUBLESTAREQUAL;
}

}  // namespace pyadamas
