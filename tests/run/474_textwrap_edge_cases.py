# textwrap 边界对拍：宽/缩进交互、shorten/fill 的 max_lines/placeholder（期望取自 CPython）。
import textwrap
for w in [1, 5, 10]:
    for ind in ["", "    ", "          "]:
        out = textwrap.wrap("a bb ccc dddd eeeee", w, initial_indent=ind, subsequent_indent=ind)
        print(w, len(ind), out)
for w in [5, 10, 20]:
    print(w, textwrap.shorten("a bb ccc dddd eeeee", w, placeholder="..."))
print(repr(textwrap.fill("hello world", 5, max_lines=1, placeholder="...")))
# --- expect ---
# 1 0 ['a', 'b', 'b', 'c', 'c', 'c', 'd', 'd', 'd', 'd', 'e', 'e', 'e', 'e', 'e']
# 1 4 ['    a', '    b', '    b', '    c', '    c', '    c', '    d', '    d', '    d', '    d', '    e', '    e', '    e', '    e', '    e']
# 1 10 ['          a', '          b', '          b', '          c', '          c', '          c', '          d', '          d', '          d', '          d', '          e', '          e', '          e', '          e', '          e']
# 5 0 ['a bb', 'ccc', 'dddd', 'eeeee']
# 5 4 ['    a', '    b', '    b', '    c', '    c', '    c', '    d', '    d', '    d', '    d', '    e', '    e', '    e', '    e', '    e']
# 5 10 ['          a', '          b', '          b', '          c', '          c', '          c', '          d', '          d', '          d', '          d', '          e', '          e', '          e', '          e', '          e']
# 10 0 ['a bb ccc', 'dddd eeeee']
# 10 4 ['    a bb', '    ccc', '    dddd', '    eeeee']
# 10 10 ['          a', '          b', '          b', '          c', '          c', '          c', '          d', '          d', '          d', '          d', '          e', '          e', '          e', '          e', '          e']
# 5 a...
# 10 a bb...
# 20 a bb ccc dddd eeeee
# '...'
