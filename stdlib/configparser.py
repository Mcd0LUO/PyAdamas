"""configparser 模块的 pyadamas 子集（INI 配置解析，纯 pyadamas 源码）。

本模块在 stdlib-batch1 的基础上按 CPython 3.14 语义重做（batch7），覆盖
ConfigParser / RawConfigParser / SectionProxy 的日常面：

- 读取：read（单个路径或路径列表，不存在的忽略）、read_string、read_file
  （source 省略时取 f.name，取不到用 '<???'）、read_dict（基础面）。
- 解析：'[section]' 头（头后可跟任意尾随文本，同 CPython 的 SECTCRE.match）、
  'key=value' / 'key:value'（取最左分隔符，键右剥空白、值两端 strip）、
  缩进续行（比上一个键/节头行缩进更深）、空行计入多行值（注释行不计）、
  整行 '#'/';' 注释（默认无行内注释）、值 join 后 rstrip、重复节/重复键
  按 strict 报错（strict=False 时后者覆盖）、'[DEFAULT]' 可重复声明、
  解析错误累积到文件末尾一次性抛 ParsingError（成功解析的部分已提交）。
- 查询：sections / options / defaults / has_section / has_option（空节名与
  'DEFAULT' 视为 DEFAULT 节）/ items（DEFAULT 优先序）/ get（含 DEFAULT
  继承、vars 覆盖、raw、fallback）。
- 类型化取值：getint / getfloat / getboolean（yes/no/true/false/on/off/1/0，
  大小写不敏感；非法值 ValueError）。
- 修改：set（空节名/'DEFAULT' 写默认节）、add_section（'DEFAULT' 抛
  ValueError）、remove_section / remove_option（'DEFAULT' 可删默认项）、
  write（space_around_delimiters 开关、键含分隔符或像节头抛 InvalidWriteError）。
- 映射面：parser['sec'] / len / in / iter / 下标赋值删除；SectionProxy
  的 get/getint/getfloat/getboolean/items/keys/values/copy/update/
  setdefault/pop/clear 与 '__getitem__'/'__setitem__'/'__delitem__'/in/len/iter。
- 插值：BasicInterpolation 的 %(key)s（同节 + DEFAULT + vars，%% 转义，
  10 层深度上限，缺失键/语法/深度三类错误）；RawConfigParser 无插值；
  ConfigParser(interpolation=None) 关闭插值。

语义边界（轻量标尺：同参数同功能同结果，错误类型对、消息合理）：
- fallback 的「省略」用私有串哨兵 _UNSET 表示（pa 默认参数只允许字面常量，
  无法用对象哨兵）；哨兵内容不可能作为用户值出现，省略与显式 None 区分正确。
- get/getint/getfloat/getboolean 的省略 fallback 抛错；显式 fallback 返回原值
  （含 None），与 CPython 一致。
- 错误消息在既有 253/254 钉住的路径上与 CPython 逐字一致（[line %2d]、
  repr(line)、插值三条消息）；其余路径消息合理即可。
- strict=False 时重复节/键后者覆盖（不再报错），解析错误仍照常抛
  ParsingError（CPython 同口径）。
- 未实现（保守跳过 + 负面钉住）：allow_no_value（无值选项为 None）、
  自定义 delimiters/comment_prefixes/inline_comment_prefixes、
  empty_lines_in_values=False、dict_type、converters（get*/converters 映射）、
  ExtendedInterpolation（\$\{section:key\}）、popitem、allow_unnamed_section /
  UNNAMED_SECTION、MultilineContinuationError / UnnamedSectionDisabledError、
  错误对象的 .section/.option/.source 等属性（只保证类型与消息）。
- SectionProxy 每次 c['sec'] 新建（CPython 有代理缓存，这里 is 不成立）。
- get 的静态返回类型是 object（fallback 可为任意值），字符串场景直接使用
  不受影响；需要拼接时先 str(...)。
- 解析失败时已解析的节/键会留在 parser 里（CPython 的 _read 同样先提交再抛，
  finally 里 join 多行）。
"""

from os.path import exists

# fallback / read_file(source) 的「省略」哨兵：pa 的默认参数只收字面常量，
# 因此用一个不可能出现在配置里的私有串（身份比较在 pa 里按内容稳定成立）。
_UNSET: str = "__configparser_unset_9f3c__"


def _is_unset(x: object) -> bool:
    """哨兵判定：默认参数字面量在不同编译单元里可能不是同一对象，先 is 再 ==。"""
    if x is _UNSET:
        return True
    return x == _UNSET


class Error(Exception):
    pass


class NoSectionError(Error):
    pass


class DuplicateSectionError(Error):
    pass


class DuplicateOptionError(Error):
    pass


class NoOptionError(Error):
    pass


class InterpolationError(Error):
    pass


class InterpolationMissingOptionError(InterpolationError):
    pass


class InterpolationSyntaxError(InterpolationError):
    pass


class InterpolationDepthError(InterpolationError):
    pass


class ParsingError(Error):
    pass


class MissingSectionHeaderError(ParsingError):
    pass


class InvalidWriteError(Error):
    pass


def _repr_str(s: str) -> str:
    """CPython str repr 口径（错误消息用）：含单引号不含双引号时用双引号。"""
    quote: str = "'"
    has_sq: bool = False
    has_dq: bool = False
    for c in s:
        if c == "'":
            has_sq = True
        elif c == '"':
            has_dq = True
    if has_sq and not has_dq:
        quote = '"'
    out: str = quote
    for c in s:
        if c == "\\":
            out += "\\\\"
        elif c == "\n":
            out += "\\n"
        elif c == "\r":
            out += "\\r"
        elif c == "\t":
            out += "\\t"
        elif c == quote:
            out += "\\" + c
        else:
            out += c
    return out + quote


def _line_tag(lineno: int) -> str:
    n: str = str(lineno)
    if len(n) < 2:
        n = " " + n
    return "[line " + n + "]"


def _indent_of(line: str) -> int:
    i: int = 0
    while i < len(line):
        c: str = line[i]
        if c != " " and c != "\t":
            break
        i += 1
    return i


class Interpolation:
    """标记类：值原样透传（RawConfigParser 与 ConfigParser(interpolation=None)）。"""


class BasicInterpolation(Interpolation):
    """标记类：%(key)s 插值（实现内联在 RawConfigParser 的取值路径里）。"""


class SectionProxy:
    """单个节的视图（parser['sec'] 返回）；DEFAULT 节返回默认项集合。"""

    parser: "RawConfigParser"
    name: str

    def __init__(self, parser: "RawConfigParser", name: str) -> None:
        self.parser = parser
        self.name = name

    def __repr__(self) -> str:
        return "<Section: " + self.name + ">"

    def _options(self) -> list[str]:
        if self.name != self.parser.default_section:
            return self.parser.options(self.name)
        d: dict[str, str] = self.parser.defaults()
        keys: list[str] = list(d.keys())
        return keys

    def __getitem__(self, key: str) -> str:
        if not self.parser.has_option(self.name, key):
            raise KeyError(key)
        v: object = self.parser.get(self.name, key)
        s: str = v
        return s

    def __setitem__(self, key: str, value: str) -> None:
        self.parser.set(self.name, key, value)

    def __delitem__(self, key: str) -> None:
        if not (self.parser.has_option(self.name, key) and
                self.parser.remove_option(self.name, key)):
            raise KeyError(key)

    def __contains__(self, key) -> bool:
        return self.parser.has_option(self.name, key)

    def __len__(self) -> int:
        return len(self._options())

    def __iter__(self) -> object:
        return iter(self._options())

    def get(self, option: str, fallback: object = None, *, raw: bool = False,
            vars: object = None) -> object:
        return self.parser.get(self.name, option, raw=raw, vars=vars,
                               fallback=fallback)

    def getint(self, option: str, fallback: object = None, *,
               raw: bool = False, vars: object = None) -> object:
        return self.parser.getint(self.name, option, raw=raw, vars=vars,
                                  fallback=fallback)

    def getfloat(self, option: str, fallback: object = None, *,
                 raw: bool = False, vars: object = None) -> object:
        return self.parser.getfloat(self.name, option, raw=raw, vars=vars,
                                    fallback=fallback)

    def getboolean(self, option: str, fallback: object = None, *,
                   raw: bool = False, vars: object = None) -> bool:
        return self.parser.getboolean(self.name, option, raw=raw, vars=vars,
                                      fallback=fallback)

    def keys(self) -> list[str]:
        return self._options()

    def values(self) -> list[str]:
        out: list[str] = []
        for k in self._options():
            out.append(self[k])
        return out

    def items(self) -> list[tuple[str, str]]:
        out: list[tuple[str, str]] = []
        for k in self._options():
            out.append((k, self[k]))
        return out

    def copy(self) -> dict[str, str]:
        d: dict[str, str] = {}
        for k in self._options():
            d[k] = self[k]
        return d

    def update(self, other: dict[str, str]) -> None:
        o: dict[str, str] = other
        for k in o:
            self[k] = o[k]

    def setdefault(self, key: str, default: str = "") -> str:
        if self.parser.has_option(self.name, key):
            return self[key]
        self[key] = default
        return default

    def pop(self, key: str,
            default: object = "__configparser_unset_9f3c__") -> object:
        if not self.parser.has_option(self.name, key):
            if _is_unset(default):
                raise KeyError(key)
            return default
        v: str = self[key]
        self.parser.remove_option(self.name, key)
        return v

    def clear(self) -> None:
        for k in list(self._options()):
            self.parser.remove_option(self.name, k)


class RawConfigParser:
    """无插值（或显式 BasicInterpolation）的 INI 解析器。"""

    _sections: dict[str, dict[str, str]]
    _defaults: dict[str, str]
    _interp: bool
    _strict: bool
    default_section: str

    def __init__(self, defaults: object = None, *,
                 interpolation: object = "none",
                 strict: bool = True) -> None:
        self._sections = {}
        self._defaults = {}
        self._interp = isinstance(interpolation, BasicInterpolation)
        self._strict = strict
        self.default_section = "DEFAULT"
        self._install_defaults(defaults)

    def _install_defaults(self, defaults: object) -> None:
        # 轻量标尺：defaults 的值需为 str（CPython 会把任意值 str() 化；
        # pa 的 object 表示层对非 str 值做 str() 会崩，故不做转换，见模块头边界）。
        if defaults is None:
            return
        d: dict[str, str] = defaults
        for k in d:
            v: str = d[k]
            self._defaults[self.optionxform(k)] = v

    def optionxform(self, optionstr: str) -> str:
        return optionstr.lower()

    # ── 查询 ──────────────────────────────────────────────────

    def defaults(self) -> dict[str, str]:
        return self._defaults

    def sections(self) -> list[str]:
        return list(self._sections.keys())

    def add_section(self, section: str) -> None:
        if section == self.default_section:
            raise ValueError("Invalid section name: " + _repr_str(section))
        if section in self._sections:
            raise DuplicateSectionError("Section " + _repr_str(section) +
                                        " already exists")
        self._sections[section] = {}

    def has_section(self, section: str) -> bool:
        return section in self._sections

    def options(self, section: str) -> list[str]:
        if section not in self._sections:
            raise NoSectionError("No section: " + _repr_str(section))
        out: list[str] = list(self._sections[section].keys())
        for k in self._defaults:
            if k not in out:
                out.append(k)
        return out

    def has_option(self, section: str, option: str) -> bool:
        o: str = self.optionxform(option)
        if len(section) == 0 or section == self.default_section:
            return o in self._defaults
        if section not in self._sections:
            return False
        return o in self._sections[section] or o in self._defaults

    # ── 读取 ──────────────────────────────────────────────────

    def read(self, filenames: object, encoding: object = None) -> list[str]:
        files: list[str] = []
        if isinstance(filenames, str):
            one: str = filenames
            files.append(one)
        else:
            files = filenames
        enc: str = "utf-8"
        if not (encoding is None):
            wanted: str = encoding
            if len(wanted) > 0:
                enc = wanted
        out: list[str] = []
        for fn in files:
            if not exists(fn):
                continue
            try:
                f = open(fn, "r", encoding=enc)
                text: str = f.read()
                f.close()
            except OSError:
                continue
            self._read(text, fn)
            out.append(fn)
        return out

    def read_string(self, string: str, source: str = "<string>") -> None:
        self._read(string, source)

    def read_file(self, f: file,
                  source: object = "__configparser_unset_9f3c__") -> None:
        src: str = ""
        if _is_unset(source):
            try:
                src = f.name
            except AttributeError:
                src = "<???>"
        else:
            given: str = source
            src = given
        self._read(f.read(), src)

    def read_dict(self, dictionary: object, source: str = "<dict>") -> None:
        d: dict[str, dict[str, str]] = dictionary
        seen_secs: list[str] = []
        seen_opts: list[str] = []
        for sname in d:
            keys: dict[str, str] = d[sname]
            repeated: bool = sname in seen_secs
            if len(sname) > 0 and sname != self.default_section:
                if repeated:
                    if self._strict:
                        raise DuplicateSectionError(
                            "Section " + _repr_str(sname) + " already exists")
                elif sname not in self._sections:
                    self.add_section(sname)
                seen_secs.append(sname)
            for key in keys:
                k: str = self.optionxform(key)
                v: str = keys[key]
                if v is None:
                    v = ""
                pair: str = sname + "\x00" + k
                if self._strict and pair in seen_opts:
                    raise DuplicateOptionError(
                        "While reading from " + _repr_str(source) + ": option " +
                        _repr_str(k) + " in section " + _repr_str(sname) +
                        " already exists")
                seen_opts.append(pair)
                self.set(sname, k, str(v))

    def _read(self, text: str, source: str) -> None:
        if "\r" in text:
            text = text.replace("\r\n", "\n").replace("\r", "\n")
        secs: dict[str, dict[str, list[str]]] = {}
        defs: dict[str, list[str]] = {}
        cur: dict[str, list[str]] = {}
        seen_secs: list[str] = []
        seen_opts: list[str] = []
        have_section: bool = False
        has_opt: bool = False
        sectname: str = ""
        optname: str = ""
        indent_level: int = 0
        err_lines: list[int] = []
        err_texts: list[str] = []
        raw_lines: list[str] = text.split("\n")
        nraw: int = len(raw_lines)
        lineno: int = 0
        for raw in raw_lines:
            lineno += 1
            line: str = raw
            if lineno < nraw:
                line = raw + "\n"
            elif len(raw) == 0:
                continue
            trimmed: str = line.strip()
            clean: str = trimmed
            has_comments: bool = False
            if len(trimmed) > 0 and (trimmed[0] == "#" or trimmed[0] == ";"):
                clean = ""
                has_comments = True
            if len(clean) == 0:
                if not has_comments and have_section and has_opt and optname in cur:
                    cur[optname].append("")
                continue
            cur_indent: int = _indent_of(line)
            # 续行：比上一个键/节头行缩进更深
            if have_section and has_opt and cur_indent > indent_level:
                if optname in cur:
                    cur[optname].append(clean)
                continue
            indent_level = cur_indent
            # 节头：'[' 起头且后面有 ']'，头后可跟尾随文本（SECTCRE.match 口径）
            if len(clean) >= 2 and clean[0] == "[":
                last: int = clean.rfind("]")
                if last >= 2:
                    sectname = clean[1:last]
                    has_opt = False
                    optname = ""
                    indent_level = cur_indent
                    have_section = True
                    if sectname == self.default_section:
                        cur = defs
                        continue
                    if sectname in seen_secs:
                        if self._strict:
                            raise DuplicateSectionError(
                                "While reading from " + _repr_str(source) + " " +
                                _line_tag(lineno) + ": section " +
                                _repr_str(sectname) + " already exists")
                    else:
                        seen_secs.append(sectname)
                    if sectname in secs:
                        cur = secs[sectname]
                    else:
                        cur = {}
                        secs[sectname] = cur
                    continue
            if not have_section:
                raise MissingSectionHeaderError(
                    "File contains no section headers.\nfile: " +
                    _repr_str(source) + ", line: " + str(lineno) + "\n" +
                    _repr_str(line))
            # 键值行：取最左 '=' 或 ':'，键右剥空白、值两端 strip
            eq: int = clean.find("=")
            colon: int = clean.find(":")
            delim: int = -1
            if eq >= 0 and (colon < 0 or eq < colon):
                delim = eq
            elif colon >= 0:
                delim = colon
            if delim < 0:
                err_lines.append(lineno)
                err_texts.append(line)
                has_opt = False
                continue
            opt: str = clean[:delim].rstrip()
            val: str = clean[delim + 1:].strip()
            optname = self.optionxform(opt)
            has_opt = len(optname) > 0
            if not has_opt:
                err_lines.append(lineno)
                err_texts.append(line)
                continue
            pair: str = sectname + "\x00" + optname
            if self._strict and pair in seen_opts:
                raise DuplicateOptionError(
                    "While reading from " + _repr_str(source) + " " +
                    _line_tag(lineno) + ": option " + _repr_str(optname) +
                    " in section " + _repr_str(sectname) + " already exists")
            seen_opts.append(pair)
            cur[optname] = [val]
        # 提交（含多行 join/rstrip）：解析错误也要先留下已解析部分，同 CPython
        for sname in secs:
            merged: dict[str, str] = {}
            if sname in self._sections:
                prev: dict[str, str] = self._sections[sname]
                for k in prev:
                    merged[k] = prev[k]
            src: dict[str, list[str]] = secs[sname]
            for k in src:
                merged[k] = "\n".join(src[k]).rstrip()
            self._sections[sname] = merged
        for k in defs:
            self._defaults[k] = "\n".join(defs[k]).rstrip()
        if len(err_lines) > 0:
            msg: str = "Source contains parsing errors: " + _repr_str(source)
            i: int = 0
            while i < len(err_lines):
                msg += "\n\t" + _line_tag(err_lines[i]) + ": " + _repr_str(
                    err_texts[i])
                i += 1
            raise ParsingError(msg)

    # ── 取值 ──────────────────────────────────────────────────

    def _unify_values(self, section: str, vars: object) -> dict[str, str]:
        d: dict[str, str] = {}
        if section == self.default_section:
            pass
        elif section not in self._sections:
            raise NoSectionError("No section: " + _repr_str(section))
        else:
            sec: dict[str, str] = self._sections[section]
            for k in sec:
                d[k] = sec[k]
        for k in self._defaults:
            if k not in d:
                d[k] = self._defaults[k]
        if not (vars is None):
            vd: dict[str, str] = vars
            for k in vd:
                v: str = vd[k]
                if v is None:
                    d[self.optionxform(k)] = ""
                else:
                    d[self.optionxform(k)] = str(v)
        return d

    def get(self, section: str, option: str, *, raw: bool = False,
            vars: object = None,
            fallback: object = "__configparser_unset_9f3c__") -> object:
        try:
            d: dict[str, str] = self._unify_values(section, vars)
        except NoSectionError:
            if _is_unset(fallback):
                raise
            return fallback
        opt: str = self.optionxform(option)
        if opt not in d:
            if _is_unset(fallback):
                raise NoOptionError("No option " + _repr_str(opt) +
                                    " in section: " + _repr_str(section))
            return fallback
        value: str = d[opt]
        if raw:
            return value
        return self._interp_value(section, opt, value, d)

    def getint(self, section: str, option: str, *, raw: bool = False,
               vars: object = None,
               fallback: object = "__configparser_unset_9f3c__") -> object:
        try:
            v: object = self.get(section, option, raw=raw, vars=vars)
        except (NoSectionError, NoOptionError):
            if _is_unset(fallback):
                raise
            return fallback
        s: str = v
        return int(s)

    def getfloat(self, section: str, option: str, *, raw: bool = False,
                 vars: object = None,
                 fallback: object = "__configparser_unset_9f3c__") -> object:
        try:
            v: object = self.get(section, option, raw=raw, vars=vars)
        except (NoSectionError, NoOptionError):
            if _is_unset(fallback):
                raise
            return fallback
        s: str = v
        return float(s)

    def getboolean(self, section: str, option: str, *, raw: bool = False,
                   vars: object = None,
                   fallback: object = "__configparser_unset_9f3c__") -> bool:
        try:
            v: object = self.get(section, option, raw=raw, vars=vars)
        except (NoSectionError, NoOptionError):
            if _is_unset(fallback):
                raise
            return fallback
        s: str = v
        lv: str = s.lower()
        if lv == "1" or lv == "yes" or lv == "true" or lv == "on":
            return True
        if lv == "0" or lv == "no" or lv == "false" or lv == "off":
            return False
        raise ValueError("Not a boolean: " + s)

    def items(self, section: str, *, raw: bool = False,
              vars: object = None) -> list[tuple[str, str]]:
        d: dict[str, str] = {}
        for k in self._defaults:
            d[k] = self._defaults[k]
        if section != self.default_section:
            if section not in self._sections:
                raise NoSectionError("No section: " + _repr_str(section))
            sec: dict[str, str] = self._sections[section]
            for k in sec:
                d[k] = sec[k]
        orig: list[str] = list(d.keys())
        if not (vars is None):
            vd: dict[str, str] = vars
            for k in vd:
                d[self.optionxform(k)] = str(vd[k])
        out: list[tuple[str, str]] = []
        for k in orig:
            v: str = d[k]
            if not raw:
                v = self._interp_value(section, k, v, d)
            out.append((k, v))
        return out

    # ── 基本插值 ──────────────────────────────────────────────

    def _interp_value(self, section: str, option: str, value: str,
                      d: dict[str, str]) -> str:
        if not self._interp:
            return value
        accum: list[str] = []
        self._interp_some(section, option, accum, value, d, 1, value)
        return "".join(accum)

    def _interp_some(self, section: str, option: str, accum: list[str],
                     rest: str, d: dict[str, str], depth: int,
                     rawval: str) -> None:
        if depth > 10:
            raise InterpolationDepthError(
                "Recursion limit exceeded in value substitution: option " +
                _repr_str(option) + " in section " + _repr_str(section) +
                " contains an interpolation key which cannot be substituted " +
                "in 10 steps. Raw value: " + _repr_str(rawval))
        while len(rest) > 0:
            p: int = rest.find("%")
            if p < 0:
                accum.append(rest)
                # break（而非 return）：经循环自然结束出口退出，该出口释放
                # 全部局部槽。早返回出口在 var/v 首次堆写入之前降级，编译器
                # 单遍 release_slots 看不到所有权标记会漏 dec（字符串 RC
                # 泄漏，见 docs/strfix-rc-early-return.md）。循环后无语句，
                # break 与 return 语义相同。
                break
            if p > 0:
                accum.append(rest[:p])
            rest = rest[p:]
            c: str = rest[1:2]
            if c == "%":
                accum.append("%")
                rest = rest[2:]
                continue
            if c == "(":
                close: int = rest.find(")")
                ok: bool = False
                var: str = ""
                if close > 2 and close + 1 < len(rest) and rest[close + 1] == "s":
                    ok = True
                    var = rest[2:close]
                if not ok:
                    raise InterpolationSyntaxError(
                        "bad interpolation variable reference " + _repr_str(rest))
                rest = rest[close + 2:]
                var = self.optionxform(var)
                if var not in d:
                    raise InterpolationMissingOptionError(
                        "Bad value substitution: option " + _repr_str(option) +
                        " in section " + _repr_str(section) +
                        " contains an interpolation key " + _repr_str(var) +
                        " which is not a valid option name. Raw value: " +
                        _repr_str(rawval))
                v: str = d[var]
                if "%" in v:
                    self._interp_some(section, option, accum, v, d, depth + 1,
                                      rawval)
                else:
                    accum.append(v)
                continue
            raise InterpolationSyntaxError(
                "'%' must be followed by '%' or '(', found: " + _repr_str(rest))

    # ── 修改 ──────────────────────────────────────────────────

    def set(self, section: str, option: str, value: str = "") -> None:
        if len(value) > 0 and self._interp:
            self._check_interp_syntax(value)
        if len(section) == 0 or section == self.default_section:
            self._defaults[self.optionxform(option)] = value
            return
        if section not in self._sections:
            raise NoSectionError("No section: " + _repr_str(section))
        sec: dict[str, str] = self._sections[section]
        sec[self.optionxform(option)] = value

    def _check_interp_syntax(self, value: str) -> None:
        tmp: str = value.replace("%%", "")
        out: str = ""
        i: int = 0
        while i < len(tmp):
            if tmp[i] == "%" and i + 1 < len(tmp) and tmp[i + 1] == "(":
                close: int = tmp.find(")", i + 2)
                if close > i + 2 and close + 1 < len(tmp) and tmp[close + 1] == "s":
                    i = close + 2
                    continue
            out += tmp[i]
            i += 1
        if "%" in out:
            raise ValueError("invalid interpolation syntax in " +
                             _repr_str(value) + " at position " +
                             str(out.find("%")))

    def remove_option(self, section: str, option: str) -> bool:
        if len(section) == 0 or section == self.default_section:
            o: str = self.optionxform(option)
            existed: bool = o in self._defaults
            if existed:
                del self._defaults[o]
            return existed
        if section not in self._sections:
            raise NoSectionError("No section: " + _repr_str(section))
        o = self.optionxform(option)
        sec: dict[str, str] = self._sections[section]
        existed = o in sec
        if existed:
            del sec[o]
        return existed

    def remove_section(self, section: str) -> bool:
        if section in self._sections:
            del self._sections[section]
            return True
        return False

    def _validate_key_contents(self, key: str) -> None:
        if len(key) >= 2 and key[0] == "[" and key.rfind("]") >= 2:
            raise InvalidWriteError("Cannot write key " + key +
                                    "; begins with section pattern")
        if "=" in key:
            raise InvalidWriteError("Cannot write key " + key +
                                    "; contains delimiter =")
        if ":" in key:
            raise InvalidWriteError("Cannot write key " + key +
                                    "; contains delimiter :")

    def _write_section(self, fp: file, name: str,
                       sec: dict[str, str], delim: str) -> None:
        fp.write("[" + name + "]\n")
        for k in sec:
            self._validate_key_contents(k)
            v: str = sec[k]
            if v is None:
                v = ""
            fp.write(k + delim + v.replace("\n", "\n\t") + "\n")
        fp.write("\n")

    def write(self, fp: file, space_around_delimiters: bool = True) -> None:
        delim: str = " = "
        if not space_around_delimiters:
            delim = "="
        if len(self._defaults) > 0:
            self._write_section(fp, self.default_section, self._defaults,
                                delim)
        for sname in self._sections:
            self._write_section(fp, sname, self._sections[sname], delim)

    # ── 映射协议 ──────────────────────────────────────────────

    def __getitem__(self, key: str) -> SectionProxy:
        if key != self.default_section and key not in self._sections:
            raise KeyError(key)
        return SectionProxy(self, key)

    def __setitem__(self, key: str, value: object) -> None:
        vals: dict[str, str] = value
        if key == self.default_section:
            old: dict[str, str] = self._defaults
            for k in list(old.keys()):
                del old[k]
        elif key in self._sections:
            sec: dict[str, str] = self._sections[key]
            for k in list(sec.keys()):
                del sec[k]
        one: dict[str, dict[str, str]] = {}
        one[key] = vals
        self.read_dict(one)

    def __delitem__(self, key: str) -> None:
        if key == self.default_section:
            raise ValueError("Cannot remove the default section.")
        if key not in self._sections:
            raise KeyError(key)
        del self._sections[key]

    def __contains__(self, key) -> bool:
        return key == self.default_section or key in self._sections

    def __len__(self) -> int:
        return len(self._sections) + 1

    def __iter__(self) -> object:
        keys: list[str] = [self.default_section]
        for k in self._sections:
            keys.append(k)
        return iter(keys)


class ConfigParser(RawConfigParser):
    """带 BasicInterpolation 的配置解析器（CPython 的默认类）。"""

    def __init__(self, defaults: object = None, *,
                 interpolation: object = "basic",
                 strict: bool = True) -> None:
        super().__init__(None, strict=strict)
        if isinstance(interpolation, Interpolation) and not isinstance(
                interpolation, BasicInterpolation):
            self._interp = False
        else:
            self._interp = not (interpolation is None)
        self._install_defaults(defaults)


