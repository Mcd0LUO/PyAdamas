// rt_re.c —— re 模块：回溯 VM、Pattern / Match 对象
//
// 语法覆盖 Python re 的常用子集：
//   字面量 . [...] \d\w\s\D\W\S \b\B \A\Z\z ^ $
//   分组 (...) (?:...) (?P<name>...) (?>...) (?#...)
//   断言 (?=...) (?!...) (?<=...) (?<!...)
//   交替 |  量词 * + ? {m,n}，后缀 ? 惰性 / + 占有
//   后向引用 \1..\99 与 (?P=name)
//   内联标志 (?i) (?im-sx) (?i:...) 与 re.I/M/S/X
//
// 字符串按字节匹配（与 pyadamas 的 str 语义一致），不做 Unicode 属性类。
#include "rt_internal.h"

#define RE_I 2
#define RE_M 8
#define RE_S 16
#define RE_X 64
#define RE_STEP_LIMIT 200000000LL

enum {
    RE_CHAR = 1,
    RE_ANY,
    RE_CLASS,
    RE_SPLIT,
    RE_JMP,
    RE_SAVE,
    RE_MATCH,
    RE_BOL,
    RE_EOL,
    RE_WORDB,
    RE_BOS,      // \A
    RE_EOS,      // \Z / \z
    RE_BACKREF,  // x = 组号
    RE_ASSERT,   // x = 种类, y = 子程序, ch = 之后, aux = 最大宽度
    RE_ATOMIC,   // x = 子程序, y = 之后
    RE_LOOP,     // 单指令体 X*/X+ 的 SPLIT（aux 预计算，见 loop_precompute）
};

// 断言种类
enum { AS_LA_POS = 0, AS_LA_NEG = 1, AS_LB_POS = 2, AS_LB_NEG = 3 };

typedef struct {
    int op;
    int x, y;
    int ch;
    int flags;
    int64_t aux;
    unsigned char cls[32];
} ReInst;

typedef struct {
    char* name;
    int group;
} ReName;

// 编译期分析结果（只用于剪枝，不改变匹配语义；见 prog_analyze）。
// 单独一块、按需分配：re.compile 的对象不匹配就不付这笔内存。
//   first_kind = 0 未知 / 1 首字节集 / 2 仅 pos 0 / 3 行首（^ + M）
//   lit/litlen：纯字面量模式的匹配串副本（litlen == 0 表示不是纯字面量）
typedef struct {
    int first_kind;
    int litlen;
    unsigned char first[32];
    unsigned char* lit;
} ReAnalysis;

typedef struct {
    ReInst* code;
    int n, cap;
    int ngroups;
    int flags;
    ReName* names;
    int nnames;
    ReAnalysis* an;  // NULL = 还没分析（惰性，见 find_from）
} ReProg;

enum {
    N_EMPTY = 1,
    N_CHAR,
    N_ANY,
    N_CLASS,
    N_ALT,
    N_STAR,
    N_PLUS,
    N_OPT,
    N_REPEAT,
    N_GROUP,
    N_BOL,
    N_EOL,
    N_WORDB,
    N_BOS,
    N_EOS,
    N_BACKREF,
    N_ASSERT,
    N_ATOMIC,
};

typedef struct ReNode {
    int kind;
    int ch;
    int min, max;
    int greedy;
    int group;
    int negate;
    int flags;
    int sub;  // 断言种类
    unsigned char cls[32];
    struct ReNode* a;
    struct ReNode* b;
    struct ReNode* next;
} ReNode;

typedef struct {
    const unsigned char* p;
    int len;
    int pos;
    int flags;
    int ngroups;
    int err;
    const char* msg;
    int started;
    ReName* names;
    int nnames, names_cap;
} ReParser;

typedef struct {
    const unsigned char* s;
    int64_t len;
    int64_t pos;
    int flags;
    int64_t* caps;
    int64_t ncap;
    int64_t steps;
    int err;
} ReCtx;

static void* re_alloc(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) {
        fputs("pyadamas: panic: out of memory\n", stderr);
        exit(1);
    }
    return p;
}

static void* re_realloc(void* p, size_t n) {
    void* q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("pyadamas: panic: out of memory\n", stderr);
        exit(1);
    }
    return q;
}

static void set_bit(unsigned char* cls, int c) {
    if (c < 0 || c > 255) return;
    cls[c >> 3] |= (unsigned char)(1u << (c & 7));
}

static int test_bit(const unsigned char* cls, int c) {
    if (c < 0 || c > 255) return 0;
    return (cls[c >> 3] >> (c & 7)) & 1;
}

static void add_range(unsigned char* cls, int lo, int hi) {
    for (int c = lo; c <= hi; ++c) set_bit(cls, c);
}

static void add_fold(unsigned char* cls, int c, int flags) {
    set_bit(cls, c);
    if ((flags & RE_I) && c >= 'A' && c <= 'Z') set_bit(cls, c - 'A' + 'a');
    if ((flags & RE_I) && c >= 'a' && c <= 'z') set_bit(cls, c - 'a' + 'A');
}

static void add_class_set(unsigned char* cls, int kind, int negate) {
    unsigned char tmp[32] = {0};
    switch (kind) {
        case 'd': add_range(tmp, '0', '9'); break;
        case 'w':
            add_range(tmp, '0', '9');
            add_range(tmp, 'A', 'Z');
            add_range(tmp, 'a', 'z');
            set_bit(tmp, '_');
            break;
        case 's':
            set_bit(tmp, ' ');
            set_bit(tmp, '\t');
            set_bit(tmp, '\n');
            set_bit(tmp, '\r');
            set_bit(tmp, '\f');
            set_bit(tmp, '\v');
            break;
        default: return;
    }
    if (negate) {
        for (int i = 0; i < 32; ++i) tmp[i] = (unsigned char)~tmp[i];
    }
    for (int i = 0; i < 32; ++i) cls[i] |= tmp[i];
}

static ReNode* node_new(int kind) {
    ReNode* n = (ReNode*)re_alloc(sizeof(ReNode));
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    n->min = 1;
    n->max = 1;
    n->greedy = 1;
    n->group = -1;
    n->sub = -1;
    return n;
}

static void node_free(ReNode* n) {
    while (n) {
        ReNode* next = n->next;
        node_free(n->a);
        node_free(n->b);
        free(n);
        n = next;
    }
}

// 子模式的最大匹配宽度；-1 表示无上界（含后向引用或星号）。
static int64_t node_maxlen(ReNode* n);

static int64_t node_maxlen1(ReNode* n) {
    switch (n->kind) {
        case N_CHAR:
        case N_ANY:
        case N_CLASS: return 1;
        case N_EMPTY:
        case N_BOL:
        case N_EOL:
        case N_BOS:
        case N_EOS:
        case N_WORDB:
        case N_ASSERT: return 0;
        case N_BACKREF: return -1;
        case N_GROUP:
        case N_ATOMIC: return node_maxlen(n->a);
        case N_ALT: {
            int64_t a = node_maxlen(n->a), b = node_maxlen(n->b);
            if (a < 0 || b < 0) return -1;
            return a > b ? a : b;
        }
        case N_STAR:
        case N_PLUS: return -1;
        case N_OPT: return node_maxlen(n->a);
        case N_REPEAT: {
            if (n->max < 0) return -1;
            int64_t a = node_maxlen(n->a);
            if (a < 0) return -1;
            return a * n->max;
        }
        default: return -1;
    }
}

static int64_t node_maxlen(ReNode* n) {
    int64_t total = 0;
    for (; n; n = n->next) {
        int64_t m = node_maxlen1(n);
        if (m < 0) return -1;
        total += m;
    }
    return total;
}

static int peek(ReParser* P) { return P->pos < P->len ? P->p[P->pos] : -1; }

static void perr(ReParser* P, const char* msg) {
    if (!P->err) {
        P->err = 1;
        P->msg = msg;
    }
}

static void name_add(ReParser* P, const unsigned char* s, int len, int group) {
    for (int i = 0; i < P->nnames; ++i) {
        if ((int)strlen(P->names[i].name) == len &&
            memcmp(P->names[i].name, s, (size_t)len) == 0) {
            perr(P, "redefinition of group name");
            return;
        }
    }
    if (P->nnames == P->names_cap) {
        P->names_cap = P->names_cap ? P->names_cap * 2 : 4;
        P->names = (ReName*)re_realloc(P->names, sizeof(ReName) * (size_t)P->names_cap);
    }
    char* d = (char*)re_alloc((size_t)len + 1);
    memcpy(d, s, (size_t)len);
    d[len] = 0;
    P->names[P->nnames].name = d;
    P->names[P->nnames].group = group;
    P->nnames++;
}

static int name_find(ReParser* P, const unsigned char* s, int len) {
    for (int i = 0; i < P->nnames; ++i) {
        if ((int)strlen(P->names[i].name) == len && memcmp(P->names[i].name, s, (size_t)len) == 0)
            return P->names[i].group;
    }
    return -1;
}

static void name_free(ReParser* P) {
    for (int i = 0; i < P->nnames; ++i) free(P->names[i].name);
    free(P->names);
    P->names = NULL;
    P->nnames = P->names_cap = 0;
}

// VERBOSE 模式下跳过空白与 # 注释。
static void skip_ws(ReParser* P) {
    if (!(P->flags & RE_X)) return;
    for (;;) {
        int c = peek(P);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            P->pos++;
            continue;
        }
        if (c == '#') {
            while (P->pos < P->len && P->p[P->pos] != '\n') P->pos++;
            continue;
        }
        return;
    }
}

static ReNode* parse_alt(ReParser* P);

static int hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 解析反斜杠后的一个转义。
// 返回 >=0 表示字面字符；返回 <0 表示特殊标记：
//   -1 已写入 cls    -2 \b    -3 \B    -4 \A    -5 \Z
//   -100-n 后向引用第 n 组
static int parse_escape(ReParser* P, unsigned char* cls, int in_class) {
    if (P->pos >= P->len) {
        perr(P, "bad escape at end of pattern");
        return '\\';
    }
    int c = P->p[P->pos++];
    switch (c) {
        case 'd': add_class_set(cls, 'd', 0); return -1;
        case 'D': add_class_set(cls, 'd', 1); return -1;
        case 'w': add_class_set(cls, 'w', 0); return -1;
        case 'W': add_class_set(cls, 'w', 1); return -1;
        case 's': add_class_set(cls, 's', 0); return -1;
        case 'S': add_class_set(cls, 's', 1); return -1;
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case 'f': return '\f';
        case 'v': return '\v';
        case 'a': return 7;
        case 'b':
            if (in_class) return 8;
            return -2;
        case 'B':
            if (in_class) return 'B';
            return -3;
        case 'A':
            if (in_class) return 'A';
            return -4;
        case 'Z':
            if (in_class) return 'Z';
            return -5;
        case 'z':
            if (in_class) return 'z';
            return -5;
        case 'x': {
            if (P->pos + 1 < P->len) {
                int h = hex_val(P->p[P->pos]);
                int l = hex_val(P->p[P->pos + 1]);
                if (h >= 0 && l >= 0) {
                    P->pos += 2;
                    return h * 16 + l;
                }
            }
            return 'x';
        }
        case '0': {
            int v = 0, cnt = 0;
            while (cnt < 2 && peek(P) >= '0' && peek(P) <= '7') {
                v = v * 8 + (peek(P) - '0');
                P->pos++;
                cnt++;
            }
            return v;
        }
        default:
            if (c >= '1' && c <= '9') {
                if (in_class) {
                    int v = c - '0', cnt = 1;
                    while (cnt < 3 && peek(P) >= '0' && peek(P) <= '7') {
                        v = v * 8 + (peek(P) - '0');
                        P->pos++;
                        cnt++;
                    }
                    return v & 0xFF;
                }
                int n = c - '0';
                while (peek(P) >= '0' && peek(P) <= '9') {
                    int d = n * 10 + (peek(P) - '0');
                    if (d > 99) break;
                    n = d;
                    P->pos++;
                }
                return -100 - n;
            }
            return c;
    }
}

static ReNode* parse_class(ReParser* P) {
    ReNode* n = node_new(N_CLASS);
    n->flags = P->flags;
    int neg = 0;
    if (peek(P) == '^') {
        neg = 1;
        P->pos++;
    }
    while (P->pos < P->len && peek(P) != ']') {
        int c = P->p[P->pos++];
        if (c == '\\') {
            int e = parse_escape(P, n->cls, 1);
            if (e >= 0) add_fold(n->cls, e, P->flags);
            continue;
        }
        if (peek(P) == '-' && P->pos + 1 < P->len && P->p[P->pos + 1] != ']') {
            P->pos++;
            int hi = P->p[P->pos++];
            if (hi == '\\') {
                int e = parse_escape(P, n->cls, 1);
                hi = e >= 0 ? e : 0;
            }
            if (hi >= c) {
                for (int x = c; x <= hi; ++x) add_fold(n->cls, x, P->flags);
            } else {
                add_fold(n->cls, c, P->flags);
                add_fold(n->cls, '-', P->flags);
                add_fold(n->cls, hi, P->flags);
            }
            continue;
        }
        add_fold(n->cls, c, P->flags);
    }
    if (peek(P) != ']') {
        perr(P, "unterminated character set");
        node_free(n);
        return node_new(N_EMPTY);
    }
    P->pos++;
    if (neg) {
        for (int i = 0; i < 32; ++i) n->cls[i] = (unsigned char)~n->cls[i];
    }
    return n;
}

// 内联标志字符 -> 位。返回 0 表示不是标志字符。
static int flag_bit(int c) {
    switch (c) {
        case 'i': return RE_I;
        case 'm': return RE_M;
        case 's': return RE_S;
        case 'x': return RE_X;
        case 'a':
        case 'L':
        case 'u': return 0;  // 接受但无效果（按字节匹配）
        default: return -1;
    }
}

static ReNode* parse_atom(ReParser* P) {
    skip_ws(P);
    int c = peek(P);
    if (c < 0) return node_new(N_EMPTY);
    if (c == '(') {
        P->pos++;
        int group = -1;
        int akind = -1;
        int atomic = 0;
        if (peek(P) == '?') {
            P->pos++;
            int d = peek(P);
            if (d == ':') {
                P->pos++;
            } else if (d == '#') {
                P->pos++;
                while (P->pos < P->len && P->p[P->pos] != ')') P->pos++;
                if (P->pos < P->len) P->pos++;
                return node_new(N_EMPTY);
            } else if (d == 'P') {
                P->pos++;
                if (peek(P) == '<') {
                    P->pos++;
                    int ns = P->pos;
                    while (P->pos < P->len && P->p[P->pos] != '>') P->pos++;
                    if (P->pos >= P->len) {
                        perr(P, "unterminated group name");
                        return node_new(N_EMPTY);
                    }
                    int nlen = P->pos - ns;
                    P->pos++;
                    group = ++P->ngroups;
                    name_add(P, P->p + ns, nlen, group);
                } else if (peek(P) == '=') {
                    P->pos++;
                    int ns = P->pos;
                    while (P->pos < P->len && P->p[P->pos] != ')') P->pos++;
                    int nlen = P->pos - ns;
                    if (peek(P) != ')') {
                        perr(P, "unterminated group name");
                        return node_new(N_EMPTY);
                    }
                    P->pos++;
                    int g = name_find(P, P->p + ns, nlen);
                    if (g < 0) {
                        perr(P, "unknown group name");
                        return node_new(N_EMPTY);
                    }
                    ReNode* r = node_new(N_BACKREF);
                    r->group = g;
                    r->flags = P->flags;
                    return r;
                } else {
                    perr(P, "unknown extension ?P");
                    return node_new(N_EMPTY);
                }
            } else if (d == '=') {
                P->pos++;
                akind = AS_LA_POS;
            } else if (d == '!') {
                P->pos++;
                akind = AS_LA_NEG;
            } else if (d == '<') {
                P->pos++;
                if (peek(P) == '=') {
                    P->pos++;
                    akind = AS_LB_POS;
                } else if (peek(P) == '!') {
                    P->pos++;
                    akind = AS_LB_NEG;
                } else {
                    perr(P, "unknown extension ?<");
                    return node_new(N_EMPTY);
                }
            } else if (d == '>') {
                P->pos++;
                atomic = 1;
            } else if (d == '(') {
                perr(P, "conditional groups are not supported yet");
                return node_new(N_EMPTY);
            } else {
                int add = 0, del = 0, neg = 0, any = 0;
                for (;;) {
                    int f = peek(P);
                    if (f == '-') {
                        neg = 1;
                        P->pos++;
                        continue;
                    }
                    int bit = flag_bit(f);
                    if (bit < 0) break;
                    any = 1;
                    if (neg)
                        del |= bit;
                    else
                        add |= bit;
                    P->pos++;
                }
                if (!any) {
                    perr(P, "unknown extension");
                    return node_new(N_EMPTY);
                }
                if (peek(P) == ')') {
                    P->pos++;
                    if (P->started) {
                        perr(P, "global flags not at the start of the expression");
                        return node_new(N_EMPTY);
                    }
                    P->flags = (P->flags | add) & ~del;
                    return node_new(N_EMPTY);
                }
                if (peek(P) == ':') {
                    P->pos++;
                    int saved = P->flags;
                    P->flags = (P->flags | add) & ~del;
                    ReNode* child = parse_alt(P);
                    if (peek(P) != ')') {
                        perr(P, "missing ')'");
                        node_free(child);
                        return node_new(N_EMPTY);
                    }
                    P->pos++;
                    P->flags = saved;
                    return child;
                }
                perr(P, "bad inline flags");
                return node_new(N_EMPTY);
            }
        } else {
            group = ++P->ngroups;
        }
        ReNode* child = parse_alt(P);
        if (peek(P) != ')') {
            perr(P, "missing ')'");
            node_free(child);
            return node_new(N_EMPTY);
        }
        P->pos++;
        if (akind >= 0) {
            ReNode* a = node_new(N_ASSERT);
            a->sub = akind;
            a->a = child;
            return a;
        }
        if (atomic) {
            ReNode* a = node_new(N_ATOMIC);
            a->a = child;
            return a;
        }
        if (group < 0) return child;
        ReNode* g = node_new(N_GROUP);
        g->group = group;
        g->a = child;
        return g;
    }
    if (c == '[') {
        P->pos++;
        return parse_class(P);
    }
    if (c == '.') {
        P->pos++;
        ReNode* n = node_new(N_ANY);
        n->flags = P->flags;
        return n;
    }
    if (c == '^') {
        P->pos++;
        ReNode* n = node_new(N_BOL);
        n->flags = P->flags;
        return n;
    }
    if (c == '$') {
        P->pos++;
        ReNode* n = node_new(N_EOL);
        n->flags = P->flags;
        return n;
    }
    if (c == '\\') {
        P->pos++;
        unsigned char tmp[32] = {0};
        int e = parse_escape(P, tmp, 0);
        if (e == -1) {
            ReNode* n = node_new(N_CLASS);
            n->flags = P->flags;
            memcpy(n->cls, tmp, sizeof n->cls);
            return n;
        }
        if (e == -2) {
            ReNode* n = node_new(N_WORDB);
            n->negate = 0;
            return n;
        }
        if (e == -3) {
            ReNode* n = node_new(N_WORDB);
            n->negate = 1;
            return n;
        }
        if (e == -4) return node_new(N_BOS);
        if (e == -5) return node_new(N_EOS);
        if (e <= -100) {
            int g = -100 - e;
            if (g > P->ngroups) {
                perr(P, "invalid group reference");
                return node_new(N_EMPTY);
            }
            ReNode* n = node_new(N_BACKREF);
            n->group = g;
            n->flags = P->flags;
            return n;
        }
        ReNode* n = node_new(N_CHAR);
        n->ch = e;
        n->flags = P->flags;
        return n;
    }
    P->pos++;
    ReNode* n = node_new(N_CHAR);
    n->ch = c;
    n->flags = P->flags;
    return n;
}

static ReNode* parse_term(ReParser* P) {
    ReNode* atom = parse_atom(P);
    skip_ws(P);
    int c = peek(P);
    if (c == '*' || c == '+' || c == '?') {
        P->pos++;
        int lazy = 0, poss = 0;
        if (peek(P) == '?') {
            P->pos++;
            lazy = 1;
        } else if (peek(P) == '+') {
            P->pos++;
            poss = 1;
        }
        ReNode* n = node_new(c == '*' ? N_STAR : c == '+' ? N_PLUS : N_OPT);
        n->a = atom;
        n->greedy = !lazy;
        if (poss) {
            ReNode* a = node_new(N_ATOMIC);
            a->a = n;
            return a;
        }
        return n;
    }
    if (c == '{') {
        int save = P->pos++;
        int m = 0, digits = 0;
        while (peek(P) >= '0' && peek(P) <= '9') {
            m = m * 10 + (peek(P) - '0');
            P->pos++;
            digits = 1;
        }
        if (!digits) {
            P->pos = save;
            return atom;
        }
        int n = m;
        if (peek(P) == ',') {
            P->pos++;
            if (peek(P) == '}') {
                n = -1;
            } else {
                n = 0;
                digits = 0;
                while (peek(P) >= '0' && peek(P) <= '9') {
                    n = n * 10 + (peek(P) - '0');
                    P->pos++;
                    digits = 1;
                }
                if (!digits) {
                    perr(P, "bad repeat count");
                    node_free(atom);
                    return node_new(N_EMPTY);
                }
            }
        }
        if (peek(P) != '}') {
            perr(P, "bad repeat count");
            node_free(atom);
            return node_new(N_EMPTY);
        }
        P->pos++;
        if (n >= 0 && n < m) {
            perr(P, "bad repeat range");
            node_free(atom);
            return node_new(N_EMPTY);
        }
        int lazy = 0, poss = 0;
        if (peek(P) == '?') {
            P->pos++;
            lazy = 1;
        } else if (peek(P) == '+') {
            P->pos++;
            poss = 1;
        }
        ReNode* r = node_new(N_REPEAT);
        r->a = atom;
        r->min = m;
        r->max = n;
        r->greedy = !lazy;
        if (poss) {
            ReNode* a = node_new(N_ATOMIC);
            a->a = r;
            return a;
        }
        return r;
    }
    return atom;
}

static ReNode* parse_seq(ReParser* P) {
    ReNode* head = NULL;
    ReNode** tail = &head;
    for (;;) {
        skip_ws(P);
        if (P->pos >= P->len) break;
        int c = peek(P);
        if (c == '|' || c == ')') break;
        ReNode* t = parse_term(P);
        if (!t) break;
        *tail = t;
        while (*tail) tail = &(*tail)->next;
        if (t->kind != N_EMPTY) P->started = 1;
    }
    return head ? head : node_new(N_EMPTY);
}

static ReNode* parse_alt(ReParser* P) {
    ReNode* left = parse_seq(P);
    while (peek(P) == '|') {
        P->pos++;
        ReNode* right = parse_seq(P);
        ReNode* n = node_new(N_ALT);
        n->a = left;
        n->b = right;
        left = n;
    }
    return left;
}

static int emit(ReProg* g, int op) {
    if (g->n == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 64;
        g->code = (ReInst*)re_realloc(g->code, sizeof(ReInst) * (size_t)g->cap);
    }
    int i = g->n++;
    memset(&g->code[i], 0, sizeof(ReInst));
    g->code[i].op = op;
    return i;
}

static void compile_node(ReProg* g, ReNode* n) {
    for (; n; n = n->next) {
        switch (n->kind) {
            case N_EMPTY: break;
            case N_CHAR: {
                int i = emit(g, RE_CHAR);
                g->code[i].ch = n->ch;
                g->code[i].flags = n->flags;
                break;
            }
            case N_ANY: {
                int i = emit(g, RE_ANY);
                g->code[i].flags = n->flags;
                break;
            }
            case N_CLASS: {
                int i = emit(g, RE_CLASS);
                memcpy(g->code[i].cls, n->cls, sizeof g->code[i].cls);
                break;
            }
            case N_BOL: {
                int i = emit(g, RE_BOL);
                g->code[i].flags = n->flags;
                break;
            }
            case N_EOL: {
                int i = emit(g, RE_EOL);
                g->code[i].flags = n->flags;
                break;
            }
            case N_BOS: emit(g, RE_BOS); break;
            case N_EOS: emit(g, RE_EOS); break;
            case N_WORDB: {
                int i = emit(g, RE_WORDB);
                g->code[i].x = n->negate;
                break;
            }
            case N_BACKREF: {
                int i = emit(g, RE_BACKREF);
                g->code[i].x = n->group;
                g->code[i].flags = n->flags;
                break;
            }
            case N_GROUP: {
                int s0 = emit(g, RE_SAVE);
                g->code[s0].x = 2 * n->group;
                compile_node(g, n->a);
                int s1 = emit(g, RE_SAVE);
                g->code[s1].x = 2 * n->group + 1;
                break;
            }
            case N_ASSERT: {
                int ai = emit(g, RE_ASSERT);
                g->code[ai].x = n->sub;
                if (n->sub >= AS_LB_POS) g->code[ai].aux = node_maxlen(n->a);
                g->code[ai].y = g->n;
                compile_node(g, n->a);
                emit(g, RE_MATCH);
                g->code[ai].ch = g->n;
                break;
            }
            case N_ATOMIC: {
                int ai = emit(g, RE_ATOMIC);
                g->code[ai].x = g->n;
                compile_node(g, n->a);
                emit(g, RE_MATCH);
                g->code[ai].y = g->n;
                break;
            }
            case N_ALT: {
                int sp = emit(g, RE_SPLIT);
                g->code[sp].x = g->n;
                compile_node(g, n->a);
                int jp = emit(g, RE_JMP);
                g->code[sp].y = g->n;
                compile_node(g, n->b);
                g->code[jp].x = g->n;
                break;
            }
            case N_STAR: {
                int sp = emit(g, RE_SPLIT);
                int body = g->n;
                compile_node(g, n->a);
                int jp = emit(g, RE_JMP);
                g->code[jp].x = sp;
                int after = g->n;
                g->code[sp].x = n->greedy ? body : after;
                g->code[sp].y = n->greedy ? after : body;
                break;
            }
            case N_PLUS: {
                int body = g->n;
                compile_node(g, n->a);
                int sp = emit(g, RE_SPLIT);
                int after = g->n;
                g->code[sp].x = n->greedy ? body : after;
                g->code[sp].y = n->greedy ? after : body;
                break;
            }
            case N_OPT: {
                int sp = emit(g, RE_SPLIT);
                int body = g->n;
                compile_node(g, n->a);
                int after = g->n;
                g->code[sp].x = n->greedy ? body : after;
                g->code[sp].y = n->greedy ? after : body;
                break;
            }
            case N_REPEAT: {
                for (int i = 0; i < n->min; ++i) compile_node(g, n->a);
                if (n->max < 0) {
                    ReNode star;
                    memset(&star, 0, sizeof star);
                    star.kind = N_STAR;
                    star.a = n->a;
                    star.greedy = n->greedy;
                    compile_node(g, &star);
                } else {
                    for (int i = n->min; i < n->max; ++i) {
                        ReNode opt;
                        memset(&opt, 0, sizeof opt);
                        opt.kind = N_OPT;
                        opt.a = n->a;
                        opt.greedy = n->greedy;
                        compile_node(g, &opt);
                    }
                }
                break;
            }
        }
    }
}

static void prog_free(ReProg* g) {
    free(g->code);
    g->code = NULL;
    g->n = g->cap = 0;
    for (int i = 0; i < g->nnames; ++i) free(g->names[i].name);
    free(g->names);
    g->names = NULL;
    g->nnames = 0;
    if (g->an) {
        free(g->an->lit);
        free(g->an);
        g->an = NULL;
    }
}

// 纯字面量查找：memchr + memcmp 的朴素实现。不用 GNU 的 memmem——保留
// 平台可移植性（Windows 运行时有独立的移植层，见 WIN_PORT_CONTEXT.md）。
static const unsigned char* lit_find(const unsigned char* h, size_t hn,
                                     const unsigned char* n, size_t nn) {
    if (nn == 0) return h;
    if (nn > hn) return NULL;
    const unsigned char* p = h;
    size_t left = hn - nn + 1;
    while (left > 0) {
        const unsigned char* q = (const unsigned char*)memchr(p, n[0], left);
        if (!q) return NULL;
        if (memcmp(q, n, nn) == 0) return q;
        left -= (size_t)(q - p) + 1;
        p = q + 1;
    }
    return NULL;
}

// ── 编译期分析：为「搜索起点剪枝」与「纯字面量」两条快路径取证 ──
//
// 分析只看程序前端的形状，结果只用于**排除不可能匹配的起点**，不参与
// 匹配本身，因此不改变任何语义（漏分析只是退回慢路径）。结论有四类：
//   first_kind 0  未知（含零宽断言/后向引用/宽子程序）→ 逐位尝试
//   first_kind 1  首个消费指令是字面字符/类/. → 首字节集 first[32]
//   first_kind 2  程序以 \A 或非 MULTILINE 的 ^ 开头 → 只有 pos 0 可能
//   first_kind 3  MULTILINE 的 ^ → 只有行首可能
// lit/litlen：整条模式是纯字面量（可整体走 memmem）。
static void first_all(unsigned char* s) { memset(s, 0xFF, 32); }

// 返回 1 表示已确定起点约束（kind 2/3）或首字节集（kind 1），0 表示未知。
// 从程序入口线性走到第一个「消费或锚定」指令；只认 SAVE/JMP 这些零成本
// 转运，遇到零宽断言/后向引用/宽子程序就放弃（保守）。
static int prog_first(ReProg* g, int pc, unsigned char* out, int* kind, int depth) {
    if (depth > 64) return 0;
    for (int guard = 0; guard < 4096; ++guard) {
        if (pc < 0 || pc >= g->n) return 0;
        ReInst* in = &g->code[pc];
        switch (in->op) {
            case RE_SAVE: pc++; continue;
            case RE_JMP: pc = in->x; continue;
            case RE_BOS:
                *kind = 2;
                return 1;
            case RE_BOL:
                *kind = (in->flags & RE_M) ? 3 : 2;
                return 1;
            case RE_CHAR:
                *kind = 1;
                set_bit(out, in->ch);
                if (in->flags & RE_I) {
                    if (in->ch >= 'A' && in->ch <= 'Z') set_bit(out, in->ch - 'A' + 'a');
                    if (in->ch >= 'a' && in->ch <= 'z') set_bit(out, in->ch - 'a' + 'A');
                }
                return 1;
            case RE_ANY:
                *kind = 1;
                first_all(out);
                if (!(in->flags & RE_S)) {
                    out['\n' >> 3] &= (unsigned char)~(1u << ('\n' & 7));
                }
                return 1;
            case RE_CLASS:
                *kind = 1;
                memcpy(out, in->cls, 32);
                return 1;
            case RE_SPLIT: {
                unsigned char l[32], r[32];
                int lk = 0, rk = 0;
                memset(l, 0, sizeof l);
                memset(r, 0, sizeof r);
                if (!prog_first(g, in->x, l, &lk, depth + 1)) return 0;
                if (!prog_first(g, in->y, r, &rk, depth + 1)) return 0;
                if (lk != 1 || rk != 1) return 0;  // 锚定/未知分支不合并
                *kind = 1;
                for (int i = 0; i < 32; ++i) out[i] |= l[i];
                for (int i = 0; i < 32; ++i) out[i] |= r[i];
                return 1;
            }
            default: return 0;
        }
    }
    return 0;
}

static int prog_build(ReProg* g, const unsigned char* pat, int len, int flags, const char** err) {
    memset(g, 0, sizeof(*g));
    g->flags = flags;
    ReParser P;
    memset(&P, 0, sizeof P);
    P.p = pat;
    P.len = len;
    P.flags = flags;
    ReNode* root = parse_alt(&P);
    if (P.err) {
        if (err) *err = P.msg;
        node_free(root);
        name_free(&P);
        return 0;
    }
    if (P.pos != P.len) {
        if (err) *err = "unexpected ')'";
        node_free(root);
        name_free(&P);
        return 0;
    }
    g->ngroups = P.ngroups;
    g->names = P.names;
    g->nnames = P.nnames;
    int s0 = emit(g, RE_SAVE);
    g->code[s0].x = 0;
    compile_node(g, root);
    int s1 = emit(g, RE_SAVE);
    g->code[s1].x = 1;
    emit(g, RE_MATCH);
    node_free(root);
    return 1;
}

static void loop_precompute(ReProg* g);

// 编译完成后的一次性分析：首字节集/锚定约束 + 纯字面量判定。
static void prog_analyze(ReProg* g) {
    ReAnalysis* an = (ReAnalysis*)re_alloc(sizeof(ReAnalysis));
    memset(an, 0, sizeof *an);
    g->an = an;
    unsigned char f[32];
    int kind = 0;
    memset(f, 0, sizeof f);
    if (prog_first(g, 0, f, &kind, 0)) {
        an->first_kind = kind;
        memcpy(an->first, f, sizeof f);
    }
    loop_precompute(g);
    // 纯字面量形态：SAVE0, RE_CHAR+（同一 flags、无 IGNORECASE）, SAVE1, MATCH。
    if (g->n >= 4 && g->code[0].op == RE_SAVE && g->code[0].x == 0) {
        int i = 1, lit0 = 1, flags0 = g->code[1].flags;
        while (i < g->n && g->code[i].op == RE_CHAR && g->code[i].flags == flags0 &&
               g->code[i].ch >= 0 && g->code[i].ch <= 255)
            i++;
        int nlit = i - lit0;
        if (nlit > 0 && i + 2 == g->n && g->code[i].op == RE_SAVE && g->code[i].x == 1 &&
            g->code[i + 1].op == RE_MATCH && !(flags0 & RE_I)) {
            an->lit = (unsigned char*)re_alloc((size_t)nlit);
            for (int k = 0; k < nlit; ++k)
                an->lit[k] = (unsigned char)g->code[lit0 + k].ch;
            an->litlen = nlit;
        }
    }
}

// ── 模式编译缓存 ────────────────────────────────────────────────────
//
// 模块级 API（re.findall/re.split/re.sub/re.search...）每次调用都把模式
// 字符串重新解析+编译一遍，而实际负载几乎总是同一批固定模式反复调用。
// 这里按 (pattern 字节, flags) 缓存编译产物（小 LRU，8 槽）：
//   - 命中只比较字节 + flags，不解析；
//   - 槽位在借用期间 pin 住，用户回调（sub 的 repl 函数）里再调 re 不会
//     把外层正在用的程序挤掉；8 槽全被 pin 时退回「现编现用」；
//   - 单线程假设：与运行时其余 119 个 static 全局态一致（M=1）。
//
// 模式串可以含 NUL，所以键是 (len, bytes)，不用 strlen。
#define RE_CACHE_SLOTS 8

typedef struct {
    unsigned char* key;
    int keylen;
    int64_t flags;
    ReProg prog;
    int used;
    int pin;
    unsigned long stamp;
} ReCacheEnt;

static ReCacheEnt re_cache[RE_CACHE_SLOTS];
static unsigned long re_cache_stamp;

static int cache_lookup(const unsigned char* pat, int len, int64_t flags) {
    for (int i = 0; i < RE_CACHE_SLOTS; ++i) {
        ReCacheEnt* e = &re_cache[i];
        if (e->used && e->flags == flags && e->keylen == len &&
            (len == 0 || memcmp(e->key, pat, (size_t)len) == 0)) {
            e->stamp = ++re_cache_stamp;
            return i;
        }
    }
    return -1;
}

static int cache_slot_for_insert(void) {
    int victim = -1;
    unsigned long oldest = 0;
    for (int i = 0; i < RE_CACHE_SLOTS; ++i) {
        ReCacheEnt* e = &re_cache[i];
        if (!e->used) return i;
        if (e->pin == 0 && (victim < 0 || e->stamp < oldest)) {
            victim = i;
            oldest = e->stamp;
        }
    }
    return victim;
}

// 取一个已分析好的程序：命中缓存则 pin 住，否则编译并尝试入缓存。
// lease >= 0 表示借用槽位，-1 表示调用方独占（用完 free_prog_lease）。
static ReProg* re_prog_acquire(const unsigned char* pat, int len, int64_t flags,
                               const char** err, int* lease) {
    int hit = cache_lookup(pat, len, flags);
    if (hit >= 0) {
        re_cache[hit].pin++;
        *lease = hit;
        return &re_cache[hit].prog;
    }
    ReProg tmp;
    memset(&tmp, 0, sizeof tmp);
    if (!prog_build(&tmp, pat, len, (int)flags, err)) {
        prog_free(&tmp);
        return NULL;
    }
    prog_analyze(&tmp);
    int slot = cache_slot_for_insert();
    if (slot < 0) {
        ReProg* g = (ReProg*)re_alloc(sizeof(ReProg));
        *g = tmp;
        *lease = -1;
        return g;
    }
    ReCacheEnt* e = &re_cache[slot];
    if (e->used) {
        prog_free(&e->prog);
        free(e->key);
    }
    e->prog = tmp;
    e->key = (unsigned char*)re_alloc((size_t)len + 1);
    if (len > 0) memcpy(e->key, pat, (size_t)len);
    e->key[len] = 0;
    e->keylen = len;
    e->flags = flags;
    e->used = 1;
    e->pin = 1;
    e->stamp = ++re_cache_stamp;
    *lease = slot;
    return &e->prog;
}

static void re_prog_release(ReProg* g, int lease) {
    if (lease >= 0) {
        if (re_cache[lease].pin > 0) re_cache[lease].pin--;
    } else {
        prog_free(g);
        free(g);
    }
}

static int is_word(int c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           c == '_';
}

static int fold_eq(int a, int b) {
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    return a == b;
}

// 单指令「消费型」字节码：只吃一个字节，不碰 caps/pos 之外的状态。
static int is_consumer(const ReInst* in) {
    return in->op == RE_CHAR || in->op == RE_CLASS || in->op == RE_ANY;
}

static int consumer_match(const ReInst* in, int ch) {
    switch (in->op) {
        case RE_CHAR:
            if (in->flags & RE_I) return fold_eq(ch, in->ch);
            return ch == in->ch;
        case RE_CLASS: return test_bit(in->cls, ch);
        case RE_ANY: return (in->flags & RE_S) || ch != '\n';
        default: return 0;
    }
}

// 识别「X* / X+ 且 X 是单指令消费」的循环形状（编译器按 N_STAR/N_PLUS 生成
// 的固定布局）。识别成功返回 1 并给出体指令/出口 pc/贪婪性。
//   X*: pc=SPLIT, pc+1=X, pc+2=JMP->pc, pc+3=after
//   X+: pc-1=X,  pc=SPLIT, pc+1=after
static int loop_shape(ReProg* g, int pc, int* body, int* after, int* greedy) {
    if (pc + 2 < g->n) {
        int b = pc + 1;
        if (is_consumer(&g->code[b]) && g->code[b + 1].op == RE_JMP &&
            g->code[b + 1].x == pc && pc + 3 < g->n) {
            if (g->code[pc].x == b && g->code[pc].y == b + 2) {
                *body = b;
                *after = b + 2;
                *greedy = 1;
                return 1;
            }
            if (g->code[pc].y == b && g->code[pc].x == b + 2) {
                *body = b;
                *after = b + 2;
                *greedy = 0;
                return 1;
            }
        }
    }
    if (pc >= 1) {
        int b = pc - 1;
        if (is_consumer(&g->code[b]) && pc + 1 < g->n) {
            if (g->code[pc].x == b && g->code[pc].y == pc + 1) {
                *body = b;
                *after = pc + 1;
                *greedy = 1;
                return 1;
            }
            if (g->code[pc].y == b && g->code[pc].x == pc + 1) {
                *body = b;
                *after = pc + 1;
                *greedy = 0;
                return 1;
            }
        }
    }
    return 0;
}

// 把循环形状预计算进 SPLIT 指令的 aux（见 prog_analyze）：
//   aux == 0 不是简单循环；否则 low32 = body+1，high32 = after。
// greedy 由 SPLIT 的 x 是否指向 body 决定（贪婪时 x = body）。
static void loop_precompute(ReProg* g) {
    for (int pc = 0; pc < g->n; ++pc) {
        ReInst* in = &g->code[pc];
        if (in->op != RE_SPLIT) continue;
        // 只在「循环后紧跟收尾（after+1 是 MATCH）」时启用：这时末段扫描与
        // 一阶失败剪枝的收益最大；嵌套量词（(a+)+b 之类）没有收尾可剪，
        // 开启只会多付扫描开销（实测 +17%），保持原递归展开。
        int body, after, greedy;
        if (loop_shape(g, pc, &body, &after, &greedy) && after + 1 < g->n &&
            g->code[after + 1].op == RE_MATCH) {
            in->aux = (int64_t)(((uint64_t)(uint32_t)after << 32) |
                                (uint64_t)(uint32_t)(body + 1));
            // 换成专用 op：普通 SPLIT 的热路径不背这个判断（嵌套量词上
            // 每个字符都走 SPLIT，多一次 aux 测试实测 +4% 指令）。
            in->op = RE_LOOP;
        }
    }
}

static int run(ReProg* g, int pc, ReCtx* c);

// 单指令体循环的快路径单独成函数并禁止内联：把它内联进 run() 会让递归
// VM 的栈帧变大、热寄存器分配变差（实测嵌套量词路径 +4% 指令），而快路径
// 本身只在「循环后紧跟收尾」的形状上命中，独立函数没有损失。
static __attribute__((noinline)) int run_loop(ReProg* g, const ReInst* in, int pc,
                                              ReCtx* c) {
    // 简单循环快路径：单指令体的 X*/X+ 用紧循环吃完再逐位回溯。
    // 等价于原递归展开（每个回溯点都调用 run(after)），只是把
    // 「每字符一次递归」换成一次循环 + 出口处的逐位调用。
    int64_t ls = in->aux;
    int body = (int)(uint32_t)(ls & 0xffffffffULL) - 1;
    int after = (int)(uint32_t)((uint64_t)ls >> 32);
    int greedy = (g->code[pc].x == body);
    int64_t p = c->pos, e = p;
    int64_t len = c->len;
    const unsigned char* s = c->s;
    ReInst* bi = &g->code[body];
    // 按指令种类特化紧循环（字节级扫描的最内层）。
    if (bi->op == RE_CLASS) {
        const unsigned char* cls = bi->cls;
        while (e < len && test_bit(cls, s[e])) e++;
    } else if (bi->op == RE_CHAR) {
        int ch = bi->ch;
        if (bi->flags & RE_I) {
            while (e < len && fold_eq(s[e], ch)) e++;
        } else {
            while (e < len && s[e] == ch) e++;
        }
    } else {
        int dotall = bi->flags & RE_S;
        while (e < len && (dotall || s[e] != '\n')) e++;
    }
    // 步数上限是「兜底 runaway 回溯」的护栏：线性扫描也要按扫描字节数
    // 一次性记入，否则超长 subject 上原本会触发 RuntimeError 的场景会
    // 变成静默成功（异常路径与慢路径保持同一口径）。
    c->steps += e - p;
    if (c->steps > RE_STEP_LIMIT) {
        c->err = 1;
        return 0;
    }
    if (greedy) {
        // 一阶快速失败：出口处是「一个消费指令 + MATCH」（X*Y 这类相邻
        // 匹配，Y 必然吃掉一个字节），先把「Y 不可能匹配」的回溯点整段
        // 跳过，剩下的交给逐位 run——只跳过必然失败的尝试。
        int64_t k0 = -1;
        if (after + 1 < g->n && is_consumer(&g->code[after]) &&
            g->code[after + 1].op == RE_MATCH) {
            const ReInst* yi = &g->code[after];
            while (e >= p) {
                if (e >= len) {
                    e--;
                    continue;
                }
                if (consumer_match(yi, s[e])) {
                    k0 = e;
                    break;
                }
                e--;
            }
        }
        for (int64_t k = e; k >= p && k >= k0; --k) {
            c->pos = k;
            if (run(g, after, c)) return 1;
            if (c->err) return 0;
        }
    } else {
        for (int64_t k = p; k <= e; ++k) {
            c->pos = k;
            if (run(g, after, c)) return 1;
            if (c->err) return 0;
        }
    }
    c->pos = p;
    return 0;
}

static int run(ReProg* g, int pc, ReCtx* c) {
    for (;;) {
        if (++c->steps > RE_STEP_LIMIT) {
            c->err = 1;
            return 0;
        }
        ReInst* in = &g->code[pc];
        switch (in->op) {
            case RE_CHAR: {
                if (c->pos >= c->len) return 0;
                int ch = c->s[c->pos];
                if (in->flags & RE_I) {
                    if (!fold_eq(ch, in->ch)) return 0;
                } else if (ch != in->ch) {
                    return 0;
                }
                c->pos++;
                pc++;
                break;
            }
            case RE_ANY:
                if (c->pos >= c->len) return 0;
                if (!(in->flags & RE_S) && c->s[c->pos] == '\n') return 0;
                c->pos++;
                pc++;
                break;
            case RE_CLASS:
                if (c->pos >= c->len) return 0;
                if (!test_bit(in->cls, c->s[c->pos])) return 0;
                c->pos++;
                pc++;
                break;
            case RE_BOL:
                if (!(c->pos == 0 || ((in->flags & RE_M) && c->s[c->pos - 1] == '\n')))
                    return 0;
                pc++;
                break;
            case RE_EOL:
                if (in->flags & RE_M) {
                    if (!(c->pos == c->len || c->s[c->pos] == '\n')) return 0;
                } else {
                    if (!(c->pos == c->len ||
                          (c->pos == c->len - 1 && c->s[c->pos] == '\n')))
                        return 0;
                }
                pc++;
                break;
            case RE_BOS:
                if (c->pos != 0) return 0;
                pc++;
                break;
            case RE_EOS:
                if (c->pos != c->len) return 0;
                pc++;
                break;
            case RE_WORDB: {
                int prev = c->pos > 0 ? is_word(c->s[c->pos - 1]) : 0;
                int cur = c->pos < c->len ? is_word(c->s[c->pos]) : 0;
                int b = prev != cur;
                if (in->x) b = !b;
                if (!b) return 0;
                pc++;
                break;
            }
            case RE_BACKREF: {
                int gi = in->x;
                int64_t a = c->caps[2 * gi], b = c->caps[2 * gi + 1];
                if (a < 0 || b < 0) return 0;
                int64_t n = b - a;
                if (n == 0) {
                    pc++;
                    break;
                }
                if (c->pos + n > c->len) return 0;
                if (in->flags & RE_I) {
                    for (int64_t i = 0; i < n; ++i) {
                        if (!fold_eq(c->s[a + i], c->s[c->pos + i])) return 0;
                    }
                } else if (memcmp(c->s + a, c->s + c->pos, (size_t)n) != 0) {
                    return 0;
                }
                c->pos += n;
                pc++;
                break;
            }
            case RE_ASSERT: {
                int kind = in->x;
                int64_t save = c->pos;
                int64_t stackbuf[32];
                int64_t* snap =
                    c->ncap <= 32 ? stackbuf
                                  : (int64_t*)re_alloc(sizeof(int64_t) * (size_t)c->ncap);
                memcpy(snap, c->caps, sizeof(int64_t) * (size_t)c->ncap);
                int ok = 0;
                if (kind < AS_LB_POS) {
                    ok = run(g, in->y, c);
                    c->pos = save;
                } else {
                    int64_t ml = in->aux;
                    int64_t lo = ml < 0 ? 0 : (save - ml > 0 ? save - ml : 0);
                    for (int64_t s = lo; s <= save; ++s) {
                        c->pos = s;
                        if (run(g, in->y, c) && c->pos == save) {
                            ok = 1;
                            break;
                        }
                        memcpy(c->caps, snap, sizeof(int64_t) * (size_t)c->ncap);
                    }
                    c->pos = save;
                }
                int want = (kind == AS_LA_POS || kind == AS_LB_POS);
                if (ok != want) {
                    memcpy(c->caps, snap, sizeof(int64_t) * (size_t)c->ncap);
                    if (snap != stackbuf) free(snap);
                    return 0;
                }
                if (!want) memcpy(c->caps, snap, sizeof(int64_t) * (size_t)c->ncap);
                if (snap != stackbuf) free(snap);
                pc = in->ch;
                break;
            }
            case RE_ATOMIC: {
                if (!run(g, in->x, c)) return 0;
                pc = in->y;
                break;
            }
            case RE_SPLIT: {
                int64_t save = c->pos;
                if (run(g, in->x, c)) return 1;
                c->pos = save;
                pc = in->y;
                break;
            }
            case RE_LOOP: return run_loop(g, in, pc, c);
            case RE_JMP: pc = in->x; break;
            case RE_SAVE: {
                int64_t old = c->caps[in->x];
                c->caps[in->x] = c->pos;
                if (run(g, pc + 1, c)) return 1;
                c->caps[in->x] = old;
                return 0;
            }
            case RE_MATCH: return 1;
            default: return 0;
        }
    }
}

// 栈上 caps：小分组（<= 64 槽 = 32 组）不 malloc。热循环里每次 API
// 调用都 malloc/free 一次 caps 是白花的开销。
#define RE_CAPS_STACK 64

static int64_t* caps_take(int64_t ncap, int64_t* stack) {
    return ncap <= RE_CAPS_STACK ? stack : (int64_t*)re_alloc(sizeof(int64_t) * (size_t)ncap);
}

static void caps_drop(int64_t* caps, int64_t* stack) {
    if (caps != stack) free(caps);
}

static void caps_fill(int64_t* caps, int64_t ncap, int64_t v) {
    for (int64_t i = 0; i < ncap; ++i) caps[i] = v;
}

// 从 start 起尝试一次匹配。mode: 0 search / 1 match / 2 fullmatch。
//
// 快路径（都不改变语义，只跳过必然失败的起点）：
//   1. 纯字面量模式：search 用 lit_find 定位，match 用 memcmp；
//   2. 编译期求出的首字节集：起点字节不在集合里就不进 VM；
//   3. \A / 非 MULTILINE ^ / MULTILINE ^：起点约束直接判死。
static int find_from(ReProg* g, PyAdamasStr* text, int64_t start, int mode, int64_t* caps) {
    if (!g->an) prog_analyze(g);  // re.compile 的对象首次使用时才分析
    const ReAnalysis* an = g->an;
    int64_t ncap = 2 * (int64_t)(g->ngroups + 1);
    const unsigned char* s = (const unsigned char*)text->data;
    int64_t len = text->len;
    if (start < 0) start = 0;
    if (start > len) return 0;

    if (an->lit && an->litlen > 0 && mode != 2) {
        int64_t L = an->litlen;
        if (L > len - start) return 0;
        if (mode == 1) {
            if (memcmp(s + start, an->lit, (size_t)L) != 0) return 0;
        } else if (L <= len - start) {
            const unsigned char* p =
                lit_find(s + start, (size_t)(len - start), an->lit, (size_t)L);
            if (!p) return 0;
            start = (int64_t)(p - s);
        }
        caps_fill(caps, ncap, -1);
        caps[0] = start;
        caps[1] = start + L;
        return 1;
    }

    // 扫描循环只建一次上下文；每个起点的 caps/steps/err 复位，pos 直接
    // 用绝对偏移（VM 里的 \b/^/$ 一律看绝对位置）。
    ReCtx c;
    memset(&c, 0, sizeof c);
    c.flags = g->flags;
    c.caps = caps;
    c.ncap = ncap;
    // 末尾单点：start == len 时唯一候选就是空匹配（各处调用方的尾巴轮），
    // 直接试一次，不必再进入扫描循环。first_kind 约束照旧生效。
    // 注意 mode==1/2（match/fullmatch）从 start=0 起，走不到这里。
    if (start == len) {
        if (an->first_kind == 2) return 0;  // \A / ^ 只可能 pos 0
        if (an->first_kind == 3 && len > 0 && s[len - 1] != '\n') return 0;
        caps_fill(caps, ncap, -1);
        c.s = s;
        c.len = len;
        c.pos = len;
        c.steps = 0;
        c.err = 0;
        if (!run(g, 0, &c)) {
            if (c.err) pa_raise_c("RuntimeError", "regex step limit exceeded");
            return 0;
        }
        return 1;
    }

    const unsigned char* fb = an->first;
    for (int64_t s0 = start; s0 <= len; ++s0) {
        if (an->first_kind == 2) {
            if (s0 > 0) return 0;  // 只有 pos 0 可能
        } else if (an->first_kind == 3) {
            if (s0 > 0 && s[s0 - 1] != '\n') continue;
        } else if (an->first_kind == 1 && s0 < len) {
            // 首字节剪枝：把 s0 推到下一个「首字节可能在集合里」的起点。
            // 每个被跳过的起点都由首字节集判定必然在第一条消费指令上失败。
            while (s0 < len && !((fb[s[s0] >> 3] >> (s[s0] & 7)) & 1)) s0++;
            if (s0 > len) break;
        }
        caps_fill(caps, ncap, -1);
        c.s = s;
        c.len = len;
        c.pos = s0;
        c.steps = 0;
        c.err = 0;
        if (run(g, 0, &c)) {
            if (mode == 2 && caps[1] != len) return 0;
            return 1;
        }
        if (c.err) pa_raise_c("RuntimeError", "regex step limit exceeded");
        if (mode != 0) return 0;
    }
    return 0;
}

typedef struct PyAdamasMatch {
    PyAdamasStr* text;
    int64_t ngroups;
    int64_t* caps;
    ReName* names;
    int nnames;
} PyAdamasMatch;

typedef struct PyAdamasRegex {
    ReProg prog;
    PyAdamasStr* pat;  // 原始模式串，供 Pattern.pattern 用
} PyAdamasRegex;

static void regex_dtor(void* p) {
    PyAdamasRegex* r = (PyAdamasRegex*)p;
    prog_free(&r->prog);
    if (r->pat) pa_rc_dec(r->pat);
}

static void match_dtor(void* p) {
    PyAdamasMatch* m = (PyAdamasMatch*)p;
    if (m->text) pa_rc_dec(m->text);
    free(m->caps);
    for (int i = 0; i < m->nnames; ++i) free(m->names[i].name);
    free(m->names);
}

static void names_copy(ReName** dst, int* dstn, const ReName* src, int n) {
    if (n <= 0) {
        *dst = NULL;
        *dstn = 0;
        return;
    }
    *dst = (ReName*)re_alloc(sizeof(ReName) * (size_t)n);
    *dstn = n;
    for (int i = 0; i < n; ++i) {
        size_t l = strlen(src[i].name);
        (*dst)[i].name = (char*)re_alloc(l + 1);
        memcpy((*dst)[i].name, src[i].name, l + 1);
        (*dst)[i].group = src[i].group;
    }
}

static PyAdamasMatch* match_new(PyAdamasStr* text, int64_t ngroups, const int64_t* caps,
                            const ReName* names, int nnames) {
    PyAdamasMatch* m = (PyAdamasMatch*)rc_alloc(sizeof(PyAdamasMatch), match_dtor, NULL, PYADAMAS_RC_OBJ);
    m->text = text;
    pa_rc_inc(text);
    m->ngroups = ngroups;
    int64_t ncap = 2 * (ngroups + 1);
    m->caps = (int64_t*)re_alloc(sizeof(int64_t) * (size_t)ncap);
    memcpy(m->caps, caps, sizeof(int64_t) * (size_t)ncap);
    names_copy(&m->names, &m->nnames, names, nnames);
    return m;
}

static PyAdamasStr* group_str(PyAdamasMatch* m, int64_t n) {
    if (!m || n < 0 || n > m->ngroups) return str_new("", 0);
    int64_t a = m->caps[2 * n];
    int64_t b = m->caps[2 * n + 1];
    if (a < 0 || b < a) return str_new("", 0);
    return str_new(m->text->data + a, b - a);
}

static int64_t group_start(PyAdamasMatch* m, int64_t n) {
    if (!m || n < 0 || n > m->ngroups) return -1;
    return m->caps[2 * n];
}

static int64_t group_end(PyAdamasMatch* m, int64_t n) {
    if (!m || n < 0 || n > m->ngroups) return -1;
    return m->caps[2 * n + 1];
}

int64_t pa_re_compile(PyAdamasStr* pat, int64_t flags) {
    PyAdamasRegex* r = (PyAdamasRegex*)rc_alloc(sizeof(PyAdamasRegex), regex_dtor, NULL, PYADAMAS_RC_OBJ);
    r->pat = NULL;
    const char* err = NULL;
    if (!prog_build(&r->prog, (const unsigned char*)pat->data, (int)pat->len, (int)flags, &err)) {
        prog_free(&r->prog);
        pa_raise_c("ValueError", err ? err : "bad pattern");
    }
    r->pat = pat;
    pa_rc_inc(pat);
    return (int64_t)r;
}

int64_t pa_re_pattern_groups(PyAdamasRegex* re) { return re ? re->prog.ngroups : 0; }

int64_t pa_re_pattern_flags(PyAdamasRegex* re) { return re ? re->prog.flags : 0; }

int64_t pa_re_pattern_str(PyAdamasRegex* re) {
    if (!re || !re->pat) return (int64_t)str_new("", 0);
    pa_rc_inc(re->pat);
    return (int64_t)re->pat;
}

int64_t pa_re_search_str(PyAdamasStr* pat, PyAdamasStr* text, int64_t flags, int64_t mode) {
    const char* err = NULL;
    int lease = -1;
    ReProg* g = re_prog_acquire((const unsigned char*)pat->data, (int)pat->len, flags, &err,
                                &lease);
    if (!g) pa_raise_c("ValueError", err ? err : "bad pattern");
    int64_t ncap = 2 * (int64_t)(g->ngroups + 1);
    int64_t stackcaps[RE_CAPS_STACK];
    int64_t* caps = caps_take(ncap, stackcaps);
    int ok = find_from(g, text, 0, (int)mode, caps);
    PyAdamasMatch* m = ok ? match_new(text, g->ngroups, caps, g->names, g->nnames) : NULL;
    caps_drop(caps, stackcaps);
    re_prog_release(g, lease);
    return (int64_t)m;
}

int64_t pa_re_search(PyAdamasRegex* re, PyAdamasStr* text, int64_t mode) {
    if (!re) pa_raise_c("AttributeError", "NoneType has no regex method");
    int64_t ncap = 2 * (int64_t)(re->prog.ngroups + 1);
    int64_t stackcaps[RE_CAPS_STACK];
    int64_t* caps = caps_take(ncap, stackcaps);
    int ok = find_from(&re->prog, text, 0, (int)mode, caps);
    PyAdamasMatch* m = ok ? match_new(text, re->prog.ngroups, caps, re->prog.names,
                                  re->prog.nnames)
                      : NULL;
    caps_drop(caps, stackcaps);
    return (int64_t)m;
}

int64_t pa_re_group(PyAdamasMatch* m, int64_t n) { return (int64_t)group_str(m, n); }

int64_t pa_re_group_str(PyAdamasMatch* m, PyAdamasStr* name) {
    if (m) {
        for (int i = 0; i < m->nnames; ++i) {
            if ((int64_t)strlen(m->names[i].name) == name->len &&
                memcmp(m->names[i].name, name->data, (size_t)name->len) == 0)
                return (int64_t)group_str(m, m->names[i].group);
        }
    }
    pa_raise_c("IndexError", "no such group");
    return (int64_t)str_new("", 0);
}

int64_t pa_re_groups(PyAdamasMatch* m) {
    PyAdamasList* out = pa_list_new(0, 1);
    /* 每个捕获组一次 str_new：out 只在本帧 C 局部变量里。 */
    pa_gc_pin(out);
    if (m) {
        for (int64_t i = 1; i <= m->ngroups; ++i) {
            PyAdamasStr* s = group_str(m, i);
            pa_list_append(out, (int64_t)s, 2);
        }
    }
    pa_gc_unpin(out);
    return (int64_t)out;
}

int64_t pa_re_groupdict(PyAdamasMatch* m) {
    // key_kind=1 是 str；key/val 都由字典拥有，析构时各自 rc_dec。
    PyAdamasDict* d = pa_dict_new(1, 1, 1, -1, -1);
    /* d 只在本帧 C 局部变量里；循环里 k 先分配、v 再分配，两个分配点都要
       保证前一个对象在根上。 */
    pa_gc_pin(d);
    if (m) {
        for (int i = 0; i < m->nnames; ++i) {
            PyAdamasStr* k = str_new(m->names[i].name, (int64_t)strlen(m->names[i].name));
            pa_gc_pin(k);
            PyAdamasStr* v = group_str(m, m->names[i].group);
            pa_dict_set(d, (int64_t)k, (int64_t)v);
            pa_gc_unpin(k);
        }
    }
    pa_gc_unpin(d);
    return (int64_t)d;
}

int64_t pa_re_lastindex(PyAdamasMatch* m) {
    if (!m) return 0;
    int64_t last = 0;
    for (int64_t i = 1; i <= m->ngroups; ++i)
        if (m->caps[2 * i] >= 0) last = i;
    return last;
}

int64_t pa_re_lastgroup(PyAdamasMatch* m) {
    int64_t idx = pa_re_lastindex(m);
    if (!m || idx <= 0) return (int64_t)str_new("", 0);
    for (int i = 0; i < m->nnames; ++i) {
        if (m->names[i].group == idx)
            return (int64_t)str_new(m->names[i].name, (int64_t)strlen(m->names[i].name));
    }
    return (int64_t)str_new("", 0);
}

int64_t pa_re_start(PyAdamasMatch* m, int64_t n) { return group_start(m, n); }
int64_t pa_re_end(PyAdamasMatch* m, int64_t n) { return group_end(m, n); }

int64_t pa_re_span(PyAdamasMatch* m, int64_t n) {
    PyAdamasList* out = pa_list_new(0, 0);
    pa_list_append(out, group_start(m, n), 2);
    pa_list_append(out, group_end(m, n), 2);
    return (int64_t)out;
}

typedef struct {
    char* p;
    size_t n, cap;
} ReBuf;

static void buf_add(ReBuf* b, const char* p, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 128;
        while (cap < b->n + n + 1) cap *= 2;
        b->p = (char*)re_realloc(b->p, cap);
        b->cap = cap;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void buf_ch(ReBuf* b, char c) { buf_add(b, &c, 1); }

static int find_next(ReProg* g, PyAdamasStr* text, int64_t from, int64_t* caps) {
    return find_from(g, text, from, 0, caps);
}

// finditer 返回 list[Match]（pyadamas 没有惰性迭代器，按列表给，语义等价）。
static PyAdamasList* finditer_impl(ReProg* g, PyAdamasStr* text) {
    PyAdamasList* out = pa_list_new(0, 1);
    /* 循环里每次 match_new 都是主收集触发点；out 只在 C 局部变量里。 */
    pa_gc_pin(out);
    int64_t ncap = 2 * (int64_t)(g->ngroups + 1);
    int64_t stackcaps[RE_CAPS_STACK];
    int64_t* caps = caps_take(ncap, stackcaps);
    int64_t pos = 0;
    while (pos <= text->len && find_next(g, text, pos, caps)) {
        pa_list_append(out, (int64_t)match_new(text, g->ngroups, caps, g->names, g->nnames), 2);
        if (caps[1] == caps[0]) pos = caps[1] + 1;
        else pos = caps[1];
    }
    pa_gc_unpin(out);
    caps_drop(caps, stackcaps);
    return out;
}

int64_t pa_re_finditer(PyAdamasRegex* re, PyAdamasStr* text) {
    if (!re) pa_raise_c("AttributeError", "NoneType has no regex method");
    return (int64_t)finditer_impl(&re->prog, text);
}

int64_t pa_re_finditer_str(PyAdamasStr* pat, PyAdamasStr* text, int64_t flags) {
    const char* err = NULL;
    int lease = -1;
    ReProg* g = re_prog_acquire((const unsigned char*)pat->data, (int)pat->len, flags, &err,
                                &lease);
    if (!g) pa_raise_c("ValueError", err ? err : "bad pattern");
    PyAdamasList* out = finditer_impl(g, text);
    re_prog_release(g, lease);
    return (int64_t)out;
}

// findall 的取段规则：没有捕获组取整段；只有一个捕获组时取该组（CPython 语义）。
static void findall_push(PyAdamasList* out, PyAdamasStr* text, int64_t ngroups, const int64_t* caps) {
    int64_t a = caps[0], b = caps[1];
    if (ngroups == 1) {
        if (caps[2] >= 0 && caps[3] >= caps[2]) {
            a = caps[2];
            b = caps[3];
        } else {
            a = b = caps[0];
        }
    }
    pa_list_append(out, (int64_t)str_new(text->data + a, b - a), 2);
}

int64_t pa_re_findall_str(PyAdamasStr* pat, PyAdamasStr* text, int64_t flags) {
    const char* err = NULL;
    int lease = -1;
    ReProg* g = re_prog_acquire((const unsigned char*)pat->data, (int)pat->len, flags, &err,
                                &lease);
    if (!g) pa_raise_c("ValueError", err ? err : "bad pattern");
    PyAdamasList* out = pa_list_new(0, 1);
    /* findall_push 内部 str_new 逐个产出匹配串；out 只在本帧 C 局部变量里。 */
    pa_gc_pin(out);
    int64_t ncap = 2 * (int64_t)(g->ngroups + 1);
    int64_t stackcaps[RE_CAPS_STACK];
    int64_t* caps = caps_take(ncap, stackcaps);
    int64_t pos = 0;
    while (pos <= text->len && find_next(g, text, pos, caps)) {
        findall_push(out, text, g->ngroups, caps);
        if (caps[1] == caps[0]) pos = caps[1] + 1;
        else pos = caps[1];
    }
    pa_gc_unpin(out);
    caps_drop(caps, stackcaps);
    re_prog_release(g, lease);
    return (int64_t)out;
}

int64_t pa_re_findall(PyAdamasRegex* re, PyAdamasStr* text) {
    if (!re) pa_raise_c("AttributeError", "NoneType has no regex method");
    PyAdamasList* out = pa_list_new(0, 1);
    pa_gc_pin(out);  /* 同 pa_re_findall_str：循环内的分配点会清扫无根的 out */
    int64_t ncap = 2 * (int64_t)(re->prog.ngroups + 1);
    int64_t stackcaps[RE_CAPS_STACK];
    int64_t* caps = caps_take(ncap, stackcaps);
    int64_t pos = 0;
    while (pos <= text->len && find_next(&re->prog, text, pos, caps)) {
        findall_push(out, text, re->prog.ngroups, caps);
        if (caps[1] == caps[0]) pos = caps[1] + 1;
        else pos = caps[1];
    }
    pa_gc_unpin(out);
    caps_drop(caps, stackcaps);
    return (int64_t)out;
}

static int64_t group_index_by_name(PyAdamasMatch* m, const char* s, int64_t len) {
    if (!m) return -1;
    for (int i = 0; i < m->nnames; ++i) {
        if ((int64_t)strlen(m->names[i].name) == len &&
            memcmp(m->names[i].name, s, (size_t)len) == 0)
            return m->names[i].group;
    }
    return -1;
}

static void add_repl(ReBuf* b, const char* r, int64_t rlen, PyAdamasMatch* m) {
    for (int64_t i = 0; i < rlen; ++i) {
        char c = r[i];
        if (c == '\\' && i + 1 < rlen) {
            char d = r[i + 1];
            if (d >= '0' && d <= '9') {
                PyAdamasStr* s = group_str(m, d - '0');
                buf_add(b, s->data, (size_t)s->len);
                pa_rc_dec(s);
                i++;
                continue;
            }
            // \g<name> / \g<1> / \g<0>
            if (d == 'g' && i + 2 < rlen && r[i + 2] == '<') {
                int64_t j = i + 3, ns = i + 3;
                while (j < rlen && r[j] != '>') j++;
                if (j < rlen && j > ns) {
                    int digits = 1;
                    for (int64_t k = ns; k < j; ++k)
                        if (r[k] < '0' || r[k] > '9') {
                            digits = 0;
                            break;
                        }
                    int64_t gi = -1;
                    if (digits) {
                        for (int64_t k = ns; k < j; ++k) {
                            if (gi < 0) gi = 0;
                            gi = gi * 10 + (r[k] - '0');
                        }
                    } else {
                        gi = group_index_by_name(m, r + ns, j - ns);
                    }
                    if (gi >= 0) {
                        PyAdamasStr* s = group_str(m, gi);
                        buf_add(b, s->data, (size_t)s->len);
                        pa_rc_dec(s);
                        i = j;
                        continue;
                    }
                }
            }
            if (d == '\\') {
                buf_ch(b, '\\');
                i++;
                continue;
            }
        }
        buf_ch(b, c);
    }
}

int64_t pa_re_expand(PyAdamasMatch* m, PyAdamasStr* tmpl) {
    ReBuf b = {NULL, 0, 0};
    add_repl(&b, tmpl->data, tmpl->len, m);
    PyAdamasStr* out = str_new(b.p ? b.p : "", (int64_t)b.n);
    free(b.p);
    return (int64_t)out;
}

int64_t pa_re_sub_str(PyAdamasStr* pat, PyAdamasStr* repl, PyAdamasStr* text, int64_t count,
                        int64_t flags) {
    const char* err = NULL;
    int lease = -1;
    ReProg* g = re_prog_acquire((const unsigned char*)pat->data, (int)pat->len, flags, &err,
                                &lease);
    if (!g) pa_raise_c("ValueError", err ? err : "bad pattern");
    int64_t ncap = 2 * (int64_t)(g->ngroups + 1);
    int64_t stackcaps[RE_CAPS_STACK];
    int64_t* caps = caps_take(ncap, stackcaps);
    ReBuf b = {NULL, 0, 0};
    int64_t pos = 0, done = 0;
    while (pos <= text->len && (count <= 0 || done < count) && find_next(g, text, pos, caps)) {
        buf_add(&b, text->data + pos, (size_t)(caps[0] - pos));
        PyAdamasMatch tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.text = text;
        tmp.ngroups = g->ngroups;
        tmp.caps = caps;
        tmp.names = g->names;
        tmp.nnames = g->nnames;
        add_repl(&b, repl->data, repl->len, &tmp);
        done++;
        if (caps[1] == caps[0]) {
            if (caps[1] < text->len) buf_ch(&b, text->data[caps[1]]);
            pos = caps[1] + 1;
        } else {
            pos = caps[1];
        }
    }
    if (pos < text->len) buf_add(&b, text->data + pos, (size_t)(text->len - pos));
    PyAdamasStr* out = str_new(b.p ? b.p : "", (int64_t)b.n);
    free(b.p);
    caps_drop(caps, stackcaps);
    re_prog_release(g, lease);
    return (int64_t)out;
}

int64_t pa_re_sub(PyAdamasRegex* re, PyAdamasStr* repl, PyAdamasStr* text, int64_t count) {
    if (!re) pa_raise_c("AttributeError", "NoneType has no regex method");
    int64_t ncap = 2 * (int64_t)(re->prog.ngroups + 1);
    int64_t stackcaps[RE_CAPS_STACK];
    int64_t* caps = caps_take(ncap, stackcaps);
    ReBuf b = {NULL, 0, 0};
    int64_t pos = 0, done = 0;
    while (pos <= text->len && (count <= 0 || done < count) &&
           find_next(&re->prog, text, pos, caps)) {
        buf_add(&b, text->data + pos, (size_t)(caps[0] - pos));
        PyAdamasMatch tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.text = text;
        tmp.ngroups = re->prog.ngroups;
        tmp.caps = caps;
        tmp.names = re->prog.names;
        tmp.nnames = re->prog.nnames;
        add_repl(&b, repl->data, repl->len, &tmp);
        done++;
        if (caps[1] == caps[0]) {
            if (caps[1] < text->len) buf_ch(&b, text->data[caps[1]]);
            pos = caps[1] + 1;
        } else {
            pos = caps[1];
        }
    }
    if (pos < text->len) buf_add(&b, text->data + pos, (size_t)(text->len - pos));
    PyAdamasStr* out = str_new(b.p ? b.p : "", (int64_t)b.n);
    free(b.p);
    caps_drop(caps, stackcaps);
    return (int64_t)out;
}

// sub 的 repl 是函数：每匹配一次就调一次闭包，返回值拼进结果。
int64_t pa_re_sub_fn(PyAdamasRegex* re, int64_t closure, PyAdamasStr* text, int64_t count) {
    if (!re) pa_raise_c("AttributeError", "NoneType has no regex method");
    ReProg* g = &re->prog;
    int64_t ncap = 2 * (int64_t)(g->ngroups + 1);
    int64_t stackcaps[RE_CAPS_STACK];
    int64_t* caps = caps_take(ncap, stackcaps);
    ReBuf b = {NULL, 0, 0};
    int64_t pos = 0, done = 0;
    while (pos <= text->len && (count <= 0 || done < count) && find_next(g, text, pos, caps)) {
        buf_add(&b, text->data + pos, (size_t)(caps[0] - pos));
        PyAdamasMatch* m = match_new(text, g->ngroups, caps, g->names, g->nnames);
        /* 回调是用户代码，内部必然大量分配：m 只在本帧 C 局部变量里，
           不 pin 的话主收集会把它清扫，回调返回后再 dec 就是 UAF。 */
        pa_gc_pin(m);
        PyAdamasStr* r = (PyAdamasStr*)pa_call1((void*)closure, (int64_t)(uintptr_t)m);
        pa_gc_unpin(m);
        if (r) {
            buf_add(&b, r->data, (size_t)r->len);
            pa_rc_dec(r);
        }
        pa_rc_dec(m);
        done++;
        if (caps[1] == caps[0]) {
            if (caps[1] < text->len) buf_ch(&b, text->data[caps[1]]);
            pos = caps[1] + 1;
        } else {
            pos = caps[1];
        }
    }
    if (pos < text->len) buf_add(&b, text->data + pos, (size_t)(text->len - pos));
    PyAdamasStr* out = str_new(b.p ? b.p : "", (int64_t)b.n);
    free(b.p);
    caps_drop(caps, stackcaps);
    return (int64_t)out;
}

int64_t pa_re_split_str(PyAdamasStr* pat, PyAdamasStr* text, int64_t maxsplit, int64_t flags) {
    const char* err = NULL;
    int lease = -1;
    ReProg* g = re_prog_acquire((const unsigned char*)pat->data, (int)pat->len, flags, &err,
                                &lease);
    if (!g) pa_raise_c("ValueError", err ? err : "bad pattern");
    int64_t ncap = 2 * (int64_t)(g->ngroups + 1);
    int64_t stackcaps[RE_CAPS_STACK];
    int64_t* caps = caps_take(ncap, stackcaps);
    PyAdamasList* out = pa_list_new(0, 1);
    /* 每个分段/捕获组一次 str_new；out 只在本帧 C 局部变量里。 */
    pa_gc_pin(out);
    int64_t pos = 0, last = 0, done = 0;
    while (pos <= text->len && (maxsplit <= 0 || done < maxsplit) &&
           find_next(g, text, pos, caps)) {
        PyAdamasStr* s = str_new(text->data + last, caps[0] - last);
        pa_list_append(out, (int64_t)s, 2);
        for (int64_t gi = 1; gi <= g->ngroups; ++gi) {
            int64_t a = caps[2 * gi], b = caps[2 * gi + 1];
            pa_list_append(out, (int64_t)(a >= 0 && b >= a
                                                ? str_new(text->data + a, b - a)
                                                : str_new("", 0)), 2);
        }
        done++;
        last = caps[1];
        if (caps[1] == caps[0]) {
            pos = caps[1] + 1;
        } else {
            pos = caps[1];
        }
    }
    PyAdamasStr* tail = str_new(text->data + last, text->len - last);
    pa_list_append(out, (int64_t)tail, 2);
    pa_gc_unpin(out);
    caps_drop(caps, stackcaps);
    re_prog_release(g, lease);
    return (int64_t)out;
}

int64_t pa_re_split(PyAdamasRegex* re, PyAdamasStr* text, int64_t maxsplit) {
    if (!re) pa_raise_c("AttributeError", "NoneType has no regex method");
    int64_t ncap = 2 * (int64_t)(re->prog.ngroups + 1);
    int64_t stackcaps[RE_CAPS_STACK];
    int64_t* caps = caps_take(ncap, stackcaps);
    PyAdamasList* out = pa_list_new(0, 1);
    pa_gc_pin(out);  /* 同 pa_re_split_str：循环里的 str_new 会清扫无根的 out */
    int64_t pos = 0, last = 0, done = 0;
    while (pos <= text->len && (maxsplit <= 0 || done < maxsplit) &&
           find_next(&re->prog, text, pos, caps)) {
        PyAdamasStr* s = str_new(text->data + last, caps[0] - last);
        pa_list_append(out, (int64_t)s, 2);
        for (int64_t gi = 1; gi <= re->prog.ngroups; ++gi) {
            int64_t a = caps[2 * gi], b = caps[2 * gi + 1];
            pa_list_append(out, (int64_t)(a >= 0 && b >= a
                                                ? str_new(text->data + a, b - a)
                                                : str_new("", 0)), 2);
        }
        done++;
        last = caps[1];
        if (caps[1] == caps[0]) {
            pos = caps[1] + 1;
        } else {
            pos = caps[1];
        }
    }
    PyAdamasStr* tail = str_new(text->data + last, text->len - last);
    pa_list_append(out, (int64_t)tail, 2);
    pa_gc_unpin(out);
    caps_drop(caps, stackcaps);
    return (int64_t)out;
}

int64_t pa_re_escape(PyAdamasStr* s) {
    static const char specials[] = "\\^$.*+?{}[]|()-#&~ \t\n\r\v\f";
    ReBuf b = {NULL, 0, 0};
    for (int64_t i = 0; i < s->len; ++i) {
        char c = s->data[i];
        if (c && strchr(specials, c)) buf_ch(&b, '\\');
        buf_ch(&b, c);
    }
    PyAdamasStr* out = str_new(b.p ? b.p : "", (int64_t)b.n);
    free(b.p);
    return (int64_t)out;
}
