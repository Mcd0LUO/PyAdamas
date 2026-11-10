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
// 字符串按字节匹配（与 py23 的 str 语义一致），不做 Unicode 属性类。
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

typedef struct {
    ReInst* code;
    int n, cap;
    int ngroups;
    int flags;
    ReName* names;
    int nnames;
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
        fputs("py23: panic: out of memory\n", stderr);
        exit(1);
    }
    return p;
}

static void* re_realloc(void* p, size_t n) {
    void* q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("py23: panic: out of memory\n", stderr);
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

static int is_word(int c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           c == '_';
}

static int fold_eq(int a, int b) {
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    return a == b;
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

// 从 start 起尝试一次匹配。mode: 0 search / 1 match / 2 fullmatch。
static int find_from(ReProg* g, Py23Str* text, int64_t start, int mode, int64_t* caps) {
    int64_t ncap = 2 * (int64_t)(g->ngroups + 1);
    for (int64_t s = start; s <= text->len; ++s) {
        for (int64_t i = 0; i < ncap; ++i) caps[i] = -1;
        ReCtx c = {(const unsigned char*)text->data, text->len, s, g->flags, caps, ncap, 0, 0};
        if (run(g, 0, &c)) {
            if (mode == 2 && caps[1] != text->len) return 0;
            return 1;
        }
        if (c.err) {
            py23_raise_c("RuntimeError", "regex step limit exceeded");
        }
        if (mode != 0) return 0;
    }
    return 0;
}

typedef struct Py23Match {
    Py23Str* text;
    int64_t ngroups;
    int64_t* caps;
    ReName* names;
    int nnames;
} Py23Match;

typedef struct Py23Regex {
    ReProg prog;
    Py23Str* pat;  // 原始模式串，供 Pattern.pattern 用
} Py23Regex;

static void regex_dtor(void* p) {
    Py23Regex* r = (Py23Regex*)p;
    prog_free(&r->prog);
    if (r->pat) py23_rc_dec(r->pat);
}

static void match_dtor(void* p) {
    Py23Match* m = (Py23Match*)p;
    if (m->text) py23_rc_dec(m->text);
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

static Py23Match* match_new(Py23Str* text, int64_t ngroups, const int64_t* caps,
                            const ReName* names, int nnames) {
    Py23Match* m = (Py23Match*)rc_alloc(sizeof(Py23Match), match_dtor, NULL, PY23_RC_OBJ);
    m->text = text;
    py23_rc_inc(text);
    m->ngroups = ngroups;
    int64_t ncap = 2 * (ngroups + 1);
    m->caps = (int64_t*)re_alloc(sizeof(int64_t) * (size_t)ncap);
    memcpy(m->caps, caps, sizeof(int64_t) * (size_t)ncap);
    names_copy(&m->names, &m->nnames, names, nnames);
    return m;
}

static Py23Str* group_str(Py23Match* m, int64_t n) {
    if (!m || n < 0 || n > m->ngroups) return str_new("", 0);
    int64_t a = m->caps[2 * n];
    int64_t b = m->caps[2 * n + 1];
    if (a < 0 || b < a) return str_new("", 0);
    return str_new(m->text->data + a, b - a);
}

static int64_t group_start(Py23Match* m, int64_t n) {
    if (!m || n < 0 || n > m->ngroups) return -1;
    return m->caps[2 * n];
}

static int64_t group_end(Py23Match* m, int64_t n) {
    if (!m || n < 0 || n > m->ngroups) return -1;
    return m->caps[2 * n + 1];
}

int64_t py23_re_compile(Py23Str* pat, int64_t flags) {
    Py23Regex* r = (Py23Regex*)rc_alloc(sizeof(Py23Regex), regex_dtor, NULL, PY23_RC_OBJ);
    r->pat = NULL;
    const char* err = NULL;
    if (!prog_build(&r->prog, (const unsigned char*)pat->data, (int)pat->len, (int)flags, &err)) {
        prog_free(&r->prog);
        py23_raise_c("ValueError", err ? err : "bad pattern");
    }
    r->pat = pat;
    py23_rc_inc(pat);
    return (int64_t)r;
}

int64_t py23_re_pattern_groups(Py23Regex* re) { return re ? re->prog.ngroups : 0; }

int64_t py23_re_pattern_flags(Py23Regex* re) { return re ? re->prog.flags : 0; }

int64_t py23_re_pattern_str(Py23Regex* re) {
    if (!re || !re->pat) return (int64_t)str_new("", 0);
    py23_rc_inc(re->pat);
    return (int64_t)re->pat;
}

int64_t py23_re_search_str(Py23Str* pat, Py23Str* text, int64_t flags, int64_t mode) {
    ReProg g;
    const char* err = NULL;
    if (!prog_build(&g, (const unsigned char*)pat->data, (int)pat->len, (int)flags, &err)) {
        py23_raise_c("ValueError", err ? err : "bad pattern");
    }
    int64_t ncap = 2 * (int64_t)(g.ngroups + 1);
    int64_t* caps = (int64_t*)re_alloc(sizeof(int64_t) * (size_t)ncap);
    int ok = find_from(&g, text, 0, (int)mode, caps);
    Py23Match* m = ok ? match_new(text, g.ngroups, caps, g.names, g.nnames) : NULL;
    free(caps);
    prog_free(&g);
    return (int64_t)m;
}

int64_t py23_re_search(Py23Regex* re, Py23Str* text, int64_t mode) {
    if (!re) py23_raise_c("AttributeError", "NoneType has no regex method");
    int64_t ncap = 2 * (int64_t)(re->prog.ngroups + 1);
    int64_t* caps = (int64_t*)re_alloc(sizeof(int64_t) * (size_t)ncap);
    int ok = find_from(&re->prog, text, 0, (int)mode, caps);
    Py23Match* m = ok ? match_new(text, re->prog.ngroups, caps, re->prog.names,
                                  re->prog.nnames)
                      : NULL;
    free(caps);
    return (int64_t)m;
}

int64_t py23_re_group(Py23Match* m, int64_t n) { return (int64_t)group_str(m, n); }

int64_t py23_re_group_str(Py23Match* m, Py23Str* name) {
    if (m) {
        for (int i = 0; i < m->nnames; ++i) {
            if ((int64_t)strlen(m->names[i].name) == name->len &&
                memcmp(m->names[i].name, name->data, (size_t)name->len) == 0)
                return (int64_t)group_str(m, m->names[i].group);
        }
    }
    py23_raise_c("IndexError", "no such group");
    return (int64_t)str_new("", 0);
}

int64_t py23_re_groups(Py23Match* m) {
    Py23List* out = py23_list_new(0, 1);
    if (m) {
        for (int64_t i = 1; i <= m->ngroups; ++i) {
            Py23Str* s = group_str(m, i);
            py23_list_append(out, (int64_t)s);
        }
    }
    return (int64_t)out;
}

int64_t py23_re_start(Py23Match* m, int64_t n) { return group_start(m, n); }
int64_t py23_re_end(Py23Match* m, int64_t n) { return group_end(m, n); }

int64_t py23_re_span(Py23Match* m, int64_t n) {
    Py23List* out = py23_list_new(0, 0);
    py23_list_append(out, group_start(m, n));
    py23_list_append(out, group_end(m, n));
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

static int find_next(ReProg* g, Py23Str* text, int64_t from, int64_t* caps) {
    return find_from(g, text, from, 0, caps);
}

// finditer 返回 list[Match]（py23 没有惰性迭代器，按列表给，语义等价）。
static Py23List* finditer_impl(ReProg* g, Py23Str* text) {
    Py23List* out = py23_list_new(0, 1);
    int64_t ncap = 2 * (int64_t)(g->ngroups + 1);
    int64_t* caps = (int64_t*)re_alloc(sizeof(int64_t) * (size_t)ncap);
    int64_t pos = 0;
    while (pos <= text->len && find_next(g, text, pos, caps)) {
        py23_list_append(out,
                         (int64_t)match_new(text, g->ngroups, caps, g->names, g->nnames));
        if (caps[1] == caps[0]) pos = caps[1] + 1;
        else pos = caps[1];
    }
    free(caps);
    return out;
}

int64_t py23_re_finditer(Py23Regex* re, Py23Str* text) {
    if (!re) py23_raise_c("AttributeError", "NoneType has no regex method");
    return (int64_t)finditer_impl(&re->prog, text);
}

int64_t py23_re_finditer_str(Py23Str* pat, Py23Str* text, int64_t flags) {
    ReProg g;
    const char* err = NULL;
    if (!prog_build(&g, (const unsigned char*)pat->data, (int)pat->len, (int)flags, &err)) {
        py23_raise_c("ValueError", err ? err : "bad pattern");
    }
    Py23List* out = finditer_impl(&g, text);
    prog_free(&g);
    return (int64_t)out;
}

// findall 的取段规则：没有捕获组取整段；只有一个捕获组时取该组（CPython 语义）。
static void findall_push(Py23List* out, Py23Str* text, int64_t ngroups, const int64_t* caps) {
    int64_t a = caps[0], b = caps[1];
    if (ngroups == 1) {
        if (caps[2] >= 0 && caps[3] >= caps[2]) {
            a = caps[2];
            b = caps[3];
        } else {
            a = b = caps[0];
        }
    }
    py23_list_append(out, (int64_t)str_new(text->data + a, b - a));
}

int64_t py23_re_findall_str(Py23Str* pat, Py23Str* text, int64_t flags) {
    ReProg g;
    const char* err = NULL;
    if (!prog_build(&g, (const unsigned char*)pat->data, (int)pat->len, (int)flags, &err)) {
        py23_raise_c("ValueError", err ? err : "bad pattern");
    }
    Py23List* out = py23_list_new(0, 1);
    int64_t ncap = 2 * (int64_t)(g.ngroups + 1);
    int64_t* caps = (int64_t*)re_alloc(sizeof(int64_t) * (size_t)ncap);
    int64_t pos = 0;
    while (pos <= text->len && find_next(&g, text, pos, caps)) {
        findall_push(out, text, g.ngroups, caps);
        if (caps[1] == caps[0]) pos = caps[1] + 1;
        else pos = caps[1];
    }
    free(caps);
    prog_free(&g);
    return (int64_t)out;
}

int64_t py23_re_findall(Py23Regex* re, Py23Str* text) {
    if (!re) py23_raise_c("AttributeError", "NoneType has no regex method");
    Py23List* out = py23_list_new(0, 1);
    int64_t ncap = 2 * (int64_t)(re->prog.ngroups + 1);
    int64_t* caps = (int64_t*)re_alloc(sizeof(int64_t) * (size_t)ncap);
    int64_t pos = 0;
    while (pos <= text->len && find_next(&re->prog, text, pos, caps)) {
        findall_push(out, text, re->prog.ngroups, caps);
        if (caps[1] == caps[0]) pos = caps[1] + 1;
        else pos = caps[1];
    }
    free(caps);
    return (int64_t)out;
}

static int64_t group_index_by_name(Py23Match* m, const char* s, int64_t len) {
    if (!m) return -1;
    for (int i = 0; i < m->nnames; ++i) {
        if ((int64_t)strlen(m->names[i].name) == len &&
            memcmp(m->names[i].name, s, (size_t)len) == 0)
            return m->names[i].group;
    }
    return -1;
}

static void add_repl(ReBuf* b, const char* r, int64_t rlen, Py23Match* m) {
    for (int64_t i = 0; i < rlen; ++i) {
        char c = r[i];
        if (c == '\\' && i + 1 < rlen) {
            char d = r[i + 1];
            if (d >= '0' && d <= '9') {
                Py23Str* s = group_str(m, d - '0');
                buf_add(b, s->data, (size_t)s->len);
                py23_rc_dec(s);
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
                        Py23Str* s = group_str(m, gi);
                        buf_add(b, s->data, (size_t)s->len);
                        py23_rc_dec(s);
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

int64_t py23_re_expand(Py23Match* m, Py23Str* tmpl) {
    ReBuf b = {NULL, 0, 0};
    add_repl(&b, tmpl->data, tmpl->len, m);
    Py23Str* out = str_new(b.p ? b.p : "", (int64_t)b.n);
    free(b.p);
    return (int64_t)out;
}

int64_t py23_re_sub_str(Py23Str* pat, Py23Str* repl, Py23Str* text, int64_t count,
                        int64_t flags) {
    ReProg g;
    const char* err = NULL;
    if (!prog_build(&g, (const unsigned char*)pat->data, (int)pat->len, (int)flags, &err)) {
        py23_raise_c("ValueError", err ? err : "bad pattern");
    }
    int64_t ncap = 2 * (int64_t)(g.ngroups + 1);
    int64_t* caps = (int64_t*)re_alloc(sizeof(int64_t) * (size_t)ncap);
    ReBuf b = {NULL, 0, 0};
    int64_t pos = 0, done = 0;
    while (pos <= text->len && (count <= 0 || done < count) && find_next(&g, text, pos, caps)) {
        buf_add(&b, text->data + pos, (size_t)(caps[0] - pos));
        Py23Match tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.text = text;
        tmp.ngroups = g.ngroups;
        tmp.caps = caps;
        tmp.names = g.names;
        tmp.nnames = g.nnames;
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
    Py23Str* out = str_new(b.p ? b.p : "", (int64_t)b.n);
    free(b.p);
    free(caps);
    prog_free(&g);
    return (int64_t)out;
}

int64_t py23_re_sub(Py23Regex* re, Py23Str* repl, Py23Str* text, int64_t count) {
    if (!re) py23_raise_c("AttributeError", "NoneType has no regex method");
    int64_t ncap = 2 * (int64_t)(re->prog.ngroups + 1);
    int64_t* caps = (int64_t*)re_alloc(sizeof(int64_t) * (size_t)ncap);
    ReBuf b = {NULL, 0, 0};
    int64_t pos = 0, done = 0;
    while (pos <= text->len && (count <= 0 || done < count) &&
           find_next(&re->prog, text, pos, caps)) {
        buf_add(&b, text->data + pos, (size_t)(caps[0] - pos));
        Py23Match tmp;
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
    Py23Str* out = str_new(b.p ? b.p : "", (int64_t)b.n);
    free(b.p);
    free(caps);
    return (int64_t)out;
}

int64_t py23_re_split_str(Py23Str* pat, Py23Str* text, int64_t maxsplit, int64_t flags) {
    ReProg g;
    const char* err = NULL;
    if (!prog_build(&g, (const unsigned char*)pat->data, (int)pat->len, (int)flags, &err)) {
        py23_raise_c("ValueError", err ? err : "bad pattern");
    }
    int64_t ncap = 2 * (int64_t)(g.ngroups + 1);
    int64_t* caps = (int64_t*)re_alloc(sizeof(int64_t) * (size_t)ncap);
    Py23List* out = py23_list_new(0, 1);
    int64_t pos = 0, last = 0, done = 0;
    while (pos <= text->len && (maxsplit <= 0 || done < maxsplit) &&
           find_next(&g, text, pos, caps)) {
        Py23Str* s = str_new(text->data + last, caps[0] - last);
        py23_list_append(out, (int64_t)s);
        for (int64_t gi = 1; gi <= g.ngroups; ++gi) {
            int64_t a = caps[2 * gi], b = caps[2 * gi + 1];
            py23_list_append(out, (int64_t)(a >= 0 && b >= a
                                                ? str_new(text->data + a, b - a)
                                                : str_new("", 0)));
        }
        done++;
        last = caps[1];
        if (caps[1] == caps[0]) {
            pos = caps[1] + 1;
        } else {
            pos = caps[1];
        }
    }
    Py23Str* tail = str_new(text->data + last, text->len - last);
    py23_list_append(out, (int64_t)tail);
    free(caps);
    prog_free(&g);
    return (int64_t)out;
}

int64_t py23_re_split(Py23Regex* re, Py23Str* text, int64_t maxsplit) {
    if (!re) py23_raise_c("AttributeError", "NoneType has no regex method");
    int64_t ncap = 2 * (int64_t)(re->prog.ngroups + 1);
    int64_t* caps = (int64_t*)re_alloc(sizeof(int64_t) * (size_t)ncap);
    Py23List* out = py23_list_new(0, 1);
    int64_t pos = 0, last = 0, done = 0;
    while (pos <= text->len && (maxsplit <= 0 || done < maxsplit) &&
           find_next(&re->prog, text, pos, caps)) {
        Py23Str* s = str_new(text->data + last, caps[0] - last);
        py23_list_append(out, (int64_t)s);
        for (int64_t gi = 1; gi <= re->prog.ngroups; ++gi) {
            int64_t a = caps[2 * gi], b = caps[2 * gi + 1];
            py23_list_append(out, (int64_t)(a >= 0 && b >= a
                                                ? str_new(text->data + a, b - a)
                                                : str_new("", 0)));
        }
        done++;
        last = caps[1];
        if (caps[1] == caps[0]) {
            pos = caps[1] + 1;
        } else {
            pos = caps[1];
        }
    }
    Py23Str* tail = str_new(text->data + last, text->len - last);
    py23_list_append(out, (int64_t)tail);
    free(caps);
    return (int64_t)out;
}

int64_t py23_re_escape(Py23Str* s) {
    static const char specials[] = "\\^$.*+?{}[]|()-#&~ \t\n\r\v\f";
    ReBuf b = {NULL, 0, 0};
    for (int64_t i = 0; i < s->len; ++i) {
        char c = s->data[i];
        if (c && strchr(specials, c)) buf_ch(&b, '\\');
        buf_ch(&b, c);
    }
    Py23Str* out = str_new(b.p ? b.p : "", (int64_t)b.n);
    free(b.p);
    return (int64_t)out;
}
