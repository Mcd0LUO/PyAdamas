"""calendar —— 日历计算与文本日历（纯 pyadamas 源码，stdlib3）。

覆盖 isleap / leapdays / weekday / monthrange / monthcalendar / timegm、
day_name/day_abbr/month_name/month_abbr、weekheader、firstweekday /
setfirstweekday、month / prmonth / calendar / prcal，以及 error /
IllegalMonthError / IllegalWeekdayError / mdays。

语义边界（轻量标尺，见 docs/stdlib-batch3.md）：
- 不依赖 datetime：星期与 timegm 走公历日期→儒略日式的整数算法（Howard
  Hinnant days_from_civil），与 CPython 对 1..9999 年的结果一致；
- day_name/day_abbr/month_name/month_abbr 是普通 list（CPython 是本地化
  只读对象），len/下标/切片可用，无 locale 切换；
- 没有 Calendar/TextCalendar/HTMLCalendar 类：month / calendar / weekheader
  等按模块级函数提供（它们本来也只是 TextCalendar 方法的别名）；
- IllegalMonthError 只继承 ValueError（pa 的自定义异常类不支持多继承，
  CPython 的 ValueError+IndexError 双基类做不到）。
"""

mdays: list[int] = [0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]

day_name: list[str] = ["Monday", "Tuesday", "Wednesday", "Thursday",
                       "Friday", "Saturday", "Sunday"]
day_abbr: list[str] = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]

month_name: list[str] = ["", "January", "February", "March", "April", "May",
                         "June", "July", "August", "September", "October",
                         "November", "December"]
month_abbr: list[str] = ["", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul",
                         "Aug", "Sep", "Oct", "Nov", "Dec"]

error = ValueError

# 星期常量（CPython calendar.MONDAY..SUNDAY，周一=0）。
MONDAY: int = 0
TUESDAY: int = 1
WEDNESDAY: int = 2
THURSDAY: int = 3
FRIDAY: int = 4
SATURDAY: int = 5
SUNDAY: int = 6

_MINYEAR = 1
_MAXYEAR = 9999


class IllegalMonthError(ValueError):
    """月份不在 1..12。"""


class IllegalWeekdayError(ValueError):
    """星期不在 0..6。"""


_firstweekday = 0


def firstweekday() -> int:
    """当前每周第一天（0=周一）。"""
    return _firstweekday


def setfirstweekday(firstweekday: int) -> None:
    """设置每周第一天（0=周一 .. 6=周日）。"""
    global _firstweekday
    if firstweekday < 0 or firstweekday > 6:
        raise IllegalWeekdayError(
            "bad weekday number " + str(firstweekday) +
            "; must be 0 (Monday) to 6 (Sunday)")
    _firstweekday = firstweekday


def isleap(year: int) -> bool:
    """闰年判定。"""
    return year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)


def leapdays(y1: int, y2: int) -> int:
    """[y1, y2) 区间内的闰年数。"""
    a = y1 - 1
    b = y2 - 1
    return (b // 4 - a // 4) - (b // 100 - a // 100) + (b // 400 - a // 400)


def _days_from_civil(y: int, m: int, d: int) -> int:
    """公历日期到 1970-01-01 的天数（可正可负）。"""
    yy = y
    if m <= 2:
        yy = y - 1
    era = yy // 400
    yoe = yy - era * 400
    mp = (m + 9) % 12
    doy = (153 * mp + 2) // 5 + d - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468


def _monthlen(year: int, month: int) -> int:
    base = mdays[month]
    if month == 2 and isleap(year):
        return base + 1
    return base


def _validate_month(month: int) -> None:
    if month < 1 or month > 12:
        raise IllegalMonthError(
            "bad month number " + str(month) + "; must be 1-12")


def weekday(year: int, month: int, day: int) -> int:
    """返回星期（0=周一 .. 6=周日）。"""
    y = year
    if y < _MINYEAR or y > _MAXYEAR:
        y = 2000 + y % 400
    _validate_month(month)
    limit = _monthlen(y, month)
    if day < 1 or day > limit:
        raise ValueError("day is out of range for month")
    n = _days_from_civil(y, month, day)
    # 1970-01-01 是周四：n=0 -> 3
    return (n + 3) % 7


def monthrange(year: int, month: int) -> tuple[int, int]:
    """返回 (该月 1 号的星期, 该月天数)。"""
    _validate_month(month)
    day1 = weekday(year, month, 1)
    return day1, _monthlen(year, month)


def _monthdays(year: int, month: int, first: int) -> list[int]:
    """该月的日历网格（扁平，7 个一行，月外补 0）。"""
    day1, ndays = monthrange(year, month)
    out: list[int] = []
    before = (day1 - first) % 7
    i = 0
    while i < before:
        out.append(0)
        i += 1
    d = 1
    while d <= ndays:
        out.append(d)
        d += 1
    after = (first - day1 - ndays) % 7
    i = 0
    while i < after:
        out.append(0)
        i += 1
    return out


def monthcalendar(year: int, month: int) -> list[list[int]]:
    """该月的星期矩阵：每行 7 个日号，月外为 0。"""
    flat = _monthdays(year, month, _firstweekday)
    rows: list[list[int]] = []
    i = 0
    while i + 7 <= len(flat):
        row: list[int] = []
        j = 0
        while j < 7:
            row.append(flat[i + j])
            j += 1
        rows.append(row)
        i += 7
    return rows


def timegm(tm: tuple[int, ...]) -> int:
    """把 UTC 时间元组（年, 月, 日, 时, 分, 秒）换算成 Unix 时间戳。"""
    year = tm[0]
    month = tm[1]
    day = tm[2]
    hour = tm[3]
    minute = tm[4]
    second = tm[5]
    days = _days_from_civil(year, month, 1) + day - 1
    hours = days * 24 + hour
    minutes = hours * 60 + minute
    return minutes * 60 + second


def _center(s: str, width: int) -> str:
    """CPython str.center 的补白规则（奇数补白多的一侧在左）。"""
    marg = width - len(s)
    if marg <= 0:
        return s
    left = marg // 2
    if (marg & width & 1) != 0:
        left = left + 1
    return " " * left + s + " " * (marg - left)


def formatweekday(day: int, width: int) -> str:
    """一天的名字按 width 居中（width>=9 用全名，否则缩写）。"""
    names = day_abbr
    if width >= 9:
        names = day_name
    return _center(names[day][:width], width)


def formatweekheader(width: int) -> str:
    """周标题行。"""
    return " ".join(formatweekday(i, width) for i in range(7))


def weekheader(width: int) -> str:
    """周标题行（模块级入口）。"""
    return formatweekheader(width)


def formatweekheader_from(first: int, width: int) -> str:
    """从 first 开始的周标题（firstweekday 非 0 时用）。"""
    out: list[str] = []
    i = 0
    while i < 7:
        d = (first + i) % 7
        out.append(formatweekday(d, width))
        i += 1
    return " ".join(out)


def formatmonthname(year: int, month: int, width: int, withyear: bool) -> str:
    """月份标题。"""
    name = month_name[month]
    if withyear:
        name = name + " " + str(year)
    return _center(name, width)


def formatday(day: int, width: int) -> str:
    if day == 0:
        return " " * width
    # CPython 先把日号右对齐到 2 位再按 width 居中
    return _center(str(day).rjust(2), width)


def formatweek(week: list[int], width: int) -> str:
    out: list[str] = []
    for d in week:
        out.append(formatday(d, width))
    return " ".join(out)


def formatstring(cols: list[str], colwidth: int, spacing: int) -> str:
    sep = " " * spacing
    parts: list[str] = []
    for c in cols:
        parts.append(_center(c, colwidth))
    return sep.join(parts)


def formatmonth(year: int, month: int, w: int = 0, l: int = 0) -> str:
    """该月的多行文本日历。"""
    width = w
    if width < 2:
        width = 2
    lines = l
    if lines < 1:
        lines = 1
    head = formatmonthname(year, month, 7 * (width + 1) - 1, True).rstrip()
    # 缓冲用 list[str] + join（逐行 `out = out + ...` 是二次曲线）
    parts: list[str] = [head + "\n" * lines]
    parts.append(formatweekheader(width).rstrip() + "\n" * lines)
    rows = monthcalendar(year, month)
    for row in rows:
        parts.append(formatweek(row, width).rstrip() + "\n" * lines)
    return "".join(parts)


def month(year: int, month: int, w: int = 0, l: int = 0) -> str:
    """该月的多行文本日历（模块级入口）。"""
    return formatmonth(year, month, w, l)


def prmonth(year: int, month: int, w: int = 0, l: int = 0) -> None:
    """打印该月的文本日历。"""
    print(formatmonth(year, month, w, l), end="")


def formatyear(year: int, w: int = 2, l: int = 1, c: int = 6,
               m: int = 3) -> str:
    """整年的多行文本日历（m 个月一行，列间距 c）。"""
    width = w
    if width < 2:
        width = 2
    lines = l
    if lines < 1:
        lines = 1
    spacing = c
    if spacing < 2:
        spacing = 2
    cols = m
    if cols < 1:
        cols = 1
    colwidth = (width + 1) * 7 - 1
    parts: list[str] = []
    parts.append(_center(str(year), colwidth * cols + spacing * (cols - 1)).rstrip())
    parts.append("\n" * lines)
    header = formatweekheader(width)
    nrows = (12 + cols - 1) // cols
    ri = 0
    while ri < nrows:
        start = cols * ri + 1
        end = cols * (ri + 1) + 1
        if end > 13:
            end = 13
        parts.append("\n" * lines)
        names: list[str] = []
        k = start
        while k < end:
            names.append(formatmonthname(year, k, colwidth, False))
            k += 1
        parts.append(formatstring(names, colwidth, spacing).rstrip())
        parts.append("\n" * lines)
        headers: list[str] = []
        k = start
        while k < end:
            headers.append(header)
            k += 1
        parts.append(formatstring(headers, colwidth, spacing).rstrip())
        parts.append("\n" * lines)
        cals: list[list[int]] = []
        k = start
        while k < end:
            cals.append(_monthdays(year, k, _firstweekday))
            k += 1
        height = 0
        for cal in cals:
            h = len(cal) // 7
            if h > height:
                height = h
        j = 0
        while j < height:
            weeks: list[str] = []
            for cal in cals:
                if j * 7 >= len(cal):
                    weeks.append("")
                else:
                    weeks.append(formatweek(cal[j * 7:(j + 1) * 7], width))
            parts.append(formatstring(weeks, colwidth, spacing).rstrip())
            parts.append("\n" * lines)
            j += 1
        ri += 1
    return "".join(parts)


def calendar(year: int, w: int = 2, l: int = 1, c: int = 6,
             m: int = 3) -> str:
    """整年的多行文本日历（模块级入口）。"""
    return formatyear(year, w, l, c, m)


def prcal(year: int, w: int = 2, l: int = 1, c: int = 6, m: int = 3) -> None:
    """打印整年的文本日历。"""
    print(formatyear(year, w, l, c, m), end="")
