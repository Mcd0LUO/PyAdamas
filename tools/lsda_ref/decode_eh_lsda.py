#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""decode_eh_lsda.py —— 从 ELF64 可重定位目标文件里逐字节解码 .eh_frame 与 LSDA。

用途：M1 侦察（docs/zero-cost-exceptions.md 的 LSDA 字节布局核对）。
本工具只依赖标准库：自己解析 section / symtab / rela，不依赖 pyelftools。

输入必须是 ET_REL（.o），因为：
  - LSDA 里的 TType 表项、LSDA 指针、personality 指针在 .o 里是“重定位”而非最终值；
    只有读 .rela.* 才能知道它指向哪个符号（以及 catch-all 的零项到底有没有重定位）。
输出：默认人类可读；--json 输出机器可读结果（check.sh 用它做硬断言）。

字段布局（GCC 14 / x86-64，实测；与 libsupc++ eh_personality.cc 的
parse_lsda_header() 和 libc++abi cxa_personality.cpp 的实现一致）：

  LSDA :=
    u8         lpstart_encoding        # 0xff = DW_EH_PE_omit，此时 LPStart := FDE 的 region start
    [encoded   lpstart]                # 仅当 lpstart_encoding != 0xff
    u8         ttype_encoding          # 0x9b = indirect|pcrel|sdata4（GCC 默认）
    [uleb128   ttype_table_offset]     # 仅当 ttype_encoding != 0xff；指向 TType 表“末尾”，反向索引
    u8         call_site_encoding      # 0x01 = uleb128（GCC 默认）
    uleb128    call_site_table_length  # 字节数
    call_site...                       # 每项: start, len, landing_pad, action
    action_table...                    # 每项: (sleb128 filter, sleb128 next)，被 call-site 的 action 引用
    ttype_table...                     # 仅当 ttype_encoding != 0xff；定宽项，反向索引

  call-site 项（4 个字段）：
    start       encoded with call_site_encoding，相对 **FDE region start（函数起点）**
    len         encoded with call_site_encoding
    landing_pad encoded with call_site_encoding，相对 LPStart；0 = 该区间无 landing pad
    action      uleb128（始终 uleb128，与 call_site_encoding 无关），1 起下标；0 = 只有 cleanup
"""

import argparse
import json
import struct
import sys

SHT_SYMTAB = 2
SHT_RELA = 4
SHN_UNDEF = 0

# DWARF 编码常量（DWARF 4 §7.7.1；数值与 libgcc/unwind-pe.h 一致）
DW_EH_PE_absptr = 0x00
DW_EH_PE_uleb128 = 0x01
DW_EH_PE_udata2 = 0x02
DW_EH_PE_udata4 = 0x03
DW_EH_PE_udata8 = 0x04
DW_EH_PE_sleb128 = 0x09
DW_EH_PE_sdata2 = 0x0A
DW_EH_PE_sdata4 = 0x0B
DW_EH_PE_sdata8 = 0x0C
DW_EH_PE_pcrel = 0x10
DW_EH_PE_textrel = 0x20
DW_EH_PE_datarel = 0x30
DW_EH_PE_funcrel = 0x40
DW_EH_PE_aligned = 0x50
DW_EH_PE_indirect = 0x80
DW_EH_PE_omit = 0xFF

FORMAT_NAMES = {
    0x00: "DW_EH_PE_absptr",
    0x01: "DW_EH_PE_uleb128",
    0x02: "DW_EH_PE_udata2",
    0x03: "DW_EH_PE_udata4",
    0x04: "DW_EH_PE_udata8",
    0x09: "DW_EH_PE_sleb128",
    0x0A: "DW_EH_PE_sdata2",
    0x0B: "DW_EH_PE_sdata4",
    0x0C: "DW_EH_PE_sdata8",
}
APPL_NAMES = {0x00: "", 0x10: "pcrel", 0x20: "textrel", 0x30: "datarel",
              0x40: "funcrel", 0x50: "aligned"}


def enc_desc(enc):
    """把 DWARF EH pointer encoding 字节翻译成人类可读名字。"""
    if enc == DW_EH_PE_omit:
        return "DW_EH_PE_omit"
    fmt = FORMAT_NAMES.get(enc & 0x0F)
    if fmt is None:
        fmt = "0x%02x(未知格式)" % (enc & 0x0F)
    bits = []
    if enc & DW_EH_PE_indirect:
        bits.append("DW_EH_PE_indirect")
    app = APPL_NAMES.get(enc & 0x70)
    if app:
        bits.append("DW_EH_PE_" + app)
    bits.append(fmt)
    return "|".join(bits)


def enc_size(enc):
    """定长编码占几个字节；leb128 返回 None（变长）。"""
    if enc == DW_EH_PE_omit:
        return 0
    if (enc & 0x0F) in (DW_EH_PE_uleb128, DW_EH_PE_sleb128):
        return None
    return {0: 8, 2: 2, 3: 4, 4: 8}.get(enc & 0x07)


def uleb(buf, pos):
    result = 0
    shift = 0
    start = pos
    while True:
        byte = buf[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not (byte & 0x80):
            break
    return result, pos, buf[start:pos]


def sleb(buf, pos):
    result = 0
    shift = 0
    start = pos
    while True:
        byte = buf[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not (byte & 0x80):
            if byte & 0x40:
                result -= 1 << shift
            break
    return result, pos, buf[start:pos]


def hexs(b):
    return b.hex()


def hexsp(b):
    return " ".join("%02x" % x for x in b)


class ELF:
    def __init__(self, path):
        self.path = path
        self.buf = open(path, "rb").read()
        if self.buf[:4] != b"\x7fELF":
            raise SystemExit("%s: 不是 ELF 文件" % path)
        if self.buf[4] != 2:
            raise SystemExit("%s: 只支持 ELF64" % path)
        self.end = "<" if self.buf[5] == 1 else ">"
        self.e_type, = struct.unpack_from(self.end + "H", self.buf, 16)
        self.e_shoff, = struct.unpack_from(self.end + "Q", self.buf, 0x28)
        self.e_shentsize, self.e_shnum, self.e_shstrndx = struct.unpack_from(
            self.end + "HHH", self.buf, 0x3A)
        self.sections = []
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size,
             sh_link, sh_info, sh_addralign, sh_entsize) = struct.unpack_from(
                self.end + "IIQQQQIIQQ", self.buf, off)
            self.sections.append(dict(
                index=i, name_off=sh_name, type=sh_type, flags=sh_flags,
                addr=sh_addr, offset=sh_offset, size=sh_size, link=sh_link,
                info=sh_info, align=sh_addralign, entsize=sh_entsize))
        shstr = self.sections[self.e_shstrndx]
        for s in self.sections:
            s["name"] = self.cstr(shstr["offset"] + s["name_off"])
        self.by_name = {s["name"]: s for s in self.sections}
        self.symbols = []
        for s in self.sections:
            if s["type"] == SHT_SYMTAB:
                strtab = self.sections[s["link"]]
                for i in range(s["size"] // 24):
                    o = s["offset"] + i * 24
                    st_name, st_info, st_other, st_shndx, st_value, st_size = \
                        struct.unpack_from(self.end + "IBBHQQ", self.buf, o)
                    self.symbols.append(dict(
                        index=i, name=self.cstr(strtab["offset"] + st_name),
                        info=st_info, shndx=st_shndx, value=st_value,
                        size=st_size))
        # 重定位：target_section_index -> {r_offset: {...}}
        self.relocs = {}
        for s in self.sections:
            if s["type"] != SHT_RELA:
                continue
            target = s["info"]
            self.relocs.setdefault(target, {})
            entsize = s["entsize"] or 24
            for i in range(s["size"] // entsize):
                o = s["offset"] + i * entsize
                r_offset, r_info, r_addend = struct.unpack_from(
                    self.end + "QQq", self.buf, o)
                symidx = r_info >> 32
                rtype = r_info & 0xFFFFFFFF
                self.relocs[target][r_offset] = dict(
                    symidx=symidx, rtype=rtype, addend=r_addend,
                    rela_section=s["name"])

    def cstr(self, off):
        end = self.buf.index(b"\x00", off)
        return self.buf[off:end].decode("latin-1")

    def sec(self, name):
        s = self.by_name.get(name)
        if s is None:
            raise SystemExit("%s: 缺少节 %s" % (self.path, name))
        return s

    def sec_bytes(self, name):
        s = self.sec(name)
        return self.buf[s["offset"]:s["offset"] + s["size"]]

    def reloc_at(self, secidx, off):
        return self.relocs.get(secidx, {}).get(off)

    def symname(self, idx):
        if idx >= len(self.symbols):
            return "<sym#%d>" % idx
        s = self.symbols[idx]
        if s["name"]:
            return s["name"]
        if 0 < s["shndx"] < len(self.sections):
            return "<%s>" % self.sections[s["shndx"]]["name"]
        return "<sym#%d>" % idx

    def symbol_at(self, secidx, value):
        names = []
        for s in self.symbols:
            if s["shndx"] == secidx and s["value"] == value and s["name"]:
                names.append(s["name"])
        return names

    def preferred_symbol_at(self, secidx, value, prefixes, exclude_prefixes=()):
        names = [n for n in self.symbol_at(secidx, value)
                 if not any(n.startswith(e) for e in exclude_prefixes)]
        for pfx in prefixes:
            for n in names:
                if n.startswith(pfx):
                    return n
        return names[0] if names else None

    def section_symbols(self, secidx):
        return [s for s in self.symbols
                if s["shndx"] == secidx and s["name"]]


def resolve_reloc_chain(elf, secidx, field_off, indirect):
    """解析字段的重定位目标。

    indirect=False：字段就是最终地址（直接 pcrel 指向目标符号）。
    indirect=True ：字段 pcrel 指向一个指针槽（如 DW.ref.*），槽里才是目标地址；
                    槽内无重定位 = 槽内容为 0（catch-all 的 TType 空项就是这种）。
    """
    rel = elf.reloc_at(secidx, field_off)
    info = dict(raw_reloc=rel, field_value=None, indirect=indirect,
                target_sym=None, target_defined=None, target_section=None,
                chain="")
    if rel is None:
        info["chain"] = "无重定位"
        return info
    sym = elf.symbols[rel["symidx"]]
    addr = sym["value"] + rel["addend"]
    sym_name = elf.symname(rel["symidx"])
    if not indirect:
        if sym["shndx"] == SHN_UNDEF:
            info.update(field_value=addr, target_sym=sym_name,
                        target_defined=False, target_section=None,
                        chain="pcrel 直接指向 %s（未定义，链接期解析）" % sym_name)
            return info
        sec = elf.sections[sym["shndx"]]
        names = elf.symbol_at(sym["shndx"], addr) if sym_name.startswith("<") else []
        name = names[0] if names else sym_name
        info.update(field_value=addr, target_sym=name, target_defined=True,
                    target_section=sec["name"],
                    chain="pcrel 直接指向 %s (%s+0x%x)" % (name, sec["name"], addr))
        return info
    if sym["shndx"] == SHN_UNDEF:
        info.update(field_value=addr, target_sym=sym_name,
                    target_defined=False, target_section=None,
                    chain="%s (未定义，链接期解析)" % sym_name)
        return info
    slot_sec = elf.sections[sym["shndx"]]
    slot_rel = elf.reloc_at(sym["shndx"], addr)
    if slot_rel is not None:
        final_idx = slot_rel["symidx"]
        final_sym = elf.symbols[final_idx]
        final_addr = final_sym["value"] + slot_rel["addend"]
        final_name = elf.symname(final_idx)
        if final_name.startswith("<") and final_sym["shndx"]:
            names = elf.symbol_at(final_sym["shndx"], final_addr)
            if names:
                final_name = names[0]
        info.update(field_value=addr, target_sym=final_name,
                    target_defined=(final_sym["shndx"] != SHN_UNDEF),
                    target_section=(elf.sections[final_sym["shndx"]]["name"]
                                    if final_sym["shndx"] else None),
                    chain="%s → (*slot@%s+0x%x) = %s" % (
                        sym_name, slot_sec["name"], addr, final_name))
        return info
    info.update(field_value=addr, target_sym=None, target_defined=None,
                target_section=None,
                chain="%s → (*slot@%s+0x%x) = 0（空指针）" % (
                    sym_name, slot_sec["name"], addr))
    return info


def parse_eh_frame(elf):
    """解析 .eh_frame 的 CIE/FDE。"""
    secidx = elf.sec(".eh_frame")["index"]
    b = elf.sec_bytes(".eh_frame")
    cies = {}
    fdes = []
    pos = 0
    while pos + 4 <= len(b):
        length, = struct.unpack_from(elf.end + "I", b, pos)
        pos += 4
        if length == 0:
            break
        end = pos + length
        ident, = struct.unpack_from(elf.end + "I", b, pos)
        if ident == 0:
            cie_start = pos - 4
            version = b[pos + 4]
            p = pos + 5
            z = b.index(b"\x00", p)
            aug = b[p:z].decode("latin-1")
            p = z + 1
            code_align, p, _ = uleb(b, p)
            data_align, p, _ = sleb(b, p)
            retreg = None
            if version >= 1:
                retreg, p, _ = uleb(b, p)
            aug_data = None
            aug_data_off = None
            aug_data_len = 0
            if "z" in aug:
                aug_data_len, p, _ = uleb(b, p)
                aug_data_off = p
                aug_data = b[p:p + aug_data_len]
                p += aug_data_len
            cies[cie_start] = dict(
                offset=cie_start, version=version, aug=aug,
                code_align=code_align, data_align=data_align,
                retreg=retreg, aug_data=aug_data,
                aug_data_off=aug_data_off, aug_data_len=aug_data_len,
                length=length)
        else:
            fde_start = pos - 4
            cie_start = pos - ident
            cie = cies.get(cie_start)
            if cie is None:
                raise SystemExit("%s: FDE@0x%x 找不到 CIE(cie_ptr=%d)" %
                                 (elf.path, fde_start, ident))
            p = pos + 4
            fde_enc = None
            pers_enc = None
            lsda_enc = None
            if cie["aug_data"]:
                q = 0
                ad = cie["aug_data"]
                for ch in cie["aug"]:
                    if ch == "P":
                        pers_enc = ad[q]
                        q += 1
                        q += enc_size(pers_enc)
                    elif ch == "L":
                        lsda_enc = ad[q]
                        q += 1
                    elif ch == "R":
                        fde_enc = ad[q]
                        q += 1
                    elif ch in "zB":
                        pass
                    else:
                        raise SystemExit("不认识的 augmentation %r" % ch)
            fields = dict(fde_offset=fde_start, cie_offset=cie_start,
                          cie=cie, personality_encoding=pers_enc,
                          lsda_encoding=lsda_enc, fde_encoding=fde_enc)
            sz = enc_size(fde_enc)
            init_raw = b[p:p + sz]
            init_off = p
            rel = elf.reloc_at(secidx, init_off)
            if rel is not None:
                sym = elf.symbols[rel["symidx"]]
                init_value = sym["value"] + rel["addend"]
                fields["func_start_section_offset"] = init_value
                fields["func_start_symbol"] = elf.symname(rel["symidx"])
            else:
                init_value = int.from_bytes(init_raw, "little", signed=True)
                fields["func_start_section_offset"] = init_value
                fields["func_start_symbol"] = None
            p += sz
            range_raw = b[p:p + sz]
            fields["range"] = int.from_bytes(range_raw, "little", signed=False)
            p += sz
            lsda_off_in_section = None
            if "z" in cie["aug"]:
                alen, p, _ = uleb(b, p)
                ad_start = p
                fields["fde_aug_data_len"] = alen
                if lsda_enc is not None:
                    sz2 = enc_size(lsda_enc)
                    lsda_field_off = ad_start
                    lsda_raw = b[p:p + sz2]
                    rel2 = elf.reloc_at(secidx, lsda_field_off)
                    if rel2 is not None:
                        sym2 = elf.symbols[rel2["symidx"]]
                        lsda_off_in_section = sym2["value"] + rel2["addend"]
                        fields["lsda_target_symbol"] = elf.symname(rel2["symidx"])
                        fields["lsda_target_section"] = (
                            elf.sections[sym2["shndx"]]["name"]
                            if sym2["shndx"] else None)
                    else:
                        val = int.from_bytes(lsda_raw, "little", signed=True)
                        lsda_off_in_section = val
                        fields["lsda_target_symbol"] = None
                        fields["lsda_target_section"] = None
                    fields["lsda_field_offset"] = lsda_field_off
                    fields["lsda_raw"] = lsda_raw
                p = ad_start + alen
            fields["lsda_section_offset"] = lsda_off_in_section
            fdes.append(fields)
        pos = end
    return cies, fdes


def enc_valid(enc):
    """判断一个字节是否是合法的 DWARF EH pointer encoding（0xff=omit 合法）。"""
    if enc == DW_EH_PE_omit:
        return True
    if (enc & 0x0F) not in FORMAT_NAMES:
        return False
    if (enc & 0x70) not in APPL_NAMES:
        return False
    return True


def decode_lsda(elf, lsda_off_in_section, func_start_off, lsda_end=None,
                raw_bytes=None):
    """按 Itanium ABI 逐字节解码一段 LSDA。

    elf 为 None 时表示解码一段裸字节（--raw-hex）：不做符号/重定位解析。
    lsda_end 是 LSDA 在 .gcc_except_table 里的可用上界（下一个 LSDA 起点或节尾），
    用来区分「LSDA 逻辑长度」与「对齐 padding / 后续数据」。
    """
    if raw_bytes is not None:
        b = raw_bytes
        base_off = 0
        secidx = None
        out = dict(lsda_offset=0, section="<raw-hex>", hex=hexs(b),
                   checks=[], raw=True, lsda_available_len=len(b))
    else:
        whole = elf.sec_bytes(".gcc_except_table")
        if lsda_end is None:
            lsda_end = len(whole)
        b = whole[lsda_off_in_section:lsda_end]
        base_off = lsda_off_in_section
        secidx = elf.sec(".gcc_except_table")["index"]
        out = dict(lsda_offset=lsda_off_in_section,
                   section=".gcc_except_table", hex=hexs(b), checks=[],
                   raw=False, lsda_available_end=lsda_end,
                   lsda_available_len=len(b))
    checks = out["checks"]
    out["structural_error"] = None

    def field(offset, name, raw, value, note):
        out.setdefault("fields", []).append(
            dict(offset=offset, name=name, raw=hexs(raw), value=value,
                 note=note))

    def struct_fail(msg):
        out["structural_error"] = msg
        checks.append(dict(name="LSDA 结构可解析", ok=False, detail=msg))
        return out

    def read_enc_field(pos, enc, what):
        if enc == DW_EH_PE_omit:
            raise ValueError("%s 用 omit 编码非法" % what)
        fmt = enc & 0x0F
        if fmt == DW_EH_PE_uleb128:
            return uleb(b, pos)
        if fmt == DW_EH_PE_sleb128:
            return sleb(b, pos)
        size = enc_size(enc)
        if size is None:
            raise ValueError("%s 的编码 0x%02x 长度不确定" % (what, enc))
        if pos + size > len(b):
            raise IndexError(
                "%s 字段越界：需要 %d 字节，起始 0x%x，本段仅剩 0x%x"
                % (what, size, pos, len(b) - pos))
        raw = b[pos:pos + size]
        signed = fmt in (DW_EH_PE_sdata2, DW_EH_PE_sdata4,
                         DW_EH_PE_sdata8)
        return (int.from_bytes(raw, "little", signed=signed), pos + size, raw)

    # ---------------------------------------------------------------- header
    if len(b) < 1:
        return struct_fail("空 LSDA")
    p = 0
    lpstart_enc = b[p]
    field(p, "lpstart_encoding", b[p:p + 1], "0x%02x" % lpstart_enc,
          enc_desc(lpstart_enc) +
          ("（0xff=omit → LPStart 取 FDE region start）"
           if lpstart_enc == DW_EH_PE_omit else ""))
    out["lpstart_encoding"] = lpstart_enc
    p += 1
    lpstart = func_start_off
    out["lpstart_relative_to_func_start"] = True
    if lpstart_enc != DW_EH_PE_omit:
        checks.append(dict(name="LPStart 编码字节合法", ok=enc_valid(lpstart_enc),
                           detail=enc_desc(lpstart_enc)))
        try:
            v, p, raw = read_enc_field(p, lpstart_enc, "LPStart")
        except (IndexError, ValueError) as ex:
            return struct_fail("LPStart 解码失败：%s" % ex)
        field(p - len(raw), "lpstart", raw, v,
              "LPStart 编码值（相对基址见编码字节）")
        lpstart = func_start_off + v
        out["lpstart_relative_to_func_start"] = False
    out["lpstart"] = lpstart

    # TType：关键点是 0xff 时「TType 偏移字段整段消失」
    ttype_enc = b[p]
    field(p, "ttype_encoding", b[p:p + 1], "0x%02x" % ttype_enc,
          enc_desc(ttype_enc) +
          ("（0xff=omit：没有类型表，因此后面的 TType offset 字段不存在）"
           if ttype_enc == DW_EH_PE_omit else ""))
    out["ttype_encoding"] = ttype_enc
    p += 1
    ttype_end = None
    ttype_offset = None
    out["ttype_offset_field_present"] = (ttype_enc != DW_EH_PE_omit)
    if ttype_enc == DW_EH_PE_omit:
        checks.append(dict(
            name="ttype_encoding=0xff → TType 偏移字段消失",
            ok=True,
            detail="符合 ABI：紧跟其后的是 call-site 编码字节（0x%02x）"
                   % b[p]))
    else:
        checks.append(dict(name="TType 编码字节合法", ok=enc_valid(ttype_enc),
                           detail=enc_desc(ttype_enc)))
        try:
            ttype_offset, p, raw = uleb(b, p)
        except IndexError:
            return struct_fail("TType 偏移 uleb128 越界")
        field(p - len(raw), "ttype_table_offset", raw, ttype_offset,
              "uleb128；从本字段之后到 TType 表末尾的偏移（表反向索引）")
        out["ttype_table_offset"] = ttype_offset
        ttype_end = p + ttype_offset
        out["ttype_table_end"] = ttype_end
        ok = ttype_end <= len(b)
        checks.append(dict(name="TType 表末尾不越过本段可用字节",
                           ok=ok, detail="ttype_end=0x%x 可用=0x%x"
                           % (ttype_end, len(b))))
        if not ok:
            return struct_fail(
                "TType 偏移 0x%x 把表末尾指到本段之外（可用 0x%x 字节），"
                "结构非法" % (ttype_offset, len(b)))

    # ---------------------------------------------------------- call-site 表
    call_site_enc = b[p]
    field(p, "call_site_encoding", b[p:p + 1], "0x%02x" % call_site_enc,
          enc_desc(call_site_enc) +
          ("（数据字段按此编码；action 字段例外，固定 uleb128）"))
    out["call_site_encoding"] = call_site_enc
    checks.append(dict(name="call-site 编码字节合法",
                       ok=enc_valid(call_site_enc),
                       detail=enc_desc(call_site_enc)))
    p += 1
    cs_len, p_after, raw = uleb(b, p)
    field(p, "call_site_table_length", raw, cs_len,
          "uleb128；call-site 表的字节长度，表后紧跟 action 表")
    p = p_after
    out["call_site_table_length"] = cs_len
    out["call_site_table_start"] = p
    action_table_start = p + cs_len
    out["action_table_start"] = action_table_start
    if action_table_start > len(b):
        return struct_fail(
            "call-site 表长度 0x%x 越界：表起点 0x%x + 长度后到 0x%x，"
            "本段可用只有 0x%x 字节（结构非法）"
            % (cs_len, p, action_table_start, len(b)))
    entries = []
    q = p
    try:
        while q < action_table_start:
            ent_off = q
            e = dict(index=len(entries) + 1, offset=ent_off)
            for fname in ("start", "length", "landing_pad"):
                v, q, rawf = read_enc_field(q, call_site_enc, fname)
                e[fname + "_raw"] = hexs(rawf)
                e[fname] = v
            action, q, rawf = uleb(b, q)
            e["action"] = action
            e["action_raw"] = hexs(rawf)
            e["start_abs"] = func_start_off + e["start"]
            e["end_abs"] = func_start_off + e["start"] + e["length"]
            e["landing_pad_abs"] = (lpstart + e["landing_pad"]
                                    if e["landing_pad"] else None)
            e["start_symbol"] = None
            e["end_symbol"] = None
            e["landing_pad_symbol"] = None
            e["raw"] = hexs(b[ent_off:q])
            entries.append(e)
    except (IndexError, ValueError) as ex:
        out["call_site_entries"] = entries
        return struct_fail(
            "call-site 表解析失败（call_site_encoding=%s＝0x%02x，"
            "表长 0x%x）：%s" % (enc_desc(call_site_enc), call_site_enc,
                                cs_len, ex))
    out["call_site_entries"] = entries
    consumed = q - p
    checks.append(dict(name="call-site 表实际字节 == 长度字段",
                       ok=(consumed == cs_len),
                       detail="consumed=0x%x len=0x%x" % (consumed, cs_len)))
    if elf is not None:
        text_secidx = elf.sec(".text")["index"]
        for e in entries:
            e["start_symbol"] = elf.preferred_symbol_at(
                text_secidx, e["start_abs"], (".LEHB", "LEHB"))
            e["end_symbol"] = elf.preferred_symbol_at(
                text_secidx, e["end_abs"], (".LEHE", "LEHE"))
            if e["landing_pad"]:
                e["landing_pad_symbol"] = elf.preferred_symbol_at(
                    text_secidx, e["landing_pad_abs"],
                    (".Llp", "Lmini_lp", ".L"), exclude_prefixes=(".LEH",))
            ok = (e["start_symbol"] is not None
                  and e["end_symbol"] is not None)
            checks.append(dict(
                name="call_site[%d] 起止地址能对回符号" % e["index"],
                ok=ok, detail="%s..%s" % (e["start_symbol"],
                                          e["end_symbol"])))

    # ------------------------------------------------------------- action 表
    action_entries = []
    visited = set()
    for e in entries:
        if not e["action"]:
            continue
        off = action_table_start + (e["action"] - 1)
        while off is not None and off not in visited:
            if off < action_table_start or off >= len(b):
                checks.append(dict(
                    name="call_site[%d] 的 action 下标可解析" % e["index"],
                    ok=False,
                    detail="action=%d → 偏移 0x%x 越界（本段 0x%x 字节）"
                           % (e["action"], off, len(b))))
                off = None
                break
            visited.add(off)
            rec_off = off
            f, off2, rawf = sleb(b, off)
            nxt, off3, rawn = sleb(b, off2)
            rec = dict(offset=rec_off, filter=f, next=nxt,
                       raw=hexs(b[rec_off:off3]))
            if f > 0:
                rec["kind"] = "catch"
                rec["ttype_index"] = f
            elif f == 0:
                rec["kind"] = "cleanup"
            else:
                rec["kind"] = "exception-spec"
            action_entries.append(rec)
            off = (off3 + nxt) if nxt else None
    out["action_entries"] = action_entries
    if action_entries:
        actions_end = max(r["offset"] + len(r["raw"]) // 2
                          for r in action_entries)
    else:
        actions_end = action_table_start
    out["action_table_hex"] = hexs(b[action_table_start:actions_end])
    out["action_table_end"] = actions_end

    # ------------------------------------------------------- TType 表（反向）
    ttype_entries = []
    if ttype_enc != DW_EH_PE_omit:
        size = enc_size(ttype_enc)
        if size is None:
            out["ttype_entries"] = []
            return struct_fail("TType 编码 %s 是变长，本工具不支持"
                               % enc_desc(ttype_enc))
        max_idx = max([r["ttype_index"] for r in action_entries
                       if r["kind"] == "catch"] or [0])
        ttype_start = ttype_end - max_idx * size
        out["ttype_table_start"] = ttype_start
        if ttype_start < actions_end:
            return struct_fail(
                "TType 表起点 0x%x 落在 action 表（到 0x%x）里，结构非法"
                % (ttype_start, actions_end))
        out["action_table_gap_hex"] = hexs(b[actions_end:ttype_start])
        for i in range(1, max_idx + 1):
            off = ttype_end - i * size
            raw = b[off:off + size]
            rel = (elf.reloc_at(secidx, base_off + off)
                   if elf is not None else None)
            if elf is not None:
                info = resolve_reloc_chain(
                    elf, secidx, base_off + off,
                    bool(ttype_enc & DW_EH_PE_indirect))
                chain = info["chain"]
                value_symbol = info["target_sym"]
                value_defined = info["target_defined"]
            else:
                chain = "（裸字节模式：无重定位信息）"
                value_symbol = None
                value_defined = None
            ent = dict(index=i, offset=off, raw=hexs(raw),
                       encoding=enc_desc(ttype_enc),
                       reloc=(dict(symbol=elf.symname(rel["symidx"]),
                                   type=rel["rtype"], addend=rel["addend"])
                              if rel is not None and elf is not None
                              else None),
                       chain=chain,
                       value_symbol=value_symbol,
                       value_defined=value_defined)
            if rel is None:
                val = int.from_bytes(raw, "little", signed=True)
                ent["value"] = val
                ent["meaning"] = ("null type_info* => catch(...)（catch-all）"
                                  if val == 0
                                  else "未重定位/原始值 %d" % val)
            else:
                ent["value"] = None
                ent["meaning"] = ("pcrel 到指针槽，槽里是 type_info*：%s"
                                  % (value_symbol or "0"))
            ttype_entries.append(ent)
        out["ttype_entries"] = ttype_entries
        checks.append(dict(
            name="TType 项数 == action 表最大正 filter 下标",
            ok=(len(ttype_entries) == max_idx),
            detail="entries=%d max_filter=%d" % (len(ttype_entries), max_idx)))
        logical_end = ttype_end
    else:
        out["ttype_entries"] = []
        out["ttype_table_start"] = None
        logical_end = actions_end

    # ------------------------------------- 逻辑长度 vs 可用长度（padding）
    out["logical_length"] = logical_end
    trailing = b[logical_end:]
    out["trailing_hex"] = hexs(trailing)
    checks.append(dict(
        name="LSDA 逻辑长度 <= 可用字节数",
        ok=(logical_end <= len(b)),
        detail="logical=0x%x available=0x%x" % (logical_end, len(b))))
    if trailing:
        checks.append(dict(
            name="尾部多余字节是 padding/后续数据（不属于任何表）",
            ok=True,
            detail="%d 字节：%s" % (len(trailing), hexs(trailing))))
    return out


def _print_lsda(lsda, title=""):
    if title:
        print(title)
    print("[LSDA] %s+0x%x，可用 %d 字节：%s" % (
        lsda["section"], lsda["lsda_offset"],
        len(lsda["hex"]) // 2, lsda["hex"]))
    if lsda["structural_error"]:
        print("  *** 结构非法：%s" % lsda["structural_error"])
    print("-" * 78)
    print(" %-6s %-24s %-10s %s" % ("偏移", "字段", "原始字节", "解释"))
    print("-" * 78)
    for fld in lsda.get("fields", []):
        print(" 0x%04x %-24s %-10s %s" % (
            fld["offset"], fld["name"], fld["raw"], fld["note"]))
    print()
    if "ttype_encoding" in lsda:
        if lsda["ttype_encoding"] == DW_EH_PE_omit:
            print(" TType 表：不存在（encoding=0xff）；"
                  "TType 偏移字段%s" % (
                      "存在" if lsda.get("ttype_offset_field_present")
                      else "已正确省略"))
        else:
            print(" TType 表：encoding=0x%02x，offset=0x%x，末尾=0x%x"
                  % (lsda["ttype_encoding"],
                     lsda.get("ttype_table_offset", -1),
                     lsda.get("ttype_table_end", -1)))
    if "call_site_entries" in lsda:
        print(" call-site 表（%d 项，start/len 相对函数起点，"
              "landing pad 相对 LPStart）：" % len(lsda["call_site_entries"]))
        for e in lsda["call_site_entries"]:
            print("   [%d] 原始=%-12s start=0x%-3x(%s) len=0x%-3x(→%s) "
                  "landing_pad=0x%-3x(%s) action=%d" % (
                      e["index"], e["raw"], e["start"], e["start_symbol"],
                      e["length"], e["end_symbol"], e["landing_pad"],
                      e["landing_pad_symbol"], e["action"]))
    print()
    if lsda.get("action_entries"):
        print(" action 表（call-site 引用到的记录，filter/next 均 SLEB128）：")
        for r in lsda["action_entries"]:
            print("   @0x%02x 原始=%-8s filter=%d (%s) next=%d" % (
                r["offset"], r["raw"], r["filter"], r["kind"], r["next"]))
    else:
        print(" action 表：空（没有任何 call-site 引用 action；"
              "action=0 = 没有 action 记录）")
    print()
    tte = lsda.get("ttype_entries") or []
    if tte:
        print(" TType 表（反向索引：第 i 项在 ttype_end - i*size）：")
        for t in tte:
            print("   [%d] @0x%02x raw=%-10s reloc=%s" % (
                t["index"], t["offset"], t["raw"], t["reloc"]))
            print("        %s" % t["meaning"])
    print()
    print(" 尾部/逻辑长度：logical_length=0x%x 可用=0x%x"
          % (lsda.get("logical_length", -1),
             lsda.get("lsda_available_len", len(lsda["hex"]) // 2)))
    if lsda.get("action_table_gap_hex"):
        print(" action 表 → TType 表之间的 padding：%s"
              % lsda["action_table_gap_hex"])
    if lsda.get("trailing_hex"):
        print(" 逻辑表之后的字节（padding / 下一段数据）：%s"
              % lsda["trailing_hex"])
    print()
    print(" 校验：")
    for c in lsda.get("checks", []):
        print("   [%s] %s  (%s)" % ("OK" if c["ok"] else "FAIL",
                                    c["name"], c["detail"]))


def decode(path, json_mode=False):
    elf = ELF(path)
    if elf.e_type != 1:
        raise SystemExit("%s: 只对 .o（ET_REL）做逐字节解码；"
                         "可执行文件请用 readelf/objdump 查看" % path)
    cies, fdes = parse_eh_frame(elf)
    result = dict(path=path, eh_frame=dict(cies=[], fdes=[],), lsda=None,
                  lsdas=[])
    for c in cies.values():
        aug = c["aug"]
        ad = c["aug_data"] or b""
        enc = {}
        q = 0
        if "P" in aug:
            p_enc = ad[q]
            q += 1
            sz = enc_size(p_enc)
            pers_field_off = c["aug_data_off"] + q
            info = resolve_reloc_chain(
                elf, elf.sec(".eh_frame")["index"], pers_field_off,
                bool(p_enc & DW_EH_PE_indirect))
            enc["personality_encoding"] = p_enc
            enc["personality_encoding_desc"] = enc_desc(p_enc)
            enc["personality_field_offset_in_eh_frame"] = pers_field_off
            enc["personality_raw"] = hexs(ad[q:q + sz])
            enc["personality_chain"] = info["chain"]
            enc["personality_symbol"] = info["target_sym"]
            enc["personality_indirect"] = bool(p_enc & DW_EH_PE_indirect)
            enc["personality_defined"] = info["target_defined"]
            q += sz
        for ch in aug:
            if ch == "L":
                enc["lsda_encoding"] = ad[q]
                enc["lsda_encoding_desc"] = enc_desc(ad[q])
                q += 1
            elif ch == "R":
                enc["fde_encoding"] = ad[q]
                enc["fde_encoding_desc"] = enc_desc(ad[q])
                q += 1
        result["eh_frame"]["cies"].append(dict(
            offset=c["offset"], augmentation=aug, version=c["version"],
            code_align=c["code_align"], data_align=c["data_align"],
            return_register=c["retreg"],
            augmentation_data=hexs(ad), augmentation_data_len=len(ad),
            encodings=enc))
    for f in fdes:
        result["eh_frame"]["fdes"].append(dict(
            fde_offset=f["fde_offset"], cie_offset=f["cie_offset"],
            func_start_section_offset=f["func_start_section_offset"],
            func_start_symbol=f["func_start_symbol"],
            range=f["range"],
            lsda_section=f.get("lsda_target_section"),
            lsda_section_offset=f["lsda_section_offset"],
            lsda_symbol=f.get("lsda_target_symbol"),
            lsda_raw=hexs(f.get("lsda_raw", b"")),
        ))
    # 所有带 LSDA 的 FDE 都解码（一个 .o 里可能有多个函数/LSDA）
    lsda_fdes = [f for f in fdes
                 if f.get("lsda_target_section") == ".gcc_except_table"]
    sec_size = elf.sec(".gcc_except_table")["size"]
    offsets = sorted(set(f["lsda_section_offset"] for f in lsda_fdes))
    bound = {}
    for i, off in enumerate(offsets):
        bound[off] = offsets[i + 1] if i + 1 < len(offsets) else sec_size
    for f in lsda_fdes:
        off = f["lsda_section_offset"]
        d = decode_lsda(elf, off, f["func_start_section_offset"], bound[off])
        d["fde_offset"] = f["fde_offset"]
        d["func_start_section_offset"] = f["func_start_section_offset"]
        d["func_start_symbol"] = f["func_start_symbol"]
        d["fde_range"] = f["range"]
        result["lsdas"].append(d)
    if result["lsdas"]:
        result["lsda"] = result["lsdas"][0]

    if json_mode:
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return result

    print("=" * 78)
    print("文件: %s" % path)
    print("=" * 78)
    for c in result["eh_frame"]["cies"]:
        e = c["encodings"]
        print("[.eh_frame] CIE@0x%02x  augmentation=%r  version=%d" %
              (c["offset"], c["augmentation"], c["version"]))
        print("            augmentation data = %s  (%d 字节)" %
              (hexsp(bytes.fromhex(c["augmentation_data"])),
               c["augmentation_data_len"]))
        if "personality_encoding" in e:
            print("            personality 编码 = 0x%02x = %s" %
                  (e["personality_encoding"], e["personality_encoding_desc"]))
            print("            personality 指针 = %s" % e["personality_chain"])
            print("            personality 符号 = %s%s" %
                  (e["personality_symbol"],
                   "（间接：CIE 指向指针槽）"
                   if e["personality_indirect"]
                   else "（直接：pcrel 指向函数）"))
        if "lsda_encoding" in e:
            print("            LSDA 指针编码 = 0x%02x = %s" % (
                e["lsda_encoding"], e["lsda_encoding_desc"]))
        if "fde_encoding" in e:
            print("            FDE 初始地址编码 = 0x%02x = %s" % (
                e["fde_encoding"], e["fde_encoding_desc"]))
    for f in result["eh_frame"]["fdes"]:
        print("[.eh_frame] FDE@0x%02x  函数起点=0x%x (%s)  range=0x%x" %
              (f["fde_offset"], f["func_start_section_offset"],
               f["func_start_symbol"], f["range"]))
        print("            LSDA 指针: %s+0x%x  (raw=%s)" %
              (f["lsda_section"], f["lsda_section_offset"], f["lsda_raw"]))
    if not result["lsdas"]:
        print("（没有解出 LSDA）")
        return result
    for i, lsda in enumerate(result["lsdas"]):
        print()
        _print_lsda(lsda, title="---------- LSDA #%d（FDE@0x%x，函数起点 0x%x"
                                 "，函数长度 0x%x）----------"
                                 % (i + 1, lsda["fde_offset"],
                                    lsda["func_start_section_offset"],
                                    lsda["fde_range"]))
    return result


def decode_raw_hex(hexstr, json_mode=False):
    cleaned = "".join(hexstr.replace("0x", " ").split())
    try:
        data = bytes.fromhex(cleaned)
    except ValueError as ex:
        raise SystemExit("--raw-hex 不是合法十六进制：%s" % ex)
    print("=" * 78)
    print("裸字节 LSDA（%d 字节）：%s" % (len(data), hexsp(data)))
    print("注意：没有 FDE，所以 start/len/landing_pad 的符号标注缺失；"
          "lpstart 按 0 处理（只影响绝对值，不影响格式判定）")
    print("=" * 78)
    lsda = decode_lsda(None, 0, 0, raw_bytes=data)
    if json_mode:
        print(json.dumps(dict(raw_hex=hexs(data), lsda=lsda),
                         ensure_ascii=False, indent=2))
        return lsda
    _print_lsda(lsda)
    return lsda


def main():
    ap = argparse.ArgumentParser(
        description="解码 ELF64 .o 的 .eh_frame/LSDA，或直接解码一段 LSDA 字节")
    ap.add_argument("obj", nargs="?", help="待解码的 .o 文件")
    ap.add_argument("--json", action="store_true", help="输出 JSON")
    ap.add_argument("--raw-hex", metavar="HEX",
                    help="直接解码一段 LSDA 十六进制字节（如 --raw-hex 'ff ff 01 04 ...'）")
    args = ap.parse_args()
    if args.raw_hex is not None:
        decode_raw_hex(args.raw_hex, json_mode=args.json)
        return
    if not args.obj:
        ap.error("需要 .o 文件，或者用 --raw-hex 解码一段裸字节")
    decode(args.obj, json_mode=args.json)


if __name__ == "__main__":
    main()
