"""configparser 模块的 pyadamas 子集（INI 配置解析，纯 pyadamas 源码）。

覆盖 ConfigParser 的日常面：read/read_string/read_file、sections/options/
has_section/has_option、get/getint/getfloat/getboolean（raw/fallback/vars）、
items、set/add_section/remove_section/remove_option、defaults、write、
optionxform（可覆盖）、DEFAULT 节、BasicInterpolation %(key)s（含 %% 转义、
10 层深度上限、before_set 校验、write 时 %% 再转义）。

语义与 CPython 对齐：选项名小写化、节名大小写敏感、=/: 分隔、续行按缩进、
空行计入多行值、整行 #/; 注释（默认无行内注释）、值 join 后 rstrip、
严格模式重复节/重复选项报错、错误消息逐字对齐（NoSectionError/NoOptionError/
DuplicateSectionError/DuplicateOptionError/ParsingError/
MissingSectionHeaderError/Interpolation*Error）。

语义边界：
- 构造只区分 interpolation=None（关闭插值）与默认（BasicInterpolation）；
  自定义插值对象按 BasicInterpolation 处理。
- get 的 fallback=None 与省略不可分（None/0 同形）：缺失且 fallback 为 None
  时抛 NoOptionError（CPython 返回 None）。
- set 的 value 只收 str；SectionProxy 下标（c["s"]["k"]）、read_dict、
  allow_no_values/inline_comment_prefixes/空节名等构造开关不做。
- 错误对象的 .section/.option/.source 等属性不做（消息一致），长尾桥接。
"""

from os.path import exists


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


def _repr_str(s: str) -> str:
    """CPython str repr 口径（错误消息用）：含单引号不含双引号时用双引号。"""
    quote = "'"
    has_sq = False
    has_dq = False
    for c in s:
        if c == "'":
            has_sq = True
        elif c == '"':
            has_dq = True
    if has_sq and not has_dq:
        quote = '"'
    out = quote
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
    n = str(lineno)
    if len(n) < 2:
        n = " " + n
    return "[line " + n + "]"


def _indent_of(line: str) -> int:
    i = 0
    while i < len(line):
        c = line[i]
        if c != " " and c != "\t":
            break
        i += 1
    return i


class ConfigParser:
    _sections: dict[str, dict[str, str]]
    _defaults: dict[str, str]
    _interp: bool

    def __init__(self, interpolation: object = 1) -> None:
        self._sections = {}
        self._defaults = {}
        self._interp = not (interpolation is None)

    def optionxform(self, optionstr: str) -> str:
        return optionstr.lower()

    # ── 读取 ──────────────────────────────────────────────────

    def read(self, filename: str, encoding: str = "utf-8") -> list[str]:
        if not exists(filename):
            return []
        try:
            f = open(filename, "r", encoding=encoding)
            text = f.read()
            f.close()
        except OSError:
            return []
        self._read(text, filename)
        return [filename]

    def read_string(self, text: str, source: str = "<string>") -> None:
        self._read(text, source)

    def read_file(self, f: file, source: str = "") -> None:
        src = source
        if len(src) == 0:
            src = f.name
        self._read(f.read(), src)

    def _read(self, text: str, source: str) -> None:
        secs: dict[str, dict[str, list[str]]] = {}
        defs: dict[str, list[str]] = {}
        cur: dict[str, list[str]] = {}
        seen: list[str] = []
        have_section = False
        sectname = ""
        optname = ""
        indent_level = 0
        err_lines: list[int] = []
        err_texts: list[str] = []
        raw_lines = text.split("\n")
        nraw = len(raw_lines)
        lineno = 0
        for raw in raw_lines:
            lineno += 1
            line = raw
            if lineno < nraw:
                line = raw + "\n"
            elif len(raw) == 0:
                continue
            trimmed = line.strip()
            clean = trimmed
            has_comments = False
            if len(trimmed) > 0 and (trimmed[0] == "#" or trimmed[0] == ";"):
                clean = ""
                has_comments = True
            if len(clean) == 0:
                if not has_comments and have_section and len(optname) > 0:
                    if optname in cur:
                        cur[optname].append("")
                continue
            cur_indent = _indent_of(line)
            # 续行：比上一个选项/节头行缩进更深
            if have_section and len(optname) > 0 and cur_indent > indent_level:
                if optname in cur:
                    cur[optname].append(clean)
                continue
            indent_level = cur_indent
            # 节头
            if len(clean) >= 3 and clean[0] == "[" and clean[len(clean) - 1] == "]":
                sectname = clean[1:len(clean) - 1]
                if sectname in seen:
                    raise DuplicateSectionError(
                        "While reading from " + _repr_str(source) + " " +
                        _line_tag(lineno) + ": section " + _repr_str(sectname) +
                        " already exists")
                seen.append(sectname)
                if sectname == "DEFAULT":
                    cur = defs
                else:
                    cur = {}
                    secs[sectname] = cur
                have_section = True
                optname = ""
                continue
            if not have_section:
                raise MissingSectionHeaderError(
                    "File contains no section headers.\nfile: " +
                    _repr_str(source) + ", line: " + str(lineno) + "\n" +
                    _repr_str(line))
            # 选项行：option = value / option : value
            eq = clean.find("=")
            colon = clean.find(":")
            delim = -1
            if eq >= 0 and (colon < 0 or eq < colon):
                delim = eq
            elif colon >= 0:
                delim = colon
            if delim < 0:
                err_lines.append(lineno)
                err_texts.append(line)
                continue
            opt = clean[:delim].rstrip()
            val = clean[delim + 1:].strip()
            if len(opt) == 0:
                err_lines.append(lineno)
                err_texts.append(line)
                continue
            optname = self.optionxform(opt)
            if optname in cur:
                raise DuplicateOptionError(
                    "While reading from " + _repr_str(source) + " " +
                    _line_tag(lineno) + ": option " + _repr_str(optname) +
                    " in section " + _repr_str(sectname) + " already exists")
            cur[optname] = [val]
        if len(err_lines) > 0:
            msg = "Source contains parsing errors: " + _repr_str(source)
            i = 0
            while i < len(err_lines):
                msg += "\n\t" + _line_tag(err_lines[i]) + ": " + _repr_str(err_texts[i])
                i += 1
            raise ParsingError(msg)
        # 多行值合并（join 后 rstrip）：每节先建带标注的局部 dict，
        # 再整体写入字段（避免 DictGet 借用的嵌套 dict 上下标存值路径）。
        for sname in secs:
            src_map = secs[sname]
            final: dict[str, str] = {}
            if sname in self._sections:
                old = self._sections[sname]
                for k in old:
                    final[k] = old[k]
            for k in src_map:
                final[k] = "\n".join(src_map[k]).rstrip()
            self._sections[sname] = final
        for k in defs:
            self._defaults[k] = "\n".join(defs[k]).rstrip()

    # ── 查询 ──────────────────────────────────────────────────

    def sections(self) -> list[str]:
        return list(self._sections.keys())

    def has_section(self, section: str) -> bool:
        return section in self._sections

    def options(self, section: str) -> list[str]:
        if section not in self._sections:
            raise NoSectionError("No section: " + _repr_str(section))
        out = list(self._sections[section].keys())
        for k in self._defaults:
            if k not in out:
                out.append(k)
        return out

    def has_option(self, section: str, option: str) -> bool:
        if section not in self._sections:
            return False
        o = self.optionxform(option)
        return o in self._sections[section] or o in self._defaults

    def defaults(self) -> dict[str, str]:
        return self._defaults

    def _unify(self, section: str, vars: object) -> dict[str, str]:
        d: dict[str, str] = {}
        for k in self._defaults:
            d[k] = self._defaults[k]
        sec = self._sections[section]
        for k in sec:
            d[k] = sec[k]
        if not (vars is None):
            vd: dict[str, str] = {}
            vd = vars
            for k in vd:
                d[k] = vd[k]
        return d

    def _get_raw(self, section: str, option: str) -> str:
        if section not in self._sections:
            raise NoSectionError("No section: " + _repr_str(section))
        o = self.optionxform(option)
        sec = self._sections[section]
        if o in sec:
            return sec[o]
        if o in self._defaults:
            return self._defaults[o]
        raise NoOptionError("No option " + _repr_str(o) + " in section: " +
                            _repr_str(section))

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
            p = rest.find("%")
            if p < 0:
                accum.append(rest)
                return
            if p > 0:
                accum.append(rest[:p])
            rest = rest[p:]
            if len(rest) >= 2 and rest[1] == "%":
                accum.append("%")
                rest = rest[2:]
                continue
            if len(rest) >= 2 and rest[1] == "(":
                close = rest.find(")", 2)
                if close < 0 or close + 1 >= len(rest) or rest[close + 1] != "s" or close == 2:
                    raise InterpolationSyntaxError(
                        "bad interpolation variable reference " + _repr_str(rest))
                var = rest[2:close]
                rest = rest[close + 2:]
                if var not in d:
                    raise InterpolationMissingOptionError(
                        "Bad value substitution: option " + _repr_str(option) +
                        " in section " + _repr_str(section) +
                        " contains an interpolation key " + _repr_str(var) +
                        " which is not a valid option name. Raw value: " +
                        _repr_str(rawval))
                self._interp_some(section, option, accum, d[var], d, depth + 1,
                                  rawval)
                continue
            raise InterpolationSyntaxError(
                "'%' must be followed by '%' or '(', found: " + _repr_str(rest))

    def get(self, section: str, option: str, *, raw: bool = False,
            vars: object = None, fallback: object = None) -> object:
        v: str = ""
        try:
            v = self._get_raw(section, option)
        except Error:
            if fallback is None:
                raise
            return fallback
        if raw:
            return v
        d = self._unify(section, vars)
        return self._interp_value(section, self.optionxform(option), v, d)

    def getint(self, section: str, option: str, *, raw: bool = False,
               vars: object = None, fallback: object = None) -> object:
        v: str = ""
        try:
            v = self._get_raw(section, option)
        except Error:
            if fallback is None:
                raise
            return fallback
        if not raw:
            d = self._unify(section, vars)
            v = self._interp_value(section, self.optionxform(option), v, d)
        return int(v)

    def getfloat(self, section: str, option: str, *, raw: bool = False,
                 vars: object = None, fallback: object = None) -> object:
        v: str = ""
        try:
            v = self._get_raw(section, option)
        except Error:
            if fallback is None:
                raise
            return fallback
        if not raw:
            d = self._unify(section, vars)
            v = self._interp_value(section, self.optionxform(option), v, d)
        return float(v)

    def getboolean(self, section: str, option: str, *, raw: bool = False,
                   vars: object = None, fallback: object = None) -> object:
        v: str = ""
        try:
            v = self._get_raw(section, option)
        except Error:
            if fallback is None:
                raise
            return fallback
        if not raw:
            d = self._unify(section, vars)
            v = self._interp_value(section, self.optionxform(option), v, d)
        lv = v.lower()
        if lv == "1" or lv == "yes" or lv == "true" or lv == "on":
            return True
        if lv == "0" or lv == "no" or lv == "false" or lv == "off":
            return False
        raise ValueError("Not a boolean: " + v)

    def items(self, section: str, *, raw: bool = False,
              vars: object = None) -> list[tuple[str, str]]:
        if section not in self._sections:
            raise NoSectionError("No section: " + _repr_str(section))
        d = self._unify(section, vars)
        out: list[tuple[str, str]] = []
        for k in d:
            v = d[k]
            if not raw:
                v = self._interp_value(section, k, v, d)
            out.append((k, v))
        return out

    # ── 修改 ──────────────────────────────────────────────────

    def add_section(self, section: str) -> None:
        if section == "DEFAULT":
            raise ValueError("Invalid section name: 'DEFAULT'")
        if section in self._sections:
            raise DuplicateSectionError("Section " + _repr_str(section) +
                                        " already exists")
        self._sections[section] = {}

    def set(self, section: str, option: str, value: str = "") -> None:
        if section not in self._sections:
            raise NoSectionError("No section: " + _repr_str(section))
        if self._interp:
            tmp = value.replace("%%", "")
            stripped = ""
            i = 0
            while i < len(tmp):
                if tmp[i] == "%" and i + 1 < len(tmp) and tmp[i + 1] == "(":
                    close = tmp.find(")", i + 2)
                    if close > i + 2 and close + 1 < len(tmp) and tmp[close + 1] == "s":
                        i = close + 2
                        continue
                stripped += tmp[i]
                i += 1
            if "%" in stripped:
                raise ValueError("invalid interpolation syntax in " +
                                 _repr_str(value) + " at position " +
                                 str(stripped.find("%")))
        self._sections[section][self.optionxform(option)] = value

    def remove_section(self, section: str) -> bool:
        if section in self._sections:
            del self._sections[section]
            return True
        return False

    def remove_option(self, section: str, option: str) -> bool:
        if section not in self._sections:
            raise NoSectionError("No section: " + _repr_str(section))
        o = self.optionxform(option)
        sec = self._sections[section]
        if o in sec:
            del sec[o]
            return True
        return False

    def write(self, f: file) -> None:
        if len(self._defaults) > 0:
            f.write("[DEFAULT]\n")
            for k in self._defaults:
                v = self._defaults[k].replace("\n", "\n\t")
                f.write(k + " = " + v + "\n")
            f.write("\n")
        for sname in self._sections:
            f.write("[" + sname + "]\n")
            sec = self._sections[sname]
            for k in sec:
                v = sec[k].replace("\n", "\n\t")
                f.write(k + " = " + v + "\n")
            f.write("\n")
