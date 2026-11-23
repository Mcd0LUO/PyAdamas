"""logging 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

支持等级常量、basicConfig(level=...)、getLogger(name)、Logger 的
debug/info/warning/error/critical/log/setLevel，默认输出到 sys.stderr，
格式为 ``LEVEL:name:message``。

第 3 批补齐：Formatter(fmt) / Handler / FileHandler(filename) /
StreamHandler()，以及 Logger.addHandler / setFormatter / setLevel。
Formatter 支持 %(levelname)s / %(message)s / %(name)s 三个占位符；
FileHandler 写文件（append 语义由调用方控制，本实现为覆盖模式 "w"，
第一次打开时截断），StreamHandler 写 sys.stderr。

与 CPython 的语义差异：
- CPython 的 Formatter 接受 record 对象，这里用 (levelname, name, message)
  三个 str 形参代替；只支持上述三个占位符。
- CPython 的 FileHandler 默认以 append 模式打开，这里用 "w"（覆盖），
  因为 PA 的 file 协议没有可靠的追加打开模式。测试用独立临时文件规避。
- 没有 Filter / propagate / 层级 logger / %-格式化日志参数；log(msg, arg)
  的 arg 被忽略。
- 没有 basicConfig(filename=...)；请用 FileHandler + addHandler 手动装配。
"""

import sys

CRITICAL = 50
FATAL = 50
ERROR = 40
WARNING = 30
WARN = 30
INFO = 20
DEBUG = 10
NOTSET = 0


def getLevelName(level: int) -> str:
    if level >= CRITICAL:
        return "CRITICAL"
    if level >= ERROR:
        return "ERROR"
    if level >= WARNING:
        return "WARNING"
    if level >= INFO:
        return "INFO"
    if level >= DEBUG:
        return "DEBUG"
    return "NOTSET"


class Formatter:
    """日志格式化器；只支持 %(levelname)s / %(name)s / %(message)s。"""
    fmt: str

    def __init__(self, fmt: str = "%(levelname)s:%(name)s:%(message)s") -> None:
        self.fmt = fmt

    def format(self, levelname: str, name: str, message: str) -> str:
        out: str = self.fmt
        out = out.replace("%(levelname)s", levelname)
        out = out.replace("%(name)s", name)
        out = out.replace("%(message)s", message)
        return out


class Handler:
    """处理器基类：保存等级和 formatter，emit 由子类实现。"""
    level: int
    formatter: Formatter

    def __init__(self) -> None:
        self.level = 0
        self.formatter = Formatter()

    def setLevel(self, level: int) -> None:
        self.level = level

    def setFormatter(self, fmt: Formatter) -> None:
        self.formatter = fmt

    def emit(self, levelname: str, name: str, message: str) -> None:
        pass


class StreamHandler(Handler):
    """输出到 sys.stderr。"""

    def __init__(self) -> None:
        self.level = 0
        self.formatter = Formatter()

    def emit(self, levelname: str, name: str, message: str) -> None:
        s: str = self.formatter.format(levelname, name, message)
        sys.stderr.write(s + "\n")


class FileHandler(Handler):
    """输出到文件；打开文件用 "w"，构造时会截断已有文件。"""
    f: file

    def __init__(self, filename: str) -> None:
        self.level = 0
        self.formatter = Formatter()
        self.f = open(filename, "w")

    def emit(self, levelname: str, name: str, message: str) -> None:
        s: str = self.formatter.format(levelname, name, message)
        self.f.write(s + "\n")
        self.f.flush()

    def close(self) -> None:
        self.f.close()


class Logger:
    name: str
    level: int
    _configured: bool
    handlers: list

    def __init__(self, name: str) -> None:
        self.name = name
        self.level = 0
        self._configured = False
        self.handlers = []

    def setLevel(self, level: int) -> None:
        self.level = level
        self._configured = True

    def addHandler(self, h: Handler) -> None:
        self.handlers.append(h)

    def setFormatter(self, fmt: Formatter) -> None:
        # CPython 的 Logger 没有 setFormatter；这里为对称性提供，
        # 把 formatter 设到已有 handler 上（最后一个）。
        i: int = len(self.handlers) - 1
        if i >= 0:
            self.handlers[i].setFormatter(fmt)

    def getEffectiveLevel(self) -> int:
        if self._configured:
            return self.level
        return _root_level

    def isEnabledFor(self, level: int) -> bool:
        return level >= self.getEffectiveLevel()

    def log(self, level: int, msg: str) -> None:
        if level < self.getEffectiveLevel():
            return
        levelname: str = getLevelName(level)
        if len(self.handlers) == 0:
            sys.stderr.write(levelname + ":" + self.name + ":" + msg + "\n")
            return
        i: int = 0
        while i < len(self.handlers):
            self.handlers[i].emit(levelname, self.name, msg)
            i += 1

    def debug(self, msg: str) -> None:
        self.log(DEBUG, msg)

    def info(self, msg: str) -> None:
        self.log(INFO, msg)

    def warning(self, msg: str) -> None:
        self.log(WARNING, msg)

    def warn(self, msg: str) -> None:
        self.log(WARNING, msg)

    def error(self, msg: str) -> None:
        self.log(ERROR, msg)

    def critical(self, msg: str) -> None:
        self.log(CRITICAL, msg)

    def exception(self, msg: str) -> None:
        self.log(ERROR, msg)


_root_level: int = WARNING
_loggers: dict[str, Logger] = {}


def basicConfig(level: int = WARNING) -> None:
    global _root_level
    _root_level = level


def getLogger(name: str = "root") -> Logger:
    if name in _loggers:
        return _loggers[name]
    lg = Logger(name)
    _loggers[name] = lg
    return lg


def setLevel(level: int) -> None:
    lg = getLogger("root")
    lg.setLevel(level)


def isEnabledFor(level: int) -> bool:
    lg = getLogger("root")
    return lg.isEnabledFor(level)


def log(level: int, msg: str) -> None:
    lg = getLogger("root")
    lg.log(level, msg)


def debug(msg: str) -> None:
    lg = getLogger("root")
    lg.debug(msg)


def info(msg: str) -> None:
    lg = getLogger("root")
    lg.info(msg)


def warning(msg: str) -> None:
    lg = getLogger("root")
    lg.warning(msg)


def warn(msg: str) -> None:
    lg = getLogger("root")
    lg.warn(msg)


def error(msg: str) -> None:
    lg = getLogger("root")
    lg.error(msg)


def critical(msg: str) -> None:
    lg = getLogger("root")
    lg.critical(msg)


def exception(msg: str) -> None:
    lg = getLogger("root")
    lg.exception(msg)
