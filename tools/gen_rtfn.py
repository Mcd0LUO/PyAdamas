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
        m = re.match(r'RTFN\((\w+),\s*"([^"]+)"\)', line)
        assert m, line
        entries.append((m.group(1), m.group(2)))
    return entries


def render(entries):
    out = [PREAMBLE, 'inline const char* rt_name(RtFn f) {', '    switch (f) {']
    for name, sym in entries:
        out.append('        case RtFn::' + name + ': return "' + sym + '";')
    out.append('    }')
    out.append('    return "?";')
    out.append('}')
    return chr(10).join(out) + chr(10)


def main():
    text = render(load())
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