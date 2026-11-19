/* probe.c —— cdll（dlopen/dlsym）端到端测试库
 *
 * 构建说明（tests/run/213_cdll.py 会用 subprocess 自动执行同一命令）：
 *   cc -shared -fPIC -O2 tests/cdll/probe.c -o <out>.so
 * 然后 pyadamas 侧：
 *   import cdll
 *   lib = cdll.load("<out>.so")
 *   lib.probe_add(2, 3)
 *
 * 签名故意覆盖：int/int→int、double→double、int/str 混合、char* 返回
 * （需 str 标注）、C float（懒调用契约按 double 传，float 编组走
 * cimport float32，见 215_c_float32.py）。
 */
#include <stdint.h>
#include <string.h>

int64_t probe_add(int64_t a, int64_t b) { return a + b; }
double probe_scale(double x) { return x * 2.5; }
int64_t probe_mix(int64_t n, const char* s) { return n + (int64_t)strlen(s); }
const char* probe_greet(const char* s) { return s; }
