# shlex 引号/切分往返口径（CPython 实跑取期望）。
import shlex

words = ["", "plain", "a b", "it's", "a'b'c", "~$#", "a\nb", "\t",
         "\u03b1\u03b2", "a\\b", "-x=1", "a  b"]
for w in words:
    q = shlex.quote(w)
    ok = "|".join(shlex.split(q))
    print(q, "=>", len(shlex.split(q)), ok == w)
print("|".join(shlex.join(words).split(" ")) == "|".join(words))
for bad in ["a 'b", "a \"b", "a \\"]:
    try:
        print(len(shlex.split(bad)))
    except ValueError as e:
        print("ValueError", str(e))

# --- expect ---
# '' => 1 True
# plain => 1 True
# 'a b' => 1 True
# 'it'"'"'s' => 1 True
# 'a'"'"'b'"'"'c' => 1 True
# '~$#' => 1 True
# 'a
# b' => 1 True
# '	' => 1 True
# 'αβ' => 1 True
# 'a\b' => 1 True
# -x=1 => 1 True
# 'a  b' => 1 True
# False
# ValueError No closing quotation
# ValueError No closing quotation
# ValueError No escaped character
