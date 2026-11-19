# NEW BUG N10 (crash): list in-place += and *= segfault (exit 139).
# check/build pass; runtime SIGSEGV. All element types, literal or variable RHS.
# CPython: [1,2,3,4] / [1,2,1,2]. pyadamas 0.311: Segmentation fault.
# Suspect: AugAssign-on-list lowering emits an unsupported concat/repeat path
# (explicit list+list is a codegen error) or mishandles temp/RC ownership.
# Workaround: list.extend(...) / append loop / comprehension reassignment.
l: list[int] = [1, 2]
l += [3, 4]          # SIGSEGV here
print("iadd:", l)
# Also crashes (uncomment to verify):
# m: list[int] = [1, 2]
# m *= 2
