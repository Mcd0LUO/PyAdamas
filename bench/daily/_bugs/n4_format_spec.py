# NEW BUG N4 (silent wrong): format spec ',' thousands grouping is ignored for
# FLOAT specs (format(x, ",.2f") and f"{x:,.2f}"); int grouping works.
# '#' alternate form is ignored everywhere (format(255,"#x"), f"{255:#x/#o/#b}").
# CPython: '1,234.57' / '0xff'. pyadamas: '1234.57' / 'ff'.
# Suspect: float format path drops ',' when precision/type present; '#' unimplemented.
# Workaround: avoid ',' on floats and '#' alternate form.
print("fmt comma float:", format(1234.5678, ",.2f"))   # CPython 1,234.57, pa 1234.57
print("fstr comma float:", f"{1234.5678:,.2f}")        # CPython 1,234.57, pa 1234.57
print("fmt comma int:", format(1234567, ","))          # matches 1,234,567
print("fmt hash:", format(255, "#x"), f"{255:#x}")     # CPython 0xff 0xff, pa ff ff
print("fstr hash oct bin:", f"{255:#o}", f"{255:#b}")  # CPython 0o377 0b11111111, pa 377 11111111
