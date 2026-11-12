#!/usr/bin/env python3
"""把一个 header-only 的类拆成 .h（声明）+ .cpp（定义）。

保留规则（宁可少拆，不可拆错）：
  - 函数体与签名同行（{ ... } 或 {}）→ 保持内联
  - 函数体不超过 4 行 → 保持内联（这些多在热路径，跨 TU 无法内联）
  - 模板成员 → 必须留在头里
其余才移到 .cpp。

用法: split_class.py <src.h> <ClassName> <out.h> <out.cpp>
"""
import re
import sys


def split_class(src_path, cls, hdr_path, cpp_path, includes):
    src = open(src_path, encoding='utf-8').read()

    m = re.search(r'^class %s \{' % re.escape(cls), src, re.M)
    if not m:
        sys.exit(f'class {cls} not found in {src_path}')
    brace = src.index('{', m.start())
    end = src.find('\n};', brace)
    if end < 0:
        sys.exit('class body end not found')

    head = src[:m.start()]
    body = src[brace + 1:end]
    tail = src[end + 3:]

    header_members = []
    cpp_defs = []
    funcs = []

    lines = body.split('\n')
    k = 0
    while k < len(lines):
        start_k = k
        line = lines[k]
        st = line.strip()
        is_top = line.startswith('    ') and not line.startswith('     ')

        # 向后探测签名结束：出现 '{' 或 ';' 为止
        probe = st
        sig_end = k
        while '{' not in probe and ';' not in probe and sig_end + 1 < len(lines):
            sig_end += 1
            probe += ' ' + lines[sig_end].strip()

        prev = lines[k - 1].strip() if k > 0 else ''
        is_template = prev.startswith('template')
        spans = probe.count('{') > probe.count('}')
        is_func = (is_top and st and not st.startswith('//') and '(' in st
                   and '{' in probe and spans and not is_template
                   and ';' not in probe.split('{')[0])

        if not is_func:
            header_members.append(line)
            k += 1
            continue

        raw_sig = ' '.join(l.strip() for l in lines[start_k:sig_end + 1])

        # 收集完整函数体
        blk = [raw_sig]
        k = sig_end + 1
        while k < len(lines):
            blk.append(lines[k])
            if lines[k].startswith('    }') and not lines[k].startswith('     '):
                k += 1
                break
            k += 1

        # 短函数留在头里
        if len(blk) <= 4:
            header_members.extend(lines[start_k:k])
            continue

        decl = raw_sig
        if decl.endswith('{'):
            decl = decl[:-1].rstrip()
        decl = re.sub(r'\)\s*:\s*[^{]*$', ')', decl).rstrip()
        if not decl.endswith(';'):
            decl += ';'
        header_members.append('    ' + decl)

        d = re.sub(r'^explicit\s+', '', raw_sig)
        d = re.sub(r'^static\s+', '', d)
        d = re.sub(r'\s=\s(?!=)[^,)]+(?=[,)])', '', d)

        paren = d.index('(')
        before = d[:paren]
        name_m = re.search(r'([A-Za-z_]\w*)$', before)
        if not name_m:
            sys.exit(f'cannot locate function name in: {raw_sig}')
        fname = name_m.group(1)
        d = before[:name_m.start()] + f'{cls}::{fname}' + d[paren:]

        cpp_defs.append(d + '\n' + '\n'.join(blk[1:]))
        funcs.append((fname, len(blk)))

    hdr = head + 'class ' + cls + ' {' + '\n'.join(header_members) + '\n};\n' + tail

    inc = '\n'.join(f'#include {i}' for i in includes)
    cpp = (f'// 由 {src_path} 拆出的实现。声明见 {hdr_path}。\n'
           f'{inc}\n\n'
           f'namespace pyadamas {{\n\n'
           + '\n\n'.join(cpp_defs) +
           '\n\n}  // namespace pyadamas\n')

    open(hdr_path, 'w', encoding='utf-8').write(hdr)
    open(cpp_path, 'w', encoding='utf-8').write(cpp)
    return funcs


if __name__ == '__main__':
    src_path, cls, hdr_path, cpp_path = sys.argv[1:5]
    fs = split_class(src_path, cls, hdr_path, cpp_path, [f'"{src_path.split("/")[-1]}"'])
    print(f'{len(fs)} members moved:')
    for n, ln in fs:
        print(f'  {ln:4d}  {n}')
