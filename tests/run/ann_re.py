import re

r: re.Pattern = re.compile('a+')
print(r.pattern)
m: re.Match = r.search('baa')
print(m.group(0))

# --- expect ---
# a+
# aa
