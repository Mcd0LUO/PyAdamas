"""traceback —— 异常栈格式化（P4 块3，PYADAMAS_LANG_GAPS，纯 pyadamas 源码）。

sys.exc_info() 取当前异常上下文，__pa_tb_frames(tb) 物化帧链
（list[(file, line, func)]，外→内），源行运行期读文件 + 模块级缓存。

CPython 对拍口径（docs/lang-gaps-landing.md §8 块3）：
- 与 CPython 3.14 **PYTHONNODEBUGRANGES=1**（-X no_debug_ranges）逐字节
  一致：3.13+ 默认给调用行加 ~~~^^^ 锚点，pa 的调试表只有行号没有列范围，
  锚点场景文档化边界（显式 raise 行本身无锚点，调用行锚点不复刻）。
- 帧函数名：<module>（模块体）/ 裸函数名 / 裸方法名（codegen 门控开时
  填 debug_name）；内部包装帧（构造/绑定方法）不进栈。
- 门控关时帧捕获不启用：exc_info 仍返回 (类值, 异常值, tb)，但 tb 帧链
  为空——format_exc 只有头尾没有帧（与 suppress 的门控语义同口径）。
- format_exc 只格式化当前异常；__cause__/__context__ 链（raise ... from /
  处理中再抛）的递归格式化不在本轮（ExcValue 带 cause 但 tb 链只有一份）。
- print_exc 写 sys.stderr；extract_tb 返回 (file, line, func, src) 四元组
  列表（CPython 是 FrameSummary 对象，pa 没有运行期属性面，元组口径）。
"""
import sys

_src_cache: dict[str, list[str]] = {}


def _source_line(filename: str, lineno: int) -> str:
    """读源文件第 lineno 行（strip 后）；读不到返回空串（CPython 同口径）。"""
    if filename not in _src_cache:
        try:
            f = open(filename, "r")
            _src_cache[filename] = f.readlines()
            f.close()
        except OSError:
            _src_cache[filename] = []
    lines: list[str] = _src_cache[filename]
    if lineno < 1 or lineno > len(lines):
        return ""
    return lines[lineno - 1].strip()


def extract_tb(tb: unknown) -> list[tuple[str, int, str, str]]:
    """帧链物化成 (file, line, func, 源行) 列表，外→内。None → 空表。"""
    out: list[tuple[str, int, str, str]] = []
    for fr in __pa_tb_frames(tb):
        file: str = fr[0]
        line: int = fr[1]
        func: str = fr[2]
        out.append((file, line, func, _source_line(file, line)))
    return out


def format_exc() -> str:
    """当前异常的完整 traceback 文本（含尾换行）；outside except →
    'NoneType: None\n'（CPython 口径）。"""
    t, v, tb = sys.exc_info()
    if t is None:
        return "NoneType: None\n"
    out = "Traceback (most recent call last):\n"
    for fr in __pa_tb_frames(tb):
        file: str = fr[0]
        line: int = fr[1]
        func: str = fr[2]
        out += '  File "' + file + '", line ' + str(line) + ", in " + func + "\n"
        src = _source_line(file, line)
        if src != "":
            out += "    " + src + "\n"
    name = t.__name__
    msg = str(v)
    if msg != "":
        out += name + ": " + msg + "\n"
    else:
        out += name + "\n"
    return out


def print_exc() -> None:
    """把当前异常 traceback 写到 sys.stderr（CPython print_exc 口径）。"""
    print(format_exc(), end="", file=sys.stderr)
