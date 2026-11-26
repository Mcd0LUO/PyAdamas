# 0.567：PEP 750 t-string —— 结构化模板（Template / Interpolation）。
name = "world"
t = t"Hello {name}!"

print(type(t))
print(t.strings)
print(len(t.interpolations))

i = t.interpolations[0]
print(i.value, i.expression)
print(i.conversion, i.format_spec)
print(t.values)

# 空模板
e = t""
print(e.strings, len(e.interpolations))

# 格式说明与转换标志会被保留
x = 42
f = t"{x!r:>6}"
fi = f.interpolations[0]
print(fi.expression, fi.conversion, fi.format_spec)

# --- expect ---
# <class 'Template'>
# ['Hello ', '!']
# 1
# world name
#  
# ['world']
# [''] 0
# x r >6
