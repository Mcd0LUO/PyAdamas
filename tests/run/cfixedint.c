/* cfixedint.c —— P1 收尾：char/short/uchar/ushort/uint64 编组测试符号。
 * 参数/返回/全局变量/ptr 解引用/结构体字段/回调跳板的宽度与符号扩展。 */
char fi_char_id(char x) { return x; }
char fi_char_neg(void) { return -1; }
short fi_short_id(short x) { return x; }
short fi_short_min(void) { return -32768; }
unsigned char fi_uchar_id(unsigned char x) { return x; }
unsigned char fi_uchar_max(void) { return 255; }
unsigned short fi_ushort_id(unsigned short x) { return x; }
unsigned short fi_ushort_max(void) { return 65535; }
unsigned long fi_u64_id(unsigned long x) { return x; }
unsigned long fi_u64_max(void) { return 18446744073709551615UL; }
unsigned long fi_u64_add(unsigned long a, unsigned long b) { return a + b; }

char fi_char_global = -5;
unsigned short fi_ushort_global = 65535;
unsigned long fi_u64_global = 18446744073709551615UL;

static char fi_char_arr[3] = {-128, 0, 127};
char* fi_char_arr_ptr(void) { return fi_char_arr; }
static unsigned long fi_u64_arr[2] = {9223372036854775808UL, 18446744073709551615UL};
unsigned long* fi_u64_arr_ptr(void) { return fi_u64_arr; }

struct FiMixed { char c; short s; unsigned char uc; unsigned short us; unsigned long u64; };
struct FiMixed fi_struct_make(char c, short s, unsigned char uc, unsigned short us,
                              unsigned long u64) {
    struct FiMixed m;
    m.c = c; m.s = s; m.uc = uc; m.us = us; m.u64 = u64;
    return m;
}

/* 回调：C 侧按 char/short/uchar/ushort/uint64 传参，跳板要按宽度提升。 */
unsigned long fi_cb_call(unsigned long (*cb)(char, short, unsigned char,
                                             unsigned short, unsigned long)) {
    return cb(-1, -2, 255, 65535, 18446744073709551615UL);
}
