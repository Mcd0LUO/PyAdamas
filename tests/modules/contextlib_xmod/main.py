# 0.540：跨模块 @contextmanager / 跨模块类引用（tag 29）。
import cmmod

with cmmod.managed('x') as v:
    print('body', v)

with cmmod.make_null(7) as w:
    print('null', w)

# --- expect ---
# enter x
# body 1
# exit x
# null 7
