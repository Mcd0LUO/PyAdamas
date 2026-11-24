"""csv 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

reader(f) 一次读完整个文件，返回 list[list[str]]；writer(f) 是带
writerow / writerows 的类。支持 delimiter / quotechar / quoting /
lineterminator。

DictReader 是**函数**而不是类：返回 list[dict[str, str]]，因此
`for row in csv.DictReader(f)` 和 `rows = list(csv.DictReader(f))` 都能用；
但拿不到 .fieldnames 属性，也不支持显式 fieldnames（默认参数不能是 list）。
DictWriter 是类，需要显式 fieldnames。

本次新增（保持旧 API 不破坏）：
- DictReader(f) 的 f 除 file 对象外也接受纯 str 文本（运行期 isinstance 分派）；
- DictWriterText(fieldnames) 内存版 DictWriter，writeheader/writerow/writerows/
  getvalue，getvalue() 返回拼接好的 CSV 文本（行终止符默认 "\r\n"）。
"""

QUOTE_MINIMAL = 0
QUOTE_ALL = 1
QUOTE_NONNUMERIC = 2
QUOTE_NONE = 3


class Error(ValueError):
    pass


def _parse(text: str, delimiter: str, quotechar: str) -> list[list[str]]:
    rows: list[list[str]] = []
    row: list[str] = []
    buf: list[str] = []
    i: int = 0
    n: int = len(text)
    field_started: bool = False
    quoted: bool = False
    while i < n:
        c: str = text[i]
        if quoted:
            if c == quotechar:
                if i + 1 < n and text[i + 1] == quotechar:
                    buf.append(quotechar)
                    i += 2
                    continue
                quoted = False
                i += 1
                continue
            buf.append(c)
            i += 1
            continue
        if c == quotechar and not field_started:
            quoted = True
            field_started = True
            i += 1
            continue
        if c == delimiter:
            row.append("".join(buf))
            buf = []
            field_started = False
            i += 1
            continue
        if c == "\n" or c == "\r":
            if field_started or len(buf) > 0 or len(row) > 0:
                row.append("".join(buf))
            rows.append(row)
            row = []
            buf = []
            field_started = False
            i += 1
            if c == "\r" and i < n and text[i] == "\n":
                i += 1
            continue
        buf.append(c)
        field_started = True
        i += 1
    if quoted:
        raise Error("unexpected end of data")
    if field_started or len(buf) > 0 or len(row) > 0:
        row.append("".join(buf))
        rows.append(row)
    return rows


def reader(f: file, delimiter: str = ",", quotechar: str = '"') -> list[list[str]]:
    """把文件内容解析成行列表（eager；只接受 file 对象）。"""
    if len(delimiter) != 1:
        raise Error("delimiter must be a 1-character string")
    if len(quotechar) != 1:
        raise Error("quotechar must be a 1-character string")
    text: str = f.read()
    return _parse(text, delimiter, quotechar)


class writer:
    def __init__(self, f: file, delimiter: str = ",", quotechar: str = '"',
                 quoting: int = 0,
                 lineterminator: str = "\r\n") -> None:
        if len(delimiter) != 1:
            raise Error("delimiter must be a 1-character string")
        if len(quotechar) != 1:
            raise Error("quotechar must be a 1-character string")
        self.f = f
        self.delimiter = delimiter
        self.quotechar = quotechar
        self.quoting = quoting
        self.lineterminator = lineterminator

    def writerow(self, row: list[object]) -> None:
        # CPython 的 csv.writer 接受任意可迭代对象，逐字段 str()；这里收
        # list[object]，用 str(row[i]) 做运行期分派（int/str 都正确）。
        # 0.322 之前签名是 list[str]，收 list[unknown] 的混合行时按 str
        # 指针读 int 元素，直接段错误。
        parts: list[str] = []
        # 引号判定只依赖实例字段：提到循环外，避免每字段重复取属性 + 重算
        quoting: int = self.quoting
        delim: str = self.delimiter
        qc: str = self.quotechar
        need_all: bool = quoting == QUOTE_ALL or quoting == QUOTE_NONNUMERIC
        qc2: str = qc + qc
        n: int = len(row)
        i: int = 0
        while i < n:
            field: str = str(row[i])
            need: bool = need_all
            if not need:
                if delim in field or qc in field:
                    need = True
                elif "\r" in field or "\n" in field:
                    need = True
            if need:
                if quoting == QUOTE_NONE:
                    raise Error("need to escape delimiter or quotechar, "
                                "but quoting is QUOTE_NONE")
                body: str = field.replace(qc, qc2)
                parts.append(qc + body + qc)
            else:
                parts.append(field)
            i += 1
        self.f.write(delim.join(parts) + self.lineterminator)

    def writerows(self, rows: list[list[object]]) -> None:
        i: int = 0
        while i < len(rows):
            self.writerow(rows[i])
            i += 1


class DictWriter:
    """按 fieldnames 把 dict 写成行；缺字段写空串。"""

    fieldnames: list[str]
    _w: writer

    def __init__(self, f: file, fieldnames: list[str], delimiter: str = ",",
                 quotechar: str = '"', quoting: int = 0,
                 lineterminator: str = "\r\n") -> None:
        self.fieldnames = fieldnames
        self._w = writer(f, delimiter, quotechar, quoting, lineterminator)

    def writeheader(self) -> None:
        self._w.writerow(self.fieldnames)

    def writerow(self, row: dict[str, str]) -> None:
        vals: list[str] = []
        for k in self.fieldnames:
            if k in row:
                vals.append(row[k])
            else:
                vals.append("")
        self._w.writerow(vals)

    def writerows(self, rows: list[dict[str, str]]) -> None:
        for row in rows:
            self.writerow(row)


def DictReader(f: unknown, delimiter: str = ",",
               quotechar: str = '"') -> list[dict[str, str]]:
    """把首行当表头，返回 list[dict[str, str]]（eager）。

    保持原有「传 file 对象」调用方式；同时接受纯 str 文本（本次新增），
    用 isinstance 在运行期分派。两种形式输出逐字一致。
    """
    if isinstance(f, str):
        table = _parse(f, delimiter, quotechar)
    else:
        table = reader(f, delimiter, quotechar)
    rows: list[dict[str, str]] = []
    if len(table) == 0:
        return rows
    header = table[0]
    i = 1
    nrow: int = len(table)
    nh: int = len(header)
    while i < nrow:
        src = table[i]
        d: dict[str, str] = {}
        j = 0
        while j < nh:
            key = header[j]
            if j < len(src):
                d[key] = src[j]
            else:
                d[key] = ""
            j += 1
        rows.append(d)
        i += 1
    return rows


class DictWriterText:
    """内存版 DictWriter：只收 fieldnames，用 getvalue() 取拼接文本。

    与现有 DictWriter(f, fieldnames) 并存，不破坏旧 API。writeheader /
    writerow / writerows 语义与 DictWriter 一致；getvalue() 返回已写入
    的全部文本（行终止符默认为 CPython csv 的 "\r\n"）。
    内部用 list[str] 累积（list 默认值在 pa 编译不过，故在 __init__ 里建）。
    """

    fieldnames: list[str]
    delimiter: str
    quotechar: str
    quoting: int
    lineterminator: str
    _rows: list[str]

    def __init__(self, fieldnames: list[str], delimiter: str = ",",
                 quotechar: str = '"', quoting: int = 0,
                 lineterminator: str = "\r\n") -> None:
        self.fieldnames = fieldnames
        self.delimiter = delimiter
        self.quotechar = quotechar
        self.quoting = quoting
        self.lineterminator = lineterminator
        self._rows = []

    def _format(self, vals: list[str]) -> str:
        parts: list[str] = []
        quoting: int = self.quoting
        delim: str = self.delimiter
        qc: str = self.quotechar
        need_all: bool = quoting == QUOTE_ALL or quoting == QUOTE_NONNUMERIC
        qc2: str = qc + qc
        n: int = len(vals)
        i: int = 0
        while i < n:
            field: str = vals[i]
            need: bool = need_all
            if not need:
                if delim in field or qc in field:
                    need = True
                elif "\r" in field or "\n" in field:
                    need = True
            if need:
                if quoting == QUOTE_NONE:
                    raise Error("need to escape delimiter or quotechar, "
                                "but quoting is QUOTE_NONE")
                body: str = field.replace(qc, qc2)
                parts.append(qc + body + qc)
            else:
                parts.append(field)
            i += 1
        return delim.join(parts) + self.lineterminator

    def writeheader(self) -> None:
        self._rows.append(self._format(self.fieldnames))

    def writerow(self, row: dict[str, str]) -> None:
        vals: list[str] = []
        for k in self.fieldnames:
            if k in row:
                vals.append(row[k])
            else:
                vals.append("")
        self._rows.append(self._format(vals))

    def writerows(self, rows: list[dict[str, str]]) -> None:
        for row in rows:
            self.writerow(row)

    def getvalue(self) -> str:
        return "".join(self._rows)
