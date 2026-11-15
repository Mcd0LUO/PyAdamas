"""collections 包在 pyadamas 里只是 collections.abc 的宿主。

真正可用的 collections 模块是编译期内建（Counter/deque/defaultdict/
OrderedDict），`import collections` 直接走内建，不加载本包。本包只让
`from collections.abc import ...` 能解析到子模块。
"""
