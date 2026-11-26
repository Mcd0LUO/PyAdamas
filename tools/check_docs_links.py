#!/usr/bin/env python3
'''检查 docs/ 下的相对 Markdown 链接是否指向真实文件。

只查「文件是否存在」，不解析锚点；跳过 http(s)/mailto/纯锚点，
以及**代码片段**里的括号（`[0](a)`、`list[int](pyobj)` 这类 C++/伪代码会误报）。
路径按「相对于该 md 文件所在目录」解析，也接受「相对于仓库根」的写法。
'''
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(ROOT, 'docs')
LINK = re.compile(r'\[[^\]]*\]\(([^)\s]+)\)')
FENCE = re.compile(r'```.*?```', re.S)
INLINE = re.compile(r'`[^`\n]*`')

def strip_code(text: str) -> str:
    # 用等长空白替换，保持行号不变
    def blank(m):
        return re.sub(r'[^\n]', ' ', m.group(0))
    text = FENCE.sub(blank, text)
    return INLINE.sub(blank, text)

def main() -> int:
    bad = []
    total = 0
    for dirpath, dirnames, files in os.walk(DOCS):
        # docs/archive/ 是冻结的历史归档，其中的失效链接是历史事实，不再修。
        if os.path.basename(dirpath) == 'archive':
            dirnames[:] = []
            continue
        for fn in sorted(files):
            if not fn.endswith('.md'):
                continue
            full = os.path.join(dirpath, fn)
            with open(full, encoding='utf-8') as f:
                raw = f.read()
            text = strip_code(raw)
            for m in LINK.finditer(text):
                target = m.group(1)
                if target.startswith(('http://', 'https://', 'mailto:', '#')):
                    continue
                target = target.split('#', 1)[0]
                if not target:
                    continue
                # 裸词（无扩展名、无路径分隔符）几乎都是正文里的伪代码，例如
                # 「list[int](pyobj)」，不是链接。
                if '.' not in target and '/' not in target:
                    continue
                total += 1
                rel_to_md = os.path.normpath(os.path.join(dirpath, target))
                rel_to_root = os.path.normpath(os.path.join(ROOT, target))
                if os.path.exists(rel_to_md) or os.path.exists(rel_to_root):
                    continue
                line = text[:m.start()].count(chr(10)) + 1
                bad.append((os.path.relpath(full, ROOT), line, target))
    for path, line, target in bad:
        print(f'{path}:{line}: 链接指向不存在的文件: {target}')
    print(f'检查 {total} 个相对链接，{len(bad)} 个失效')
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main())