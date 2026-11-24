# string.Template._substitute 优化回归：长模板、多次占位符、$$、safe/strict。
import string

parts: list[str] = []
i = 0
while i < 500:
    parts.append("word $name and $other $$ ")
    i += 1
txt = "".join(parts)
t = string.Template(txt)
m = {"name": "ABC", "other": "XYZ"}
out = t.safe_substitute(m)
print(len(out))
print(out[:36])
print(out[-36:])

# 同一 mapping 被多个占位符复用，hoist items 后结果不变
t2 = string.Template("$a-$b-$a-$c")
print(repr(t2.substitute({"a": "1", "b": "2", "c": "3"})))
print(repr(t2.safe_substitute({"a": "1"})))
# --- expect ---
# 9500
# word ABC and XYZ $ word ABC and XYZ 
# rd ABC and XYZ $ word ABC and XYZ $ 
# '1-2-1-3'
# '1-$b-1-$c'
