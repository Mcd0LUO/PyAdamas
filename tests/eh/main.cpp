// tests/eh/main.cpp —— LSDA 发射器的 golden 测试驱动
//
// 用法：eh_gen <输出目录>
//   为每个用例生成一份 <输出目录>/<name>.s：一个合成函数（.cfi_startproc /
//   .cfi_endproc）+ 由 src/eh_emit.cpp 发射的 .cfi_personality / .cfi_lsda
//   与 .gcc_except_table LSDA + DW.ref 指针槽。
//   run.sh 负责汇编、用 objcopy 抽出 .gcc_except_table 的原始字节，再和
//   gcc 14.2 实测基准（tools/lsda_ref/FINDINGS.md §0.1）逐字节比对。
//
// 不进 CMake：g++ -std=c++20 -I src tests/eh/main.cpp src/eh_emit.cpp 一条命令编完。
// 每个 .s 是独立 TU（各自带 DW.ref 槽），所以逐文件汇编、逐文件比对。

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "eh_emit.h"

namespace {

using pyadamas::EhFuncPlan;
using pyadamas::EhRange;

struct TestCase {
    const char* name;
    EhFuncPlan plan;
    std::string body;  // 函数体（不含 .cfi_startproc / .cfi_endproc）
};

// 与 gcc 14.2 的 catch-all 探针同款布局：
//   函数起点 0；保护区 [0x04, 0x09)（call 长 5）；landing pad 在 0x13；
//   landing pad 里的 call 在 [0x1b, 0x20) —— 对应 FINDINGS §0.1 的 20 字节基准。
std::string gcc20_body() {
    return
        "\tsubq $8, %rsp\n"
        "\t.cfi_def_cfa_offset 16\n"
        ".LEHB0:\n"
        "\tcall eh_dummy_may_throw\n"
        ".LEHE0:\n"
        "\tmovl $1, %eax\n"
        "\tjmp .Lret0\n"
        "\tnop\n\tnop\n\tnop\n"
        ".Llp0:\n"
        "\tmovl $2, %eax\n"
        "\tnop\n\tnop\n\tnop\n"
        ".LEHB1:\n"
        "\tcall eh_dummy_cleanup\n"
        ".LEHE1:\n"
        ".Lret0:\n"
        "\taddq $8, %rsp\n"
        "\t.cfi_def_cfa_offset 8\n"
        "\tret\n";
}

// gcc 的 cleanup 探针同款布局：
//   保护区 [0x05, 0x0a)；清理块（landing pad）在 0x0c；里面的
//   _Unwind_Resume 调用在 [0x1c, 0x21) —— 对应 12 字节基准。
std::string gcc12_body() {
    return
        "\tsubq $8, %rsp\n"
        "\t.cfi_def_cfa_offset 16\n"
        "\tnop\n"
        ".LEHB0:\n"
        "\tcall eh_dummy_ctor\n"
        ".LEHE0:\n"
        "\tnop\n\tnop\n"
        ".Llp0:\n"
        "\tmovq %rax, %rdi\n"
        "\tcall eh_dummy_free\n"
        "\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n"
        ".LEHB1:\n"
        "\tcall eh_unwind_resume\n"
        ".LEHE1:\n"
        "\tud2\n";
}

// 没有标签的最简函数体：数值模式的计划不依赖真实标签，多个函数可以共用。
std::string simple_body() {
    return
        "\tsubq $8, %rsp\n"
        "\t.cfi_def_cfa_offset 16\n"
        "\tcall eh_dummy_may_throw\n"
        "\taddq $8, %rsp\n"
        "\t.cfi_def_cfa_offset 8\n"
        "\tret\n";
}

// 渲染单个函数（.text 段）：函数标签 + .cfi_startproc + EH 指令 + 函数体。
std::string render_function(const TestCase& tc) {
    const int idx = tc.plan.lsda_index;
    const std::string fn = ".Lf" + std::to_string(idx);
    const std::string lfb = ".LFB" + std::to_string(idx);
    const std::string lfe = ".LFE" + std::to_string(idx);

    std::string out;
    out += "\t.text\n";
    out += "\t.globl " + fn + "\n";
    out += "\t.type " + fn + ", @function\n";
    out += fn + ":\n";
    out += lfb + ":\n";
    out += "\t.cfi_startproc\n";

    std::string dirs;
    pyadamas::emit_eh_func_directives(dirs, tc.plan);
    out += dirs;
    out += tc.body;

    out += "\t.cfi_endproc\n";
    out += lfe + ":\n";
    return out;
}

// 渲染本函数的 LSDA（.gcc_except_table 段）。
std::string render_table(const TestCase& tc) {
    std::string table;
    pyadamas::emit_eh_lsda(table, tc.plan);
    return table;
}

// TU 尾部：唯一的 DW.ref 槽 + .note.GNU-stack。
std::string render_tu_tail() {
    std::string out;
    std::string dwref;
    pyadamas::emit_eh_dwref(dwref);
    out += dwref;
    out += "\n\t.section .note.GNU-stack,\"\",@progbits\n";
    return out;
}

// 单函数 .s：函数正文 + 自己的 LSDA + 唯一一次 DW.ref 槽。
std::string build_asm(const TestCase& tc) {
    std::string out = "# 由 tests/eh/main.cpp 生成：用例 ";
    out += tc.name;
    out += "\n";
    out += render_function(tc);
    out += render_table(tc);
    out += render_tu_tail();
    return out;
}

// 两个函数共用一个 TU 的 .s：验证多个 LSDA 在同一个 .gcc_except_table 段里
// 的顺序/对齐，以及 DW.ref 槽只发一次（发两次汇编会报标签重定义）。
std::string build_two_asm(const TestCase& a, const TestCase& b) {
    std::string out = "# 由 tests/eh/main.cpp 生成：两个函数同一 TU（共用 DW.ref 槽）\n";
    out += render_function(a);
    out += render_function(b);
    out += render_table(a);
    out += render_table(b);
    out += render_tu_tail();
    return out;
}

std::vector<TestCase> make_cases() {
    std::vector<TestCase> cases;

    // 1) 单 range catch-all：只有一条 handler 记录（没有 LP 覆盖区间，
    //    Z2 约定「LP 只调用不会抛的运行时函数」）。16 字节形态。
    cases.push_back({"catch_single", EhFuncPlan{{{4, 9, 0x13, true}}, 0, nullptr}, gcc20_body()});

    // 2) 两条 range（handler + lp=0）：与 gcc 20 字节基准逐字节相同。
    cases.push_back({"catch_gcc20",
                     EhFuncPlan{{{4, 9, 0x13, true}, {0x1b, 0x20, 0, false}}, 1, nullptr},
                     gcc20_body()});

    // 3) 全是 cleanup range：12 字节形态（ttype=0xff、无 action/TType 表）。
    cases.push_back({"cleanup_gcc12",
                     EhFuncPlan{{{5, 0xa, 0xc, false}, {0x1c, 0x21, 0, false}}, 2, nullptr},
                     gcc12_body()});

    // 4) 多条 handler range：action 字段分别是 1 和 3（ABI 语义是 action 表
    //    里 1 起的**字节偏移**，每条 (filter=1,next=0) 记录 2 字节）；中间夹一条
    //    cleanup（action=0）。call-site 表里有超过 127 的偏移，专门验证
    //    「表长是字节数不是条目数」。只做字节比对，偏移可以超出合成函数体。
    cases.push_back({"multi_handler",
                     EhFuncPlan{{{4, 9, 0x13, true},
                                 {0x0a, 0x0f, 0x0b, false},
                                 {0x120, 0x125, 0x130, true}},
                                3,
                                nullptr},
                     gcc20_body()});

    // 5) 标签模式（codegen 集成的接法）：同一份 gcc 布局用
    //    .uleb128 .LEHE0-.LFB0 这类标签差发射，字节必须与用例 2 完全一致。
    cases.push_back({"catch_gcc20_labels",
                     EhFuncPlan{{{4, 9, 0x13, true, ".LEHB0", ".LEHE0", ".Llp0"},
                                 {0x1b, 0x20, 0, false, ".LEHB1", ".LEHE1", nullptr}},
                                4,
                                ".LFB4"},
                     gcc20_body()});
    return cases;
}

// eh_plan_check / emit_* 的自检（与字节比对互补，守住接口契约）。
int self_check() {
    int fails = 0;
    auto expect = [&](bool got, bool want, const char* what) {
        if (got == want) return;
        std::fprintf(stderr, "[FAIL] 自检：%s\n", what);
        ++fails;
    };
    auto check = [&](const EhFuncPlan& plan) {
        std::string error;
        return pyadamas::eh_plan_check(plan, &error);
    };
    auto throws = [&](const EhFuncPlan& plan) {
        std::string out;
        try {
            pyadamas::emit_eh_lsda(out, plan);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };

    expect(check(EhFuncPlan{}), true, "空计划合法（什么都不发射）");
    expect(check(EhFuncPlan{{{4, 9, 0x13, true}}, 0, nullptr}), true, "数值模式 catch-all 合法");
    expect(check(EhFuncPlan{{{4, 9, 0x13, true, ".B", ".E", ".L"}}, 0, ".F"}), true,
           "标签模式合法");
    expect(check(EhFuncPlan{{{9, 4, 0x13, true}}, 0, nullptr}), false, "begin >= end 非法");
    expect(check(EhFuncPlan{{{4, 9, 0, true}}, 0, nullptr}), false, "handler 无 landing pad 非法");
    expect(check(EhFuncPlan{{{4, 9, 0x13, true}, {4, 9, 0, false, ".B", ".E", nullptr}}, 0, ".F"}),
           false, "混用标签/数值模式非法");
    expect(check(EhFuncPlan{{{4, 9, 0x13, false, ".B", ".E", ".L"}}, 0, nullptr}), false,
           "标签模式缺 func_label 非法");

    std::string emitted;
    pyadamas::emit_eh_lsda(emitted, EhFuncPlan{});
    expect(emitted.empty(), true, "空计划不发射 LSDA");
    pyadamas::emit_eh_func_directives(emitted, EhFuncPlan{});
    expect(emitted.empty(), true, "空计划不发射函数级 CFI 指令");
    expect(throws(EhFuncPlan{{{4, 9, 0, true}}, 0, nullptr}), true,
           "非法计划发射时抛 std::invalid_argument");
    return fails;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "用法: %s <输出目录>\n", argv[0]);
        return 2;
    }
    if (self_check() != 0) return 1;
    std::printf("[self] eh_plan_check / 空计划 / 非法计划抛异常：全部通过\n");

    auto write_file = [&](const std::string& name, const std::string& text) {
        const std::string path = std::string(argv[1]) + "/" + name + ".s";
        std::ofstream f(path, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "[FAIL] 无法写入 %s\n", path.c_str());
            return false;
        }
        f << text;
        if (!f.good()) {
            std::fprintf(stderr, "[FAIL] 写 %s 失败\n", path.c_str());
            return false;
        }
        std::printf("[gen] %s\n", path.c_str());
        return true;
    };

    const std::vector<TestCase> cases = make_cases();
    for (const TestCase& tc : cases)
        if (!write_file(tc.name, build_asm(tc))) return 1;

    // 6) 两个函数同一个 TU：LSDA 在同一 .gcc_except_table 段里顺序追加、
    //    DW.ref 槽只发一次。两份计划都是单 handler（各 16 字节），第二份
    //    偏移不同，拼起来必须是 32 字节。
    const TestCase combo_a{"combo_a", EhFuncPlan{{{4, 9, 0x13, true}}, 5, nullptr}, simple_body()};
    const TestCase combo_b{"combo_b", EhFuncPlan{{{8, 0xb, 0x20, true}}, 6, nullptr}, simple_body()};
    if (!write_file("combine_two", build_two_asm(combo_a, combo_b))) return 1;

    return 0;
}
