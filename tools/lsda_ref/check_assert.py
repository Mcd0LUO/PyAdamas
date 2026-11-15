#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_assert.py —— check.sh 的断言集合。

把 FINDINGS.md 里断言的关键字节/字段硬编码在这里，重新编译解码后逐项比对。
所有期望值都来自本仓库 gen.sh 的实际输出（g++ 14.2.0 / binutils 2.44）。
任何一项不符：打印 [FAIL] 并以非零退出。
"""

import json
import os
import subprocess
import sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
PY = sys.executable
FAILS = []


def check(cond, msg):
    print(("  [PASS] " if cond else "  [FAIL] ") + msg)
    if not cond:
        FAILS.append(msg)


def eq(got, want, msg):
    check(got == want, "%s（期望 %r，实测 %r）" % (msg, want, got))


def dec(name):
    """解码 out/<name>.o，返回 JSON。"""
    obj = os.path.join(OUT, name + ".o")
    out = subprocess.check_output([PY, "decode_eh_lsda.py", obj, "--json"])
    return json.loads(out)


def section_bytes(obj, sec):
    tmp = os.path.join(OUT, "_check_tmp.bin")
    if os.path.exists(tmp):
        os.remove(tmp)
    subprocess.check_call(["objcopy", "-O", "binary",
                           "--only-section=" + sec, obj, tmp])
    with open(tmp, "rb") as f:
        data = f.read()
    os.remove(tmp)
    return data


def cie(d):
    return d["eh_frame"]["cies"][0]


def fde(d):
    return d["eh_frame"]["fdes"][0]


def enc(d):
    return cie(d)["encodings"]


def cs_tuple(d):
    return [(e["start"], e["length"], e["landing_pad"], e["action"],
             e["start_symbol"], e["end_symbol"], e["landing_pad_symbol"])
            for e in d["lsda"]["call_site_entries"]]


def main():
    # =====================================================================
    # 1. gcc 参考用例：.eh_frame 里的三个编码字节
    # =====================================================================
    print("## gcc 参考用例 .eh_frame 编码（CIE augmentation data = 9b 00 00 00 00 1b 1b）")
    for name in ("probe_catch_int", "probe_catch_all", "probe_cleanup"):
        d = dec(name + ".O1")
        eq(cie(d)["augmentation"], "zPLR", name + " CIE augmentation 字符串")
        eq(cie(d)["augmentation_data"], "9b000000001b1b",
           name + " CIE augmentation data 原始字节")
        eq(enc(d)["personality_encoding"], 0x9B,
           name + " personality 编码字节 0x9b")
        eq(enc(d)["personality_encoding_desc"],
           "DW_EH_PE_indirect|DW_EH_PE_pcrel|DW_EH_PE_sdata4",
           name + " personality 编码语义（不是 udata4）")
        eq(enc(d)["lsda_encoding"], 0x1B, name + " LSDA 指针编码字节 0x1b")
        eq(enc(d)["lsda_encoding_desc"], "DW_EH_PE_pcrel|DW_EH_PE_sdata4",
           name + " LSDA 指针编码语义（不是 udata4）")
        eq(enc(d)["fde_encoding"], 0x1B, name + " FDE 初始地址编码字节 0x1b")
        eq(enc(d)["personality_symbol"], "__gxx_personality_v0",
           name + " personality 间接层最终符号")
        check(enc(d)["personality_indirect"] is True,
              name + " CIE 的 personality 指针是 indirect（指向 DW.ref 槽）")
        eq(fde(d)["func_start_section_offset"], 0,
           name + " FDE 的 region start（= 函数起点）")
        eq(fde(d)["lsda_section"], ".gcc_except_table",
           name + " FDE 的 LSDA 指针目标节")
        eq(fde(d)["lsda_section_offset"], 0,
           name + " FDE 的 LSDA 在节内偏移")
        eq(d["lsda"]["lsda_offset"], 0, name + " LSDA 在节内偏移")
        eq(d["lsda"]["lpstart_encoding"], 0xFF, name + " LPStart 编码字节 0xff")
        eq(d["lsda"]["lpstart_relative_to_func_start"], True,
           name + " 0xff => LPStart 落回函数起点")
        eq(d["lsda"]["call_site_encoding"], 0x01,
           name + " call-site 表编码字节 0x01(uleb128)")
        eq(d["lsda"]["call_site_table_length"], 8,
           name + " call-site 表长度字节数")

    # =====================================================================
    # 2. 三个用例的 LSDA 完整字节 + 内部结构（逐项硬编码）
    # =====================================================================
    print()
    print("## LSDA 完整字节与内部结构")

    d = dec("probe_catch_int.O1")
    eq(d["lsda"]["hex"], "ff9b110108040513011c05000001000000000000",
       "catch(int) LSDA 完整 20 字节")
    eq(fde(d)["range"], 0x32, "catch(int) 函数长度 0x32")
    eq(d["lsda"]["ttype_encoding"], 0x9B, "catch(int) TType 编码 0x9b")
    eq(d["lsda"]["ttype_table_offset"], 0x11, "catch(int) TType 表偏移 0x11")
    eq(d["lsda"]["ttype_table_end"], 20, "catch(int) TType 表末尾")
    eq(d["lsda"]["ttype_table_start"], 16, "catch(int) TType 表起点")
    eq(cs_tuple(d), [
        (0x4, 0x5, 0x13, 1, ".LEHB0", ".LEHE0", ".L5"),
        (0x1C, 0x5, 0x0, 0, ".LEHB1", ".LEHE1", None),
    ], "catch(int) call-site 表（2 项，start/len 相对函数起点）")
    eq(len(d["lsda"]["action_entries"]), 1, "catch(int) action 表 1 条")
    eq((d["lsda"]["action_entries"][0]["offset"],
        d["lsda"]["action_entries"][0]["filter"],
        d["lsda"]["action_entries"][0]["next"],
        d["lsda"]["action_entries"][0]["raw"]),
       (13, 1, 0, "0100"),
       "catch(int) action 记录 = SLEB128(filter=1,next=0) @13")
    eq(len(d["lsda"]["ttype_entries"]), 1, "catch(int) TType 表 1 项")
    tt0 = d["lsda"]["ttype_entries"][0]
    eq(tt0["offset"], 16, "catch(int) TType[1] 位于表尾-4")
    eq(tt0["raw"], "00000000", "catch(int) TType 表项在 .o 里的原始字节（RELA 占位 0）")
    eq(tt0["reloc"]["symbol"] if tt0["reloc"] else None, "DW.ref._ZTIi",
       "catch(int) TType[1] 有重定位 → DW.ref._ZTIi")
    eq(tt0["value_symbol"], "_ZTIi", "catch(int) TType[1] 间接解出 type_info = _ZTIi")
    eq(d["lsda"]["action_table_hex"], "0100", "catch(int) action 表字节 01 00")

    d = dec("probe_catch_all.O1")
    eq(d["lsda"]["hex"], "ff9b110108040513011b05000001000000000000",
       "catch-all LSDA 完整 20 字节")
    eq(fde(d)["range"], 0x27, "catch-all 函数长度 0x27")
    eq(d["lsda"]["ttype_encoding"], 0x9B, "catch-all TType 编码仍是 0x9b（不是 omit）")
    eq(d["lsda"]["ttype_table_offset"], 0x11, "catch-all TType 表偏移 0x11（不是 0）")
    eq(d["lsda"]["ttype_table_end"], 20, "catch-all TType 表末尾")
    eq(d["lsda"]["ttype_table_start"], 16, "catch-all TType 表起点")
    eq(cs_tuple(d), [
        (0x4, 0x5, 0x13, 1, ".LEHB0", ".LEHE0", ".L4"),
        (0x1B, 0x5, 0x0, 0, ".LEHB1", ".LEHE1", None),
    ], "catch-all call-site 表")
    eq(len(d["lsda"]["action_entries"]), 1, "catch-all action 表 1 条")
    eq((d["lsda"]["action_entries"][0]["filter"],
        d["lsda"]["action_entries"][0]["next"]), (1, 0),
       "catch-all action 记录 = (filter=1, next=0)")
    eq(len(d["lsda"]["ttype_entries"]), 1, "catch-all TType 表有 1 项（不是空表）")
    tt0 = d["lsda"]["ttype_entries"][0]
    eq(tt0["raw"], "00000000", "catch-all TType[1] 原始字节 4 个 0")
    eq(tt0["reloc"], None, "catch-all TType[1] 没有重定位（区别于真 type_info 项）")
    eq(tt0["value"], 0, "catch-all TType[1] 解码值 = 0")
    check("catch(...)" in tt0["meaning"],
          "catch-all TType[1] 语义 = null type_info* => catch(...)")

    d = dec("probe_cleanup.O1")
    eq(d["lsda"]["hex"], "ffff010805050c001c050000",
       "cleanup-only LSDA 完整 12 字节")
    eq(fde(d)["range"], 0x36, "cleanup-only 函数长度 0x36")
    eq(d["lsda"]["ttype_encoding"], 0xFF,
       "cleanup-only TType 编码 = 0xff(omit)，TType 偏移字段整段消失")
    eq("ttype_table_offset" in d["lsda"], False,
       "cleanup-only 没有 ttype_table_offset 字段")
    eq(d["lsda"]["ttype_entries"], [], "cleanup-only 没有 TType 表")
    eq(d["lsda"]["action_entries"], [], "cleanup-only 没有 action 表")
    eq(d["lsda"]["action_table_start"], 12,
       "cleanup-only action 表起点 = call-site 表末尾")
    eq(cs_tuple(d), [
        (0x5, 0x5, 0x0C, 0, ".LEHB0", ".LEHE0", ".L3"),
        (0x1C, 0x5, 0x0, 0, ".LEHB1", ".LEHE1", None),
    ], "cleanup-only call-site 表（action=0 = 只有 cleanup）")

    # =====================================================================
    # 3. 手写 mini 汇编：声明字节真的落盘，且与 gcc 产物逐字节一致
    # =====================================================================
    print()
    print("## 手写 mini.S（0x9b 间接 personality）与 gcc 逐字节比较")
    mini = dec("mini_indirect")
    eq(cie(mini)["augmentation_data"], "9b000000001b1b",
       "mini_indirect CIE augmentation data（0x9b 间接 personality）")
    eq(enc(mini)["personality_encoding"], 0x9B, "mini_indirect personality 编码 0x9b")
    eq(enc(mini)["personality_indirect"], True, "mini_indirect 是 indirect 指针")
    eq(enc(mini)["personality_symbol"], "mini_personality",
       "mini_indirect 间接槽内容 = mini_personality")
    eq(mini["lsda"]["hex"], "ff9b110108040513011b05000001000000000000",
       "mini_indirect LSDA 20 字节 == 判定的 catch-all 形态")
    eq(cs_tuple(mini), [
        (0x4, 0x5, 0x13, 1, ".LEHB_mini", ".LEHE_mini", ".Lmini_lp"),
        (0x1B, 0x5, 0x0, 0, ".LEHB2_mini", ".LEHE2_mini", None),
    ], "mini_indirect call-site 表与 gcc catch-all 同构")

    gcc_lsda = section_bytes(os.path.join(OUT, "probe_catch_all.O1.o"),
                             ".gcc_except_table")
    mini_lsda = section_bytes(os.path.join(OUT, "mini_indirect.o"),
                              ".gcc_except_table")
    eq(len(gcc_lsda), 20, "gcc catch-all .gcc_except_table 长度 20 字节")
    check(gcc_lsda == mini_lsda,
          "手写 .gcc_except_table 与 gcc -O1 catch-all 逐字节完全一致")
    gcc_eh = section_bytes(os.path.join(OUT, "probe_catch_all.O1.o"),
                           ".eh_frame")
    mini_eh = section_bytes(os.path.join(OUT, "mini_indirect.o"), ".eh_frame")
    check(gcc_eh == mini_eh,
          "手写 .eh_frame（CIE+personality 编码+FDE+LSDA 指针）与 gcc 逐字节完全一致")

    # =====================================================================
    # 4. 省掉 DW.ref：0x1b 直接 pcrel 指向 personality 的三种落点
    # =====================================================================
    print()
    print("## 0x1b 直接 personality：本地符号 / 同可执行文件其它 TU / 共享库符号")
    d = dec("mini_direct")
    eq(cie(d)["augmentation_data"], "1b000000001b1b",
       "mini_direct CIE augmentation data（0x1b 直接 personality）")
    eq(enc(d)["personality_encoding"], 0x1B, "mini_direct personality 编码 0x1b")
    eq(enc(d)["personality_indirect"], False, "mini_direct 是直接 pcrel")
    eq(enc(d)["personality_symbol"], ".Lmini_pers",
       "mini_direct 直接指向本地 personality 符号")
    eq(d["lsda"]["hex"], "ff9b110108040513011b05000001000000000000",
       "mini_direct LSDA 与 catch-all 形态一致")

    d = dec("mini_direct_xo")
    eq(cie(d)["augmentation_data"], "1b000000001b1b",
       "mini_direct_xo CIE augmentation data（0x1b 直接 personality）")
    eq(enc(d)["personality_symbol"], "ext_personality",
       "mini_direct_xo 直接指向另一个 TU 的符号")
    eq(enc(d)["personality_defined"], False,
       "mini_direct_xo 在 .o 里该符号仍未定义（链接期由同一可执行文件解析）")

    d = dec("mini_direct_ext")
    eq(cie(d)["augmentation_data"], "1b000000001b1b",
       "mini_direct_ext CIE augmentation data（0x1b 直接 personality）")
    eq(enc(d)["personality_symbol"], "__gxx_personality_v0",
       "mini_direct_ext 直接指向共享库符号 __gxx_personality_v0")
    eq(enc(d)["personality_defined"], False,
       "mini_direct_ext 该符号未定义（来自 libstdc++.so）")

    # =====================================================================
    # 5. 链接后的重定位效果：catch-all 的 TType 零项保持 0
    # =====================================================================
    print()
    print("## 链接后字节（重定位应用）")
    exe_int = section_bytes(os.path.join(OUT, "probe_catch_int.exe"),
                            ".gcc_except_table")
    exe_all = section_bytes(os.path.join(OUT, "probe_catch_all.exe"),
                            ".gcc_except_table")
    obj_int = section_bytes(os.path.join(OUT, "probe_catch_int.O1.o"),
                            ".gcc_except_table")
    obj_all = section_bytes(os.path.join(OUT, "probe_catch_all.O1.o"),
                            ".gcc_except_table")
    eq(obj_all, exe_all,
       "catch-all：.o 与 .exe 的 .gcc_except_table 完全相同（无重定位）")
    eq(exe_all[-4:], b"\x00\x00\x00\x00",
       "catch-all 链接后 TType[1] 仍是 4 个 0（空指针 = catch(...)）")
    check(obj_int != exe_int,
          "catch(int)：链接后 .gcc_except_table 变化（TType 项被填真实 pcrel 值）")
    eq(obj_int[-4:], b"\x00\x00\x00\x00",
       "catch(int) 在 .o 里 TType 项原始字节是 0（RELA 占位）")
    check(exe_int[-4:] != b"\x00\x00\x00\x00",
          "catch(int) 在 .exe 里 TType 项是非零 pcrel 偏移")

    # =====================================================================
    # 6. g++ -S 的汇编文本（文档引用的那几行）确实如此
    # =====================================================================
    print()
    print("## g++ -O1 -S 汇编文本（文档引用的指令行）")
    for name in ("probe_catch_int", "probe_catch_all", "probe_cleanup"):
        path = os.path.join(OUT, name + ".O1.s")
        text = open(path, encoding="utf-8", errors="replace").read()
        check(".cfi_personality 0x9b,DW.ref.__gxx_personality_v0" in text,
              name + ".s 含 .cfi_personality 0x9b,DW.ref.__gxx_personality_v0")
        check(".cfi_lsda 0x1b,.LLSDA0" in text,
              name + ".s 含 .cfi_lsda 0x1b,.LLSDA0")
    s_all = open(os.path.join(OUT, "probe_catch_all.O1.s"),
                 encoding="utf-8", errors="replace").read()
    check(".long\t0" in s_all,
          "catch-all 的 .s 里 TType 表项是 .long 0（不是整段消失）")
    s_int = open(os.path.join(OUT, "probe_catch_int.O1.s"),
                 encoding="utf-8", errors="replace").read()
    check(".long\tDW.ref._ZTIi-." in s_int,
          "catch(int) 的 .s 里 TType 表项是 .long DW.ref._ZTIi-.")
    s_cl = open(os.path.join(OUT, "probe_cleanup.O1.s"),
                encoding="utf-8", errors="replace").read()
    check(".long" not in s_cl and "DW.ref._ZTIi" not in s_cl,
          "cleanup-only 的 .s 里没有 TType 表项")

    print()
    if FAILS:
        print("共 %d 项断言失败：" % len(FAILS))
        for f in FAILS:
            print("  - " + f)
        return 1
    print("全部断言通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
