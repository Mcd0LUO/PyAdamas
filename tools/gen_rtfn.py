#!/usr/bin/env python3
"""从 src/ir/rtfn.def 生成 src/ir/rtfn_names.h（RtFn 名字表的唯一来源）。

用法：python3 tools/gen_rtfn.py          # 重新生成
      python3 tools/gen_rtfn.py --check  # 只校验生成物是否最新（CI 用）
"""
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEF = ROOT / 'src/ir/rtfn.def'
OUT = ROOT / 'src/ir/rtfn_names.h'

PREAMBLE = '// Runtime symbol names; included inside namespace pyadamas by ir/rtfn.h.'


def load():
    entries = []
    for line in DEF.read_text(encoding='utf-8').splitlines():
        line = line.strip()
        if not line.startswith('RTFN('):
            continue
        m = re.match(r'RTFN\((\w+),\s*"([^"]+)"(?:,\s*(\w+))?\)', line)
        assert m, line
        entries.append((m.group(1), m.group(2), m.group(3) or 'custom'))
    return entries


def render(entries):
    out = [PREAMBLE, 'inline const char* rt_name(RtFn f) {', '    switch (f) {']
    for name, sym, _g in entries:
        out.append('        case RtFn::' + name + ': return "' + sym + '";')
    out.append('    }')
    out.append('    return "?";')
    out.append('}')
    return chr(10).join(out) + chr(10)




def enum_names():
    """解析 src/ir/rtfn.h 的枚举成员（一行可能多个，注释去掉）。"""
    text = (ROOT / 'src/ir/rtfn.h').read_text(encoding='utf-8')
    m = re.search(r'enum class RtFn : uint16_t \{(.*?)\n\};', text, re.S)
    assert m, 'rtfn.h 里找不到 enum class RtFn'
    body = m.group(1)
    body = re.sub(r'//[^\n]*', '', body)
    names = []
    for item in body.split(','):
        item = item.strip()
        if not item:
            continue
        mm = re.match(r'^([A-Z][A-Za-z0-9]*)$', item)
        assert mm, '无法解析的枚举项: ' + item
        names.append(mm.group(1))
    return names


def check_enum():
    """枚举顺序必须与 rtfn.def 完全一致：名字表按枚举值索引，错位即静默错名。"""
    want = [e[0] for e in load()]
    have = enum_names()
    if want != have:
        print('rtfn.h 与 rtfn.def 不一致：', file=sys.stderr)
        for i, (a, b) in enumerate(zip(want, have)):
            if a != b:
                print('  第 ' + str(i) + ' 项：def=' + a + ' enum=' + b, file=sys.stderr)
                break
        print('  def ' + str(len(want)) + ' 项，enum ' + str(len(have)) + ' 项', file=sys.stderr)
        return 1
    print('rtfn.h 与 rtfn.def 一致（' + str(len(want)) + ' 项）')
    return 0



def check_groups():
    """call_rt.cpp 的 case → 编组必须与 rtfn.def 一致（编组错 = 参数传递错，静默错值）。"""
    src = (ROOT / 'src/backend/call_rt.cpp').read_text(encoding='utf-8')
    i = src.index('void AsmGen::gen_rt_args(')
    j = src.index(chr(10) + '}', src.index('switch (fn) {', i))
    labels = []
    pairs = {}
    for line in src[i:j].split(chr(10)):
        ls = line.strip()
        m = re.match(r'case RtFn::(\w+):', ls)
        if m:
            labels.append(m.group(1))
            continue
        g = re.search(r'gen_rt_(i[1-6]|f[1-3]|none)\s*\(', ls)
        if g and labels:
            for l in labels:
                pairs[l] = g.group(1)
            labels = []
    bad = []
    for name, _sym, grp in load():
        want = pairs.get(name, 'custom')
        if grp != want:
            bad.append((name, grp, want))
    if bad:
        print('rtfn.def 的编组与 call_rt.cpp 不一致：', file=sys.stderr)
        for n, a, b in bad[:5]:
            print('  ' + n + '：def=' + a + ' call_rt=' + b, file=sys.stderr)
        return 1
    print('rtfn.def 编组与 call_rt.cpp 一致（' + str(len(pairs)) + ' 项）')
    return 0

def main():
    text = render(load())
    if '--check-groups' in sys.argv:
        return check_groups()
    if '--check-enum' in sys.argv:
        return check_enum()
    if '--check' in sys.argv:
        cur = OUT.read_text(encoding='utf-8') if OUT.exists() else ''
        if cur != text:
            print('rtfn_names.h 与 rtfn.def 不一致，请运行 tools/gen_rtfn.py', file=sys.stderr)
            return 1
        print('rtfn_names.h 与 rtfn.def 一致')
        return 0
    OUT.write_text(text, encoding='utf-8')
    print('generated', OUT)
    return 0


if __name__ == '__main__':
    sys.exit(main())