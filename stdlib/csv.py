"""csv 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

reader(f) 一次读完整个文件，返回 list[list[str]]；writer(f) 是带
writerow / writerows 的类。支持 delimiter / quotechar / quoting /
lineterminator，不建模 dialect、DictReader / DictWriter。
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

    def writerow(self, row: list[str]) -> None:
        parts: list[str] = []
        i: int = 0
        while i < len(row):
            field: str = row[i]
            need: bool = self.quoting == QUOTE_ALL or self.quoting == QUOTE_NONNUMERIC
            if not need:
                if self.delimiter in field or self.quotechar in field:
                    need = True
                elif "\r" in field or "\n" in field:
                    need = True
            if need:
                if self.quoting == QUOTE_NONE:
                    raise Error("need to escape delimiter or quotechar, "
                                "but quoting is QUOTE_NONE")
                body: str = field.replace(self.quotechar,
                                          self.quotechar + self.quotechar)
                parts.append(self.quotechar + body + self.quotechar)
            else:
                parts.append(field)
            i += 1
        self.f.write(self.delimiter.join(parts) + self.lineterminator)

    def writerows(self, rows: list[list[str]]) -> None:
        i: int = 0
        while i < len(rows):
            self.writerow(rows[i])
            i += 1
