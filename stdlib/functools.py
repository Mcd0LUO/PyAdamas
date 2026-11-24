"""functools 的 stdlib 扩展层（原生模块 + Python 补丁）。

PyAdamas 的 functools 原生实现（typecheck/builtin_sigs.cpp 签名 + 降级里的特化）提供
reduce / wraps / partial / lru_cache；本文件只补原生没有的 CPython 接口。
导入时编译器把扩展源编成模块，接口与原生接口合并：原生名字优先，本文件里
原生没有的名字作为扩展全局导出（见 src/driver/build.cpp 的 kModuleTable）。
往 functools 补 API 只需要改这个文件，不用碰 src/。

语义边界（PyAdamas 不建模运行期函数元数据与缓存）：
- WRAPPER_ASSIGNMENTS / WRAPPER_UPDATES：与 CPython 相同的常量。
- update_wrapper：返回 wrapper 本身；PyAdamas 里函数的 __name__/__doc__
  不可见（同原生 wraps 的「装饰器整层跳过」），这是等价语义。
- cache：等价恒等装饰器。返回值语义与 CPython 一致，但不缓存结果；需要真
  缓存请用原生 lru_cache(maxsize=None)。
- cmp_to_key：返回一个可作 sorted(key=...) 的 key 函数。

  CPython 的实现是返回一个包装对象、靠其 __lt__ 调 cmp(a,b)；
  PA 的 sorted(key=...) 先算完所有 key 再按 key 的机器字比较，
  完全不走 key 对象的 __lt__，所以无法照搬。这里改用「可变 key +
  增量排名」：key 函数每次被调用时把新值加入模块级 seen 列表，
  用 cmp 对所有 seen 值做插入排序，再把每个已发出的 key list 的
  第 0 项更新为其在排序结果中的名次。key 本身是 list[int]，PA
  按字典序比较，因此 sorted(key=...) 得到与 cmp 一致的顺序。

  代价与边界：
  1. 每次调用 cmp_to_key 会重置全局状态，所以同一时刻只能有一个
     活跃的 cmp_to_key 生成的 key；不支持嵌套/交叉使用两个 key。
  2. sorted 的 key 阶段会调用 key N 次，每次都做 O(N log N) 的插入
     排序，总复杂度 O(N^2)；CPython 是 O(N log N) 次 cmp 调用。
  3. key 返回的是 `list[int]` 而非 CPython 的 K 实例；直接检查
     key 的类型或复用 key 跨 sorted 调用都属于未定义行为。
  4. 只支持 list/str/int 等 PA 可比较元素；对不支持 < 的未知对象，
     cmp 自行负责给出一致的比较结果。
"""

WRAPPER_ASSIGNMENTS = ("__module__", "__name__", "__qualname__", "__annotations__", "__doc__")
WRAPPER_UPDATES = ("__dict__",)


def update_wrapper(wrapper, wrapped):
    return wrapper


def cache(func):
    return func


_cmp_values: list = []
_cmp_keys: list = []
_cmp_func: object = None


def cmp_to_key(cmp):
    """把比较函数 cmp(a,b)->int 转成可用于 sorted(key=...) 的 key 函数。

    结果与 CPython functools.cmp_to_key 一致：`sorted(xs, key=cmp_to_key(cmp))`
    等价于按 cmp 定义的次序排列 xs。详见模块 docstring 的语义边界。
    """
    global _cmp_values, _cmp_keys, _cmp_func
    _cmp_values = []
    _cmp_keys = []
    _cmp_func = cmp

    def key(v: object) -> list:
        _cmp_values.append(v)
        k: list = [0]
        _cmp_keys.append(k)
        # 用 cmp 对所有已见值做插入排序，得到当前总次序
        order: list = []
        i: int = 0
        while i < len(_cmp_values):
            x = _cmp_values[i]
            j: int = 0
            while j < len(order):
                if _cmp_func(x, order[j]) < 0:
                    break
                j += 1
            order.insert(j, x)
            i += 1
        # 回填每个已发出 key 的名次（key 是 list[int]，PA 按字典序比较）
        i = 0
        while i < len(_cmp_keys):
            _cmp_keys[i][0] = order.index(_cmp_values[i])
            i += 1
        return k

    return key
