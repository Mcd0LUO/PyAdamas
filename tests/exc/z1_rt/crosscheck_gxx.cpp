// crosscheck_gxx.cpp —— 用真的 libstdc++ __gxx_personality_v0 裁决
// tests/exc/z1_rt/z1_step2.S 升级后的 handler LSDA 是否按 ABI 语义是 catch-all。
//
// 复刻自 tools/lsda_ref/crosscheck_m2/m2_driver.cpp（只做适配，不修改 tools/ 下任何文件）：
//   - 夹具用 -Wa,--defsym,Z1_CROSSCHECK=1 编出「保护区里只有一条 C++ throw」的变体；
//   - 夹具 CIE 的 0x9b personality 槽在运行期填 __gxx_personality_v0（不是 pa_personality）；
//   - 外层用真正的 C++ try/catch(int) 观察：
//       如果夹具帧被当 catch-all handler：landing pad 跑一次、异常被吃掉
//       （外层 catch 拿不到 7），函数正常返回 → exit 0；
//       如果被当 cleanup（旧形态的错误语义）：landing pad 跑一次后异常继续、
//       外层 catch 拿到 7 → 本程序 exit 1。
#include <cstdio>
#include <cstdint>
#include <unwind.h>

// 夹具 .S 引用的符号（这套二进制的唯一一份定义）
extern "C" {
void *z1_personality_slot = nullptr;
volatile int z1_step2_seen_lp = 0;
volatile uintptr_t z1_step2_seen_exc = 0;
}

extern "C" int z1_step2_fn();

static int cc_lp_calls;

// 夹具 landing pad 里调用它（夹具 LSDA 已给这条 call 补了 lp=0 的 call-site 记录）
extern "C" void z1_note(int tag, uintptr_t exc) {
    (void)tag;
    (void)exc;
    ++cc_lp_calls;
}

// 夹具保护区里调用它：真的 C++ throw
extern "C" void z1_cxx_throw() {
    throw 7;
}

// libstdc++ 的 personality，符号在 libstdc++.so 里
extern "C" _Unwind_Reason_Code __gxx_personality_v0(
    int version, _Unwind_Action actions, _Unwind_Exception_Class ex_class,
    struct _Unwind_Exception *exc, struct _Unwind_Context *ctx);

int main() {
    z1_personality_slot = (void *)&__gxx_personality_v0;
    int caught = 0;
    int rc = 0;
    try {
        rc = z1_step2_fn();
    } catch (int v) {
        caught = v;
    }
    std::fprintf(stderr,
                 "[crosscheck] caught=%d lp_calls=%d fn_ret=%d z1_lp_seen=%d\n",
                 caught, cc_lp_calls, rc, (int)z1_step2_seen_lp);
    // catch-all 语义：异常被夹具 landing pad 吃掉（caught==0），LP 恰好跑一次
    return (caught == 0 && cc_lp_calls == 1 && z1_step2_seen_lp == 1) ? 0 : 1;
}
