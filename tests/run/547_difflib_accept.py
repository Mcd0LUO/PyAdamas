# difflib 常用 API 对拍；PA unified_diff 返回 list，CPython 返回生成器，用 list() 对齐。
import difflib
print("ratio", difflib.SequenceMatcher(None, "abc", "abd").ratio())
print("ratio2", difflib.SequenceMatcher(None, "hello", "hello").ratio())
d = list(difflib.unified_diff(["a\n","b\n"], ["a\n","c\n"], lineterm=""))
print("diff", d)

# --- expect ---
# ratio 0.6666666666666666
# ratio2 1.0
# diff ['--- ', '+++ ', '@@ -1,2 +1,2 @@', ' a\n', '-b\n', '+c\n']
