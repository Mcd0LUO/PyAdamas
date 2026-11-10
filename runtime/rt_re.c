// rt_re.c —— re 模块：回溯 VM、Pattern / Match 对象
//
// 语法是 Python re 的子集：字面量、.、字符类、\d\w\s\D\W\S、\b、
// ^ $、分组（含 (?:...) 与 (?P<name>...)）、交替、* + ? {m,n}（含惰性）。
// v1 不支持 lookaround、反向引用、条件组；字符串按字节匹配（与 py23 的
// str 语义一致）。
#include "rt_internal.h"

#define RE_I 2
#define RE_M 8
#define RE_S 16
#define RE_STEP_LIMIT 50000000LL

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
};

typedef struct {
    int op;
    int x, y;
    int ch;
    unsigned char cls[32];
} ReInst;

typedef struct {
    ReInst* code;
    int n, cap;
    int ngroups;
    int flags;
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
};

typedef struct ReNode {
    int kind;
    int ch;
    int min, max;
    int greedy;
    int group;
    int negate;
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

static int peek(ReParser* P) { return P->pos < P->len ? P->p[P->pos] : -1; }

static void perr(ReParser* P, const char* msg) {
    if (!P->err) {
        P->err = 1;
        P->msg = msg;
    }
}

static ReNode* parse_alt(ReParser* P);

static int hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 解析反斜杠后的一个转义；返回字符，或 -1 表示已经写进 cls（字符类）。
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
            return -2;  // word boundary marker
        case 'B':
            if (in_class) return 'B';
            return -3;  // non-boundary marker
        case 'A':
            if (in_class) return 'A';
            return -4;  // start anchor
        case 'Z':
            if (in_class) return 'Z';
            return -5;  // end anchor
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
        default:
            if (c >= '1' && c <= '9') {
                perr(P, "backreferences are not supported yet");
                return c;
            }
            return c;
    }
}

static ReNode* parse_class(ReParser* P) {
    ReNode* n = node_new(N_CLASS);
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

static ReNode* parse_atom(ReParser* P) {
    int c = peek(P);
    if (c < 0) return node_new(N_EMPTY);
    if (c == '(') {
        P->pos++;
        int group = -1;
        if (peek(P) == '?') {
            P->pos++;
            if (peek(P) == ':') {
                P->pos++;
            } else if (peek(P) == 'P' && P->pos + 1 < P->len && P->p[P->pos + 1] == '<') {
                P->pos += 2;
                while (P->pos < P->len && P->p[P->pos] != '>') P->pos++;
                if (P->pos >= P->len) {
                    perr(P, "unterminated group name");
                    return node_new(N_EMPTY);
                }
                P->pos++;
                group = ++P->ngroups;
            } else {
                perr(P, "lookaround / inline flags are not supported yet");
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
        return node_new(N_ANY);
    }
    if (c == '^') {
        P->pos++;
        return node_new(N_BOL);
    }
    if (c == '$') {
        P->pos++;
        return node_new(N_EOL);
    }
    if (c == '\\') {
        P->pos++;
        unsigned char tmp[32] = {0};
        int e = parse_escape(P, tmp, 0);
        if (e == -1) {
            ReNode* n = node_new(N_CLASS);
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
        if (e == -4) return node_new(N_BOL);
        if (e == -5) return node_new(N_EOL);
        ReNode* n = node_new(N_CHAR);
        n->ch = e;
        return n;
    }
    P->pos++;
    ReNode* n = node_new(N_CHAR);
    n->ch = c;
    return n;
}

static ReNode* parse_term(ReParser* P) {
    ReNode* atom = parse_atom(P);
    int c = peek(P);
    if (c == '*' || c == '+' || c == '?') {
        P->pos++;
        int lazy = 0;
        if (peek(P) == '?') {
            P->pos++;
            lazy = 1;
        }
        ReNode* n = node_new(c == '*' ? N_STAR : c == '+' ? N_PLUS : N_OPT);
        n->a = atom;
        n->greedy = !lazy;
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
        int lazy = 0;
        if (peek(P) == '?') {
            P->pos++;
            lazy = 1;
        }
        ReNode* r = node_new(N_REPEAT);
        r->a = atom;
        r->min = m;
        r->max = n;
        r->greedy = !lazy;
        return r;
    }
    return atom;
}

static ReNode* parse_seq(ReParser* P) {
    ReNode* head = NULL;
    ReNode** tail = &head;
    while (P->pos < P->len && peek(P) != '|' && peek(P) != ')') {
        ReNode* t = parse_term(P);
        if (!t) break;
        *tail = t;
        tail = &t->next;
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
                break;
            }
            case N_ANY: emit(g, RE_ANY); break;
            case N_CLASS: {
                int i = emit(g, RE_CLASS);
                memcpy(g->code[i].cls, n->cls, sizeof g->code[i].cls);
                break;
            }
            case N_BOL: emit(g, RE_BOL); break;
            case N_EOL: emit(g, RE_EOL); break;
            case N_WORDB: {
                int i = emit(g, RE_WORDB);
                g->code[i].x = n->negate;
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
}

static int prog_build(ReProg* g, const unsigned char* pat, int len, int flags, const char** err) {
    memset(g, 0, sizeof(*g));
    g->flags = flags;
    ReParser P = {pat, len, 0, flags, 0, 0, NULL};
    ReNode* root = parse_alt(&P);
    if (P.err) {
        if (err) *err = P.msg;
        node_free(root);
        return 0;
    }
    if (P.pos != P.len) {
        if (err) *err = "unexpected ')'";
        node_free(root);
        return 0;
    }
    g->ngroups = P.ngroups;
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
                if (g->flags & RE_I) {
                    if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
                    int want = in->ch;
                    if (want >= 'A' && want <= 'Z') want = want - 'A' + 'a';
                    if (ch != want) return 0;
                } else if (ch != in->ch) {
                    return 0;
                }
                c->pos++;
                pc++;
                break;
            }
            case RE_ANY:
                if (c->pos >= c->len) return 0;
                if (!(g->flags & RE_S) && c->s[c->pos] == '\n') return 0;
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
                if (!(c->pos == 0 || ((g->flags & RE_M) && c->s[c->pos - 1] == '\n')))
                    return 0;
                pc++;
                break;
            case RE_EOL:
                if (!(c->pos == c->len || ((g->flags & RE_M) && c->s[c->pos] == '\n')))
                    return 0;
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
            if (mode == 2 && caps[1] != text->len) {
                if (mode != 0) return 0;
                continue;
            }
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
} Py23Match;

typedef struct Py23Regex {
    ReProg prog;
} Py23Regex;

static void regex_dtor(void* p) {
    Py23Regex* r = (Py23Regex*)p;
    prog_free(&r->prog);
}

static void match_dtor(void* p) {
    Py23Match* m = (Py23Match*)p;
    if (m->text) py23_rc_dec(m->text);
    free(m->caps);
}

static Py23Match* match_new(Py23Str* text, int64_t ngroups, const int64_t* caps) {
    Py23Match* m = (Py23Match*)rc_alloc(sizeof(Py23Match), match_dtor, NULL, PY23_RC_OBJ);
    m->text = text;
    py23_rc_inc(text);
    m->ngroups = ngroups;
    int64_t ncap = 2 * (ngroups + 1);
    m->caps = (int64_t*)re_alloc(sizeof(int64_t) * (size_t)ncap);
    memcpy(m->caps, caps, sizeof(int64_t) * (size_t)ncap);
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
    const char* err = NULL;
    if (!prog_build(&r->prog, (const unsigned char*)pat->data, (int)pat->len, (int)flags, &err)) {
        prog_free(&r->prog);
        py23_raise_c("ValueError", err ? err : "bad pattern");
    }
    return (int64_t)r;
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
    Py23Match* m = ok ? match_new(text, g.ngroups, caps) : NULL;
    free(caps);
    prog_free(&g);
    return (int64_t)m;
}

int64_t py23_re_search(Py23Regex* re, Py23Str* text, int64_t mode) {
    if (!re) py23_raise_c("AttributeError", "NoneType has no regex method");
    int64_t ncap = 2 * (int64_t)(re->prog.ngroups + 1);
    int64_t* caps = (int64_t*)re_alloc(sizeof(int64_t) * (size_t)ncap);
    int ok = find_from(&re->prog, text, 0, (int)mode, caps);
    Py23Match* m = ok ? match_new(text, re->prog.ngroups, caps) : NULL;
    free(caps);
    return (int64_t)m;
}

int64_t py23_re_group(Py23Match* m, int64_t n) { return (int64_t)group_str(m, n); }

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
        Py23Str* s = str_new(text->data + caps[0], caps[1] - caps[0]);
        py23_list_append(out, (int64_t)s);
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
        Py23Str* s = str_new(text->data + caps[0], caps[1] - caps[0]);
        py23_list_append(out, (int64_t)s);
        if (caps[1] == caps[0]) pos = caps[1] + 1;
        else pos = caps[1];
    }
    free(caps);
    return (int64_t)out;
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
            if (d == '\\') {
                buf_ch(b, '\\');
                i++;
                continue;
            }
        }
        buf_ch(b, c);
    }
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
