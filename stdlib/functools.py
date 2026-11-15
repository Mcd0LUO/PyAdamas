"""functools 的 stdlib 扩展层（原生模块 + Python 补丁）。

PyAdamas 的 functools 原生实现（builtin_sigs.cpp 签名 + 降级里的特化）提供
reduce / wraps / partial / lru_cache；本文件只补原生没有的 CPython 接口。
导入时编译器把扩展源编成模块，接口与原生接口合并：原生名字优先，本文件里
原生没有的名字作为扩展全局导出（见 src/driver_build.cpp 的 kModuleTable）。
往 functools 补 API 只需要改这个文件，不用碰 src/。

语义边界（PyAdamas 不建模运行期函数元数据与缓存）：
- WRAPPER_ASSIGNMENTS / WRAPPER_UPDATES：与 CPython 相同的常量。
- update_wrapper：返回 wrapper 本身；PyAdamas 里函数的 __name__/__doc__
  不可见（同原生 wraps 的「装饰器整层跳过」），这是等价语义。
- cache：等价恒等装饰器。返回值语义与 CPython 一致，但不缓存结果；需要真
  缓存请用原生 lru_cache(maxsize=None)。
"""

WRAPPER_ASSIGNMENTS = ("__module__", "__name__", "__qualname__", "__annotations__", "__doc__")
WRAPPER_UPDATES = ("__dict__",)


def update_wrapper(wrapper, wrapped):
    return wrapper


def cache(func):
    return func
