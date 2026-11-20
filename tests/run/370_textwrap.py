# 370_textwrap —— stdlib/textwrap.py 与 CPython 对拍（折行/缩进/截断）
#
# 覆盖面：wrap/fill/shorten/dedent/indent、TextWrapper 类（属性 + 私有钩子
# _munge_whitespace/_split/_wrap_chunks/_handle_long_word、子类覆写 wrap），
# 参数面 width/initial_indent/subsequent_indent/expand_tabs/tabsize/
# replace_whitespace/fix_sentence_endings/break_long_words/drop_whitespace/
# break_on_hyphens/max_lines/placeholder，以及 ValueError 口径。
# 期望输出由 CPython 3.14.4 textwrap 逐行生成（见文件尾）。
import textwrap

TEXT = "the quick brown fox jumps over the lazy dog"

print(textwrap.wrap(TEXT, 15))
print(textwrap.fill(TEXT, 15))
print(repr(textwrap.fill(TEXT, 12, initial_indent="> ",
                        subsequent_indent=".. ")))

# 连字符 / em-dash 断点
print(textwrap.wrap("one-two-three-four-five", 8))
print(textwrap.wrap("one-two-three-four-five", 8, break_on_hyphens=False))
print(textwrap.wrap("Hello there -- you goof-ball, use the -b option!", 20))
print(textwrap.wrap("a-bc ab-c a-b-c x-y-z", 10))
print(textwrap.wrap("foo---bar one--two", 12))

# 超长词与 break_long_words
print(textwrap.wrap("averyveryverylongword", 5))
print(textwrap.wrap("averyveryverylongword", 5, break_long_words=False))
print(textwrap.wrap("supercalifragilistic-expialidocious", 10, max_lines=1,
                    placeholder="..."))

# 空白：tab 展开 / 空白替换 / 丢弃
print(repr(textwrap.fill("a\tb\n\nc", 5)))
print(textwrap.wrap("a\tb", 3, expand_tabs=False))
print(textwrap.wrap("a\tb", 3, expand_tabs=False, replace_whitespace=False))
print(textwrap.wrap("  a  b  ", 5))
print(textwrap.wrap("  a  b  ", 5, drop_whitespace=False))
print(textwrap.wrap("a\tb", 3, tabsize=4))

# max_lines / placeholder
print(textwrap.wrap("foo bar baz qux", 10, max_lines=2))
print(textwrap.wrap("foo bar baz", 5, max_lines=1))
print(textwrap.wrap("foo bar baz", 5, max_lines=1, placeholder="."))
print(textwrap.wrap("x " * 5, 4))
print(repr(textwrap.fill("aa bb cc dd ee", 5, initial_indent="[",
                         subsequent_indent="]")))

# fix_sentence_endings
print(repr(textwrap.fill("Hello. World. Again.", 20,
                         fix_sentence_endings=True)))
print(repr(textwrap.fill("Hello. World. Again.", 20)))

# shorten
print(repr(textwrap.shorten("Hello  world!", 12)))
print(repr(textwrap.shorten("Hello  world!", 11)))
print(repr(textwrap.shorten("Hello  world!", 11, placeholder="...")))
print(repr(textwrap.shorten("   ", 5)))
print(repr(textwrap.shorten("a b c d e f", 9)))

# 空文本
print(textwrap.wrap("", 10), repr(textwrap.fill("", 10)))
print(textwrap.wrap("   ", 10))

# dedent
print(repr(textwrap.dedent("    a\n      b\n")))
print(repr(textwrap.dedent("  a\n  b")))
print(repr(textwrap.dedent(" \n  \n ")))
print(repr(textwrap.dedent("\ta\n\t\tb")))
print(repr(textwrap.dedent("  a\n\tb")))
print(repr(textwrap.dedent("")))

# indent
print(repr(textwrap.indent("a\n\nb", "> ")))
print(repr(textwrap.indent("a\n\n b\n  \nc", "> ")))
print(repr(textwrap.indent("a\r\n b\rc\nd", "> ")))
print(repr(textwrap.indent("a\nb", "")))
print(repr(textwrap.indent("a\n\nb", "  ", lambda line: len(line) > 2)))

# TextWrapper 属性与私有钩子
w = textwrap.TextWrapper(width=20)
print(w.width, repr(w.initial_indent), repr(w.subsequent_indent))
print(w.expand_tabs, w.replace_whitespace, w.fix_sentence_endings)
print(w.break_long_words, w.drop_whitespace, w.break_on_hyphens, w.tabsize)
print(repr(w.placeholder))
print(repr(w._munge_whitespace("a\tb")))
print(w._split("goof-ball -- x"))
print(w._split_chunks("a b"))
print(w._wrap_chunks(["ab", " ", "cd"]))
print(w.fill("one two three four five"))
w2 = textwrap.TextWrapper(width=12, initial_indent="  ", subsequent_indent="    ")
print(repr(w2.fill("one two three four five")))


class Bracketed(textwrap.TextWrapper):
    def wrap(self, text: str) -> list[str]:
        lines = super().wrap(text)
        out: list[str] = []
        for l in lines:
            out.append("[" + l + "]")
        return out


b = Bracketed(width=9)
print(b.wrap("one two three four"))
print(repr(b.fill("one two three four")))

# ValueError 口径
try:
    textwrap.wrap("x", 0)
except ValueError as e:
    print("ValueError:", e)
try:
    textwrap.shorten("hello", 2)
except ValueError as e:
    print("ValueError:", e)
# --- expect ---
# ['the quick brown', 'fox jumps over', 'the lazy dog']
# the quick brown
# fox jumps over
# the lazy dog
# '> the quick\n.. brown fox\n.. jumps\n.. over the\n.. lazy dog'
# ['one-two-', 'three-', 'four-', 'five']
# ['one-two-', 'three-fo', 'ur-five']
# ['Hello there -- you', 'goof-ball, use the', '-b option!']
# ['a-bc ab-c', 'a-b-c', 'x-y-z']
# ['foo---bar', 'one--two']
# ['avery', 'veryv', 'erylo', 'ngwor', 'd']
# ['averyveryverylongword']
# ['...']
# 'a\nb  c'
# ['a b']
# ['a\tb']
# ['  a', 'b']
# ['  a  ', 'b  ']
# ['a', 'b']
# ['foo bar', 'baz qux']
# ['[...]']
# ['foo.']
# ['x x', 'x x', 'x']
# '[aa\n]bb\n]cc\n]dd\n]ee'
# 'Hello.  World.\nAgain.'
# 'Hello. World. Again.'
# 'Hello world!'
# 'Hello [...]'
# 'Hello...'
# ''
# 'a b [...]'
# [] ''
# []
# 'a\n  b\n'
# 'a\nb'
# '\n\n'
# 'a\n\tb'
# '  a\n\tb'
# ''
# '> a\n\n> b'
# '> a\n\n>  b\n  \n> c'
# '> a\r\n>  b\r> c\n> d'
# 'a\nb'
# 'a\n\nb'
# 20 '' ''
# True True False
# True True True 8
# ' [...]'
# 'a       b'
# ['goof-', 'ball', ' ', '--', ' ', 'x']
# ['a', ' ', 'b']
# ['ab cd']
# one two three four
# five
# '  one two\n    three\n    four\n    five'
# ['[one two]', '[three]', '[four]']
# '[one two]\n[three]\n[four]'
# ValueError: invalid width 0 (must be > 0)
# ValueError: placeholder too large for max width
