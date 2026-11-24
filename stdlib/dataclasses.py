"""dataclasses —— @dataclass 装饰器（P4 块4，PYADAMAS_LANG_GAPS）。

pa 的 @dataclass 是**编译期展开**：类型检查阶段按字段注解合成
__init__/__repr__/__eq__ 注入类体（src/typecheck/dataclass.cpp），运行期
这个装饰器只是恒等函数（门控开时降级器甚至跳过调用）。门控关时导入可用、
装饰器恒等，但带参构造不合成 __init__（与基线 @dataclass 口径一致）。

v1 范围（docs/lang-gaps-landing.md §8 块4）：单类 + 字面量默认值 +
int/float/bool/str/bytes/list/dict/tuple/set 字段。继承 dataclass /
field() / frozen / order / kw_only / slots / unsafe_hash → 门控开时编译期
明确报错。field() 运行期兜底抛 NotImplementedError（正常路径编译期已拦）。
"""


def dataclass(cls: unknown = None, **kwargs: object) -> unknown:
    """恒等装饰器：真实展开在编译期完成。

    0.484：签名要接受 CPython 的那套关键字参数（frozen/order/kw_only/...），
    否则 check 一加载模块接口就会先报 "unexpected keyword argument 'frozen'"，
    把 lang-gaps 展开路径里更准确的 "decorator option 'frozen' is not supported"
    盖掉（tests/fail/352..354）。选项的合法性由 typecheck/dataclass.cpp 判。
    """
    return cls


def field(*args, **kwargs) -> unknown:
    """v1 不支持 field() 描述符（编译期已拦截；这里是运行期兜底）。"""
    raise NotImplementedError("dataclasses.field() is not supported by pyadamas lang-gaps v1")
