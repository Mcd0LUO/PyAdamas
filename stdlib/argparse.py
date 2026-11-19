"""argparse 模块的 pyadamas 子集（CLI 解析，纯 pyadamas 源码）。

覆盖日常 CLI 面：ArgumentParser(prog/description/epilog/add_help/
allow_abbrev/usage)、add_argument（store/store_true/store_false/
store_const/append/append_const/count/help；nargs None/?/*/+/整数；
type=int/float/str/自定义可调用；choices；default；required；metavar；
dest；help）、位置参数、--long=value、短选项 bundling（-abc/-fval/-f=val）、
-- 分隔、长选项唯一前缀缩写、parse_args/parse_known_args、Namespace
（动态属性：直读/直写/getattr/setattr/hasattr/vars）、format_usage/
format_help/print_usage/print_help、error/exit（退出码 2，usage+错误到
stderr）、ArgumentTypeError/ArgumentError。

错误消息与 CPython 逐字对齐（expected one argument / expected N
arguments / expected at least one argument / invalid int value /
invalid choice / ignored explicit argument / ambiguous option /
unrecognized arguments / the following arguments are required /
conflicting option string）。help 排版按 CPython 默认 HelpFormatter
（帮助列 min(最长调用+2, 24)、宽度 78、options: 节名）。

语义边界：
- Namespace 的 bool/None 值在 unknown 域按裸 1/0 表示（0.324 文档化
  边界）：print(args.flag) 得 1 而非 True；用比较/条件取值，或 print
  整个 Namespace 时避开 bool 字段。
- type=自定义函数时，转换错误消息的类型名用 value（CPython 用函数
  __name__，函数值无 __name__）。
- prog 省略时取 sys.argv[0] 的 basename（同 CPython 规则；编译产物与
  脚本名不同，对拍请显式传 prog）。
- help 宽度固定 78（CPython 按终端宽度）；超长 usage 不折行。
- 互斥组/参数组/parents/fromfile_prefix_chars/prefix_chars/
  argument_default/conflict_handler/version 动作不做，桥接兜底。
- 负数选项名（-1 这类）按选项处理（CPython 按无负数选项时当值）。
"""

import sys
from os.path import basename


class ArgumentTypeError(Exception):
    pass


class ArgumentError(Exception):
    pass


class Namespace:
    """解析结果命名空间：成员存在运行期动态属性表里。"""

    def __init__(self, **kwargs: object) -> None:
        for k in kwargs:
            setattr(self, k, kwargs[k])

    def __repr__(self) -> str:
        d = vars(self)
        parts: list[str] = []
        for k in d:
            parts.append(k + "=" + repr(d[k]))
        return "Namespace(" + ", ".join(parts) + ")"

    def __contains__(self, key) -> bool:
        return hasattr(self, key)


def _type_name(conv: object) -> str:
    if conv is None:
        return "str"
    name = str(conv)
    if name == "int" or name == "float" or name == "str" or name == "bool":
        return name
    return "value"


def _convert_value(conv: object, s: str) -> object:
    if conv is None:
        return s
    name = str(conv)
    if name == "int":
        return int(s)
    if name == "float":
        return float(s)
    if name == "str":
        return s
    if name == "bool":
        return bool(s)
    return conv(s)


class _Action:
    option_strings: list[str]
    dest: str
    action: str
    nargs: object
    const: object
    default: object
    type: object
    choices: object
    required: bool
    help: str
    metavar: object

    def __init__(self, option_strings: list[str], dest: str, action: str,
                 nargs: object, const: object, default: object, type: object,
                 choices: object, required: bool, help: str,
                 metavar: object) -> None:
        self.option_strings = option_strings
        self.dest = dest
        self.action = action
        self.nargs = nargs
        self.const = const
        self.default = default
        self.type = type
        self.choices = choices
        self.required = required
        self.help = help
        self.metavar = metavar


def _looks_like_option(s: str) -> bool:
    if len(s) < 2 or s[0] != "-":
        return False
    if s[1:].isdigit():
        return False
    return True


def _is_long(s: str) -> bool:
    return len(s) > 2 and s[0] == "-" and s[1] == "-"


def _nargs_int(n: object) -> int:
    """nargs 是整数时返回其值，否则 -1。unknown 域没有 int 标签，
    isinstance(n, int) 不可用；按 str(n) 是否全数字判定（None 先排除）。"""
    if n is None:
        return -1
    s = str(n)
    if len(s) == 0:
        return -1
    i = 0
    while i < len(s):
        c = s[i]
        if c < "0" or c > "9":
            return -1
        i += 1
    return int(s)


class ArgumentParser:
    prog: str
    description: str
    epilog: str
    add_help: bool
    allow_abbrev: bool
    usage: str
    _actions: list[_Action]

    def __init__(self, prog: str = "", description: str = "", epilog: str = "",
                 add_help: bool = True, allow_abbrev: bool = True,
                 usage: str = "") -> None:
        if len(prog) == 0:
            prog = basename(sys.argv[0])
        self.prog = prog
        self.description = description
        self.epilog = epilog
        self.add_help = add_help
        self.allow_abbrev = allow_abbrev
        self.usage = usage
        self._actions = []
        if add_help:
            self.add_argument("-h", "--help", action="help",
                              help="show this help message and exit")

    # ── 注册 ──────────────────────────────────────────────────

    def add_argument(self, *name_or_flags: str, action: str = "store",
                     nargs: object = None, const: object = None,
                     default: object = None, type: object = None,
                     choices: object = None, required: bool = False,
                     help: str = "", metavar: object = None,
                     dest: str = "") -> _Action:
        if len(name_or_flags) == 0:
            raise ValueError("expected one argument")
        known = action == "store" or action == "store_true" or \
            action == "store_false" or action == "store_const" or \
            action == "append" or action == "append_const" or \
            action == "count" or action == "help"
        if not known:
            raise ValueError("unknown action '" + action + "'")
        zero_nargs = action == "store_true" or action == "store_false" or \
            action == "store_const" or action == "count" or \
            action == "help" or action == "append_const"
        if zero_nargs and not (nargs is None):
            cls = "_StoreAction"
            if action == "store_true":
                cls = "_StoreTrueAction"
            elif action == "store_false":
                cls = "_StoreFalseAction"
            elif action == "store_const":
                cls = "_StoreConstAction"
            elif action == "count":
                cls = "_CountAction"
            elif action == "help":
                cls = "_HelpAction"
            elif action == "append_const":
                cls = "_AppendConstAction"
            raise TypeError(cls + ".__init__() got an unexpected keyword argument 'nargs'")
        optional = name_or_flags[0][0] == "-"
        option_strings: list[str] = []
        d = dest
        if optional:
            for s in name_or_flags:
                if s[0] != "-":
                    raise ValueError("expected an option starting with '-'")
                for prev in self._actions:
                    for os2 in prev.option_strings:
                        if os2 == s:
                            raise ArgumentError("argument " + s +
                                                ": conflicting option string: " + s)
                option_strings.append(s)
            if len(d) == 0:
                d = option_strings[0]
                for s in option_strings:
                    if len(s) > 2 and s[0] == "-" and s[1] == "-":
                        d = s
                        break
                d = d.lstrip("-").replace("-", "_")
            if action == "store_true":
                if default is None:
                    default = False
                const = True
            elif action == "store_false":
                if default is None:
                    default = True
                const = False
            elif action == "count":
                pass
            elif action == "store_const" or action == "append_const":
                if const is None:
                    raise ValueError("const is required for action '" + action + "'")
        else:
            if len(name_or_flags) != 1:
                raise ValueError("expected one positional name")
            if len(d) == 0:
                d = name_or_flags[0]
            if action == "store_true" or action == "store_false" or \
                action == "store_const" or action == "count" or action == "help":
                raise ValueError("action '" + action + "' is not valid for positionals")
            required = True
            if not (nargs is None) and (nargs == "?" or nargs == "*"):
                required = False
        a = _Action(option_strings, d, action, nargs, const, default, type,
                    choices, required, help, metavar)
        self._actions.append(a)
        return a

    # ──  metavar / usage / help ────────────────────────────────

    def _metavar(self, a: _Action) -> str:
        if not (a.metavar is None):
            return str(a.metavar)
        if not (a.choices is None):
            ch: list[object] = a.choices
            parts: list[str] = []
            for c in ch:
                parts.append(str(c))
            return "{" + ",".join(parts) + "}"
        if len(a.option_strings) == 0:
            return a.dest
        return a.dest.upper()

    def _args_string(self, a: _Action, mv: str) -> str:
        n = a.nargs
        if n is None:
            return mv
        ni = _nargs_int(n)
        if ni >= 0:
            parts: list[str] = []
            i = 0
            while i < ni:
                parts.append(mv)
                i += 1
            return " ".join(parts)
        if n == "?":
            return "[" + mv + "]"
        if n == "*":
            return "[" + mv + " ...]"
        if n == "+":
            return mv + " [" + mv + " ...]"
        return mv

    def _usage_part(self, a: _Action) -> str:
        mv = self._metavar(a)
        if len(a.option_strings) == 0:
            n = a.nargs
            if n == "?":
                return "[" + mv + "]"
            if n == "*":
                return "[" + mv + " ...]"
            if n == "+":
                return mv + " [" + mv + " ...]"
            ni = _nargs_int(n)
            if ni >= 0 and ni != 1:
                return self._args_string(a, mv)
            return mv
        zero = a.action == "store_true" or a.action == "store_false" or \
            a.action == "store_const" or a.action == "count" or \
            a.action == "help" or a.action == "append_const"
        s = a.option_strings[0]
        if not zero:
            s = s + " " + self._args_string(a, mv)
        if a.required:
            return s
        return "[" + s + "]"

    def _usage_lines(self, parts: list[str], indent: str,
                     start_len: int) -> list[str]:
        width = 78
        lines: list[str] = []
        line: list[str] = []
        line_len = start_len
        for part in parts:
            if line_len + 1 + len(part) > width and len(line) > 0:
                lines.append(indent + " ".join(line))
                line = []
                line_len = len(indent) - 1
            line.append(part)
            line_len += len(part) + 1
        if len(line) > 0:
            lines.append(indent + " ".join(line))
        return lines

    def format_usage(self) -> str:
        if len(self.usage) > 0:
            return "usage: " + self.usage.replace("%(prog)s", self.prog) + "\n"
        opt_parts: list[str] = []
        pos_parts: list[str] = []
        for a in self._actions:
            if len(a.option_strings) > 0:
                opt_parts.append(self._usage_part(a))
            else:
                pos_parts.append(self._usage_part(a))
        prog = self.prog
        full = prog
        for p in opt_parts:
            full += " " + p
        for p in pos_parts:
            full += " " + p
        if 7 + len(full) <= 78:
            return "usage: " + full + "\n"
        lines: list[str] = []
        if 7 + len(prog) <= 58:
            indent = " " * (7 + len(prog) + 1)
            first: list[str] = [prog]
            first.extend(opt_parts)
            lines = self._usage_lines(first, indent, 6)
            if len(lines) > 0:
                lines[0] = "usage: " + lines[0][len(indent):]
            lines.extend(self._usage_lines(pos_parts, indent, len(indent) - 1))
        else:
            indent = " " * 7
            allp: list[str] = []
            allp.extend(opt_parts)
            allp.extend(pos_parts)
            wrapped = self._usage_lines(allp, indent, 6)
            lines.append(prog)
            lines.extend(wrapped)
            lines[0] = "usage: " + lines[0]
        return "\n".join(lines) + "\n"

    def _invocation(self, a: _Action) -> str:
        if len(a.option_strings) == 0:
            return self._metavar(a)
        s = ", ".join(a.option_strings)
        zero = a.action == "store_true" or a.action == "store_false" or \
            a.action == "store_const" or a.action == "count" or \
            a.action == "help" or a.action == "append_const"
        if not zero:
            s += " " + self._args_string(a, self._metavar(a))
        return s

    def _fill(self, text: str, width: int, indent: str) -> str:
        words = text.split()
        if len(words) == 0:
            return ""
        lines: list[str] = []
        cur = indent + words[0]
        i = 1
        while i < len(words):
            if len(cur) + 1 + len(words[i]) > width:
                lines.append(cur)
                cur = indent + words[i]
            else:
                cur += " " + words[i]
            i += 1
        lines.append(cur)
        return "\n".join(lines)

    def format_help(self) -> str:
        width = 78
        out = self.format_usage()
        if len(self.description) > 0:
            out += "\n" + self._fill(self.description, width, "") + "\n"
        positionals: list[_Action] = []
        optionals: list[_Action] = []
        for a in self._actions:
            if len(a.option_strings) == 0:
                positionals.append(a)
            else:
                optionals.append(a)
        maxlen = 0
        for a in self._actions:
            inv = self._invocation(a)
            if len(inv) > maxlen:
                maxlen = len(inv)
        help_pos = maxlen + 4
        if help_pos > 24:
            help_pos = 24
        if len(positionals) > 0:
            out += "\npositional arguments:\n"
            for a in positionals:
                out += self._help_entry(a, help_pos, width)
        if len(optionals) > 0:
            out += "\noptions:\n"
            for a in optionals:
                out += self._help_entry(a, help_pos, width)
        if len(self.epilog) > 0:
            out += "\n" + self._fill(self.epilog, width, "") + "\n"
        return out

    def _help_entry(self, a: _Action, help_pos: int, width: int) -> str:
        inv = self._invocation(a)
        header = "  " + inv
        if len(a.help) == 0:
            return header + "\n"
        if len(header) + 2 <= help_pos:
            pad = help_pos - len(header)
            spaces = ""
            i = 0
            while i < pad:
                spaces += " "
                i += 1
            first = header + spaces
        else:
            first = header + "\n" + " " * help_pos
        help_width = width - help_pos
        if help_width < 10:
            help_width = 10
        wrapped = self._fill(a.help, help_width, "")
        # 首行接在 header 后，后续行按 help_pos 缩进
        nl = wrapped.find("\n")
        if nl < 0:
            return first + wrapped + "\n"
        out = first + wrapped[:nl] + "\n"
        rest = wrapped[nl + 1:]
        for ln in rest.split("\n"):
            out += " " * help_pos + ln + "\n"
        return out

    def print_usage(self, f: file = None) -> None:
        if f is None:
            f = sys.stdout
        f.write(self.format_usage())

    def print_help(self, f: file = None) -> None:
        if f is None:
            f = sys.stdout
        f.write(self.format_help())

    # ── 错误 / 退出 ────────────────────────────────────────────

    def exit(self, status: int = 0, message: str = "") -> None:
        if len(message) > 0:
            sys.stderr.write(message)
        sys.exit(status)

    def error(self, message: str) -> None:
        self.print_usage(sys.stderr)
        self.exit(2, self.prog + ": error: " + message + "\n")

    # ── 解析 ──────────────────────────────────────────────────

    def _match_long(self, name: str) -> Optional[_Action]:
        exact: list[_Action] = []
        prefix: list[_Action] = []
        for a in self._actions:
            for s in a.option_strings:
                if s == name:
                    exact.append(a)
                elif self.allow_abbrev and len(s) > len(name) and s[:len(name)] == name:
                    prefix.append(a)
        if len(exact) > 0:
            return exact[0]
        if len(prefix) == 1:
            return prefix[0]
        if len(prefix) > 1:
            names: list[str] = []
            for a in prefix:
                for s in a.option_strings:
                    if len(s) > len(name) and s[:len(name)] == name:
                        names.append(s)
            self.error("ambiguous option: " + name + " could match " + ", ".join(names))
        return None

    def _check_choices(self, a: _Action, name: str, value: object) -> None:
        if a.choices is None:
            return
        ch: list[object] = a.choices
        if value in ch:
            return
        parts: list[str] = []
        for c in ch:
            parts.append(str(c))
        self.error("argument " + name + ": invalid choice: " + repr(value) +
                   " (choose from " + ", ".join(parts) + ")")

    def _convert(self, a: _Action, name: str, s: str) -> object:
        # type 省略或 str：值本身就是 str，不走 object 返回的转换路径
        # （leaf str × unknown 返回的 RC 机制见 docs/stdlib-batch1.md 已知问题）。
        if a.type is None:
            self._check_choices(a, name, s)
            return s
        if str(a.type) == "str":
            self._check_choices(a, name, s)
            return s
        try:
            v = _convert_value(a.type, s)
        except ArgumentTypeError as e:
            self.error("argument " + name + ": " + str(e))
            return None
        except (ValueError, TypeError):
            self.error("argument " + name + ": invalid " + _type_name(a.type) +
                       " value: " + repr(s))
            return None
        self._check_choices(a, name, v)
        return v

    def _store(self, a: _Action, ns: Namespace, value: object) -> None:
        if a.action == "store" or a.action == "store_const" or a.action == "help":
            setattr(ns, a.dest, value)
        elif a.action == "append" or a.action == "append_const":
            cur = getattr(ns, a.dest)
            items: list[object] = []
            if not (cur is None):
                items = list(cur)
            items.append(value)
            setattr(ns, a.dest, items)
        elif a.action == "count":
            cur = getattr(ns, a.dest)
            n = 0
            if not (cur is None):
                n = cur
            setattr(ns, a.dest, n + 1)
        else:
            setattr(ns, a.dest, value)

    def _take_values(self, a: _Action, name: str, argv: list[str], i: int,
                     explicit: object, only_pos: bool) -> tuple[int, object]:
        """从 argv[i:] 取值；返回 (新下标, 值)。值按 nargs 口径。"""
        n = a.nargs
        zero = a.action == "store_true" or a.action == "store_false" or \
            a.action == "store_const" or a.action == "count" or \
            a.action == "help" or a.action == "append_const"
        if zero:
            if not (explicit is None):
                self.error("argument " + name + ": ignored explicit argument " +
                           repr(explicit))
            if a.action == "store_true":
                return (i, True)
            if a.action == "store_false":
                return (i, False)
            if a.action == "count":
                return (i, 1)
            return (i, a.const)
        if not (explicit is None):
            v = self._convert(a, name, explicit)
            if n == "*" or n == "+" or (_nargs_int(n) >= 0 and _nargs_int(n) != 1):
                vs: list[object] = [v]
                return (i, vs)
            return (i, v)
        # 从后续 token 收集
        vals: list[str] = []
        j = i
        if n is None:
            if j >= len(argv) or (not only_pos and _looks_like_option(argv[j])):
                self.error("argument " + name + ": expected one argument")
            vals.append(argv[j])
            j += 1
            return (j, self._convert(a, name, vals[0]))
        ni = _nargs_int(n)
        if ni >= 0:
            k = 0
            while k < ni:
                if j >= len(argv) or (not only_pos and _looks_like_option(argv[j])):
                    word = " argument"
                    if ni != 1:
                        word = " arguments"
                    self.error("argument " + name + ": expected " + str(ni) + word)
                vals.append(argv[j])
                j += 1
                k += 1
            out: list[object] = []
            for s in vals:
                out.append(self._convert(a, name, s))
            return (j, out)
        if n == "?":
            if j < len(argv) and (only_pos or not _looks_like_option(argv[j])):
                v = self._convert(a, name, argv[j])
                return (j + 1, v)
            return (j, a.const)
        if n == "*":
            while j < len(argv) and (only_pos or not _looks_like_option(argv[j])):
                vals.append(argv[j])
                j += 1
            out2: list[object] = []
            for s in vals:
                out2.append(self._convert(a, name, s))
            return (j, out2)
        if n == "+":
            if j >= len(argv) or (not only_pos and _looks_like_option(argv[j])):
                self.error("argument " + name + ": expected at least one argument")
            while j < len(argv) and (only_pos or not _looks_like_option(argv[j])):
                vals.append(argv[j])
                j += 1
            out3: list[object] = []
            for s in vals:
                out3.append(self._convert(a, name, s))
            return (j, out3)
        return (i, None)

    def parse_known_args(self, argv: Optional[list[str]] = None) -> tuple[Namespace, list[str]]:
        args: list[str] = []
        if argv is None:
            args = sys.argv[1:]
        else:
            args = argv
        ns = Namespace()
        for a in self._actions:
            if a.action == "help":
                continue
            if not hasattr(ns, a.dest):
                setattr(ns, a.dest, a.default)
        positionals: list[_Action] = []
        for a in self._actions:
            if len(a.option_strings) == 0:
                positionals.append(a)
        seen: list[str] = []
        extras: list[str] = []
        i = 0
        pos_idx = 0
        only_pos = False
        while i < len(args):
            tok = args[i]
            if not only_pos and tok == "--":
                only_pos = True
                i += 1
                continue
            if not only_pos and _looks_like_option(tok):
                if _is_long(tok):
                    eq = tok.find("=")
                    name = tok
                    explicit: object = None
                    if eq >= 0:
                        name = tok[:eq]
                        explicit = tok[eq + 1:]
                    a = self._match_long(name)
                    if a is None:
                        extras.append(tok)
                        i += 1
                        continue
                    if a.action == "help":
                        self.print_help()
                        self.exit(0)
                    j, v = self._take_values(a, name, args, i + 1, explicit, only_pos)
                    self._store(a, ns, v)
                    seen.append(a.dest)
                    i = j
                else:
                    # 短选项：-abc bundling / -fval / -f=val / -f val
                    body = tok[1:]
                    eq = body.find("=")
                    if eq >= 0:
                        head = body[:eq]
                        rest_val: object = body[eq + 1:]
                    else:
                        head = body
                        rest_val = None
                    k = 0
                    consumed_next = False
                    while k < len(head):
                        opt = "-" + head[k]
                        a = None
                        for act in self._actions:
                            for s in act.option_strings:
                                if s == opt:
                                    a = act
                        if a is None:
                            extras.append(tok)
                            break
                        if a.action == "help":
                            self.print_help()
                            self.exit(0)
                        zero = a.action == "store_true" or a.action == "store_false" or \
                            a.action == "store_const" or a.action == "count" or \
                            a.action == "append_const"
                        if zero:
                            j, v = self._take_values(a, opt, args, i + 1, None, only_pos)
                            self._store(a, ns, v)
                            seen.append(a.dest)
                            k += 1
                            continue
                        #  takes value：bundling 剩余部分或 = 后的部分是值
                        attached: object = None
                        if k + 1 < len(head):
                            attached = head[k + 1:]
                        elif not (rest_val is None):
                            attached = rest_val
                        j = i + 1
                        if not (attached is None):
                            j, v = self._take_values(a, opt, args, i + 1, attached, only_pos)
                        else:
                            j, v = self._take_values(a, opt, args, i + 1, None, only_pos)
                        self._store(a, ns, v)
                        seen.append(a.dest)
                        consumed_next = j > i + 1
                        break
                    if consumed_next:
                        i = j
                    else:
                        i += 1
                continue
            # 位置参数
            if pos_idx < len(positionals):
                a = positionals[pos_idx]
                n = a.nargs
                if n is None:
                    v = self._convert(a, a.dest, tok)
                    self._store(a, ns, v)
                    seen.append(a.dest)
                    pos_idx += 1
                    i += 1
                    continue
                if n == "?":
                    v = self._convert(a, a.dest, tok)
                    self._store(a, ns, v)
                    seen.append(a.dest)
                    pos_idx += 1
                    i += 1
                    continue
                # * / + / N：连续吃非选项 token
                vals: list[str] = []
                j = i
                while j < len(args) and (only_pos or not _looks_like_option(args[j])):
                    vals.append(args[j])
                    j += 1
                ni = _nargs_int(n)
                if ni >= 0:
                    if len(vals) < ni:
                        word = " argument"
                        if ni != 1:
                            word = " arguments"
                        self.error("argument " + a.dest + ": expected " + str(ni) + word)
                    vals = vals[:ni]
                    j = i + ni
                elif n == "+" and len(vals) == 0:
                    self.error("argument " + a.dest + ": expected at least one argument")
                out: list[object] = []
                for s in vals:
                    out.append(self._convert(a, a.dest, s))
                self._store(a, ns, out)
                seen.append(a.dest)
                pos_idx += 1
                i = j
                continue
            extras.append(tok)
            i += 1
        # 未出现的位置参数：按 nargs 补默认
        while pos_idx < len(positionals):
            a = positionals[pos_idx]
            n = a.nargs
            if n == "?":
                if not (a.default is None):
                    setattr(ns, a.dest, a.default)
                else:
                    setattr(ns, a.dest, a.const)
            elif n == "*":
                if not (a.default is None):
                    setattr(ns, a.dest, a.default)
                else:
                    setattr(ns, a.dest, [])
            pos_idx += 1
        # required 检查
        missing: list[str] = []
        for a in self._actions:
            if a.required and a.dest not in seen:
                if len(a.option_strings) > 0:
                    missing.append(a.option_strings[0])
                else:
                    missing.append(a.dest)
        if len(missing) > 0:
            self.error("the following arguments are required: " + ", ".join(missing))
        return (ns, extras)

    def parse_args(self, args: Optional[list[str]] = None) -> Namespace:
        ns, extras = self.parse_known_args(args)
        if len(extras) > 0:
            self.error("unrecognized arguments: " + " ".join(extras))
        return ns
