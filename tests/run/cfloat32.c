/* cfloat32.c —— C float（单精度）编组测试辅助符号
 *
 * 0.304 前 cdecl 遇到 C float 参数/返回会整体跳过函数；现在生成
 * float32 种类，调用边界做 double↔float 转换（cvtsd2ss/cvtss2sd）。
 * 结构体里的 C float 字段同样按 float32 处理（4 字节/4 对齐）。
 */
float f32_mul(float a, float b) { return a * b; }
float f32_of_int(int n) { return (float)n * 0.5f; }
double f32_roundtrip(double x) { float f = (float)x; return (double)f; }
float f32_global = 2.5f;
static float f32_arr[3] = {1.25f, 2.5f, 3.75f};
float* f32_arr_ptr(void) { return f32_arr; }

struct fpair {
    float x;
    float y;
};

/* 8 字节、单个 SSE 块：按值返回/传参都走 xmm。 */
struct fpair fpair_make(float x, float y) {
    struct fpair p;
    p.x = x;
    p.y = y;
    return p;
}
float fpair_add(struct fpair p) { return p.x + p.y; }
static struct fpair fpair_g = {7.25f, 0.5f};
struct fpair* fpair_ptr(void) { return &fpair_g; }
float fpair_ptr_add(struct fpair* p) { return p->x + p->y; }
