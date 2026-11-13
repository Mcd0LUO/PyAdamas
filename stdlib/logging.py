"""logging 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

支持等级常量、basicConfig(level=...)、getLogger(name)、
Logger 的 debug/info/warning/error/critical/log/setLevel，默认输出到
sys.stderr，格式为 ``LEVEL:name:message``。不建模 handler / filter /
Formatter / 文件输出 / % 参数展开。
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


class Logger:
    name: str
    level: int
    _configured: bool

    def __init__(self, name: str) -> None:
        self.name = name
        self.level = 0
        self._configured = False

    def setLevel(self, level: int) -> None:
        self.level = level
        self._configured = True

    def getEffectiveLevel(self) -> int:
        if self._configured:
            return self.level
        return _root_level

    def isEnabledFor(self, level: int) -> bool:
        return level >= self.getEffectiveLevel()

    def log(self, level: int, msg: str) -> None:
        if level >= self.getEffectiveLevel():
            sys.stderr.write(getLevelName(level) + ":" + self.name + ":" + msg + "\n")

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
