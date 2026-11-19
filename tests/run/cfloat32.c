/* cfloat32.c —— C float（单精度）编组测试辅助符号
 *
 * 0.304 前 cdecl 遇到 C float 参数/返回会整体跳过函数；现在生成
 * float32 种类，调用边界做 double↔float 转换（cvtsd2ss/cvtss2sd）。
 */
float f32_mul(float a, float b) { return a * b; }
float f32_of_int(int n) { return (float)n * 0.5f; }
double f32_roundtrip(double x) { float f = (float)x; return (double)f; }
float f32_global = 2.5f;
static float f32_arr[3] = {1.25f, 2.5f, 3.75f};
float* f32_arr_ptr(void) { return f32_arr; }
