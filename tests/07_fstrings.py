# f-string：PEP 701 与转义语义
#
# CPython 3.11 只接受旧规则，所以这份文件不能用 cp3.11 对照，
# 它是按 PEP 701 写的：表达式中可以复用外层引号、可以嵌套 f-string。

name = "world"
width = 8
value = 3.14159

basic = f"hello {name}"
nested_quotes = f"outer {"inner"} tail"          # PEP 701：同引号嵌套
nested_fstring = f"level1 {f"level2 {name}"}"
self_doc = f"{width=}"                            # f"{x=}"
self_doc_spec = f"{value=:.2f}"
spec_nested = f"{value:{width}.{width // 4}f}"     # format spec 里的嵌套替换
spec_literal = f"{name:>{width}}"
conv = f"{name!r:>10}"
braces = f"{{literal}} {name}"
multiline = f"""first {name}
second {width + 1}"""

# raw f-string：字面量段的反斜杠保持原样，替换段照常求值
raw_f = rf"path\n{name}\t"
plain_f = f"path\n{name}\t"

# 反斜杠在替换表达式里（PEP 701 允许）
escaped_in_expr = f"{'a' + 'b'}\n"

print(basic, nested_quotes, nested_fstring, self_doc, self_doc_spec)
print(spec_nested, spec_literal, conv, braces, multiline)
print(raw_f, plain_f, escaped_in_expr)
