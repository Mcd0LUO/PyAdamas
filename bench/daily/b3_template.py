"""b3_template -- 字符串处理型：模板渲染引擎。

实现 {{key}} 占位符替换 + 行区块循环，渲染一张发票报表：
表头标量替换、明细行循环、金额格式化、合计。大量字符串拼接。
"""
import re

PLACEHOLDER = re.compile(r"\{\{(\w+)\}\}")


def lcg(state: int) -> int:
    return (state * 1664525 + 1013904223) % 4294967296


def render_scalar(tmpl: str, ctx: dict[str, str]) -> str:
    out: list[str] = []
    i: int = 0
    n: int = len(tmpl)
    while i < n:
        if tmpl[i] == "{" and i + 1 < n and tmpl[i + 1] == "{":
            end: int = tmpl.find("}}", i + 2)
            if end < 0:
                out.append(tmpl[i:])
                break
            key: str = tmpl[i + 2:end].strip()
            out.append(ctx.get(key, ""))
            i = end + 2
        else:
            out.append(tmpl[i])
            i += 1
    return "".join(out)


def money(cents: int) -> str:
    sign: str = "-" if cents < 0 else ""
    c: int = abs(cents)
    whole: int = c // 100
    frac: int = c % 100
    return sign + str(whole) + "." + str(frac).zfill(2)


def main() -> None:
    products: list[str] = ["widget", "gadget", "sprocket", "flange", "bearing",
                           "washer", "bolt", "nut", "bracket", "hinge"]
    rows: list[dict[str, str]] = []
    state: int = 55555
    total_cents: int = 0
    for i in range(500):
        state = lcg(state)
        prod: str = products[state % len(products)]
        state = lcg(state)
        qty: int = 1 + state % 20
        state = lcg(state)
        price_cents: int = 50 + state % 9950
        line_cents: int = qty * price_cents
        total_cents += line_cents
        row: dict[str, str] = {
            "no": str(i + 1),
            "product": prod,
            "qty": str(qty),
            "price": money(price_cents),
            "line": money(line_cents),
        }
        rows.append(row)
    tax_cents: int = total_cents * 8 // 100
    grand_cents: int = total_cents + tax_cents
    ctx: dict[str, str] = {
        "invoice": "INV-2026-0042",
        "customer": "Acme Industrial Supply",
        "date": "2026-11-19",
        "subtotal": money(total_cents),
        "tax": money(tax_cents),
        "grand": money(grand_cents),
        "count": str(len(rows)),
    }
    header: str = (
        "Invoice {{invoice}}\n"
        "Customer: {{customer}}\n"
        "Date: {{date}}\n"
        "Lines: {{count}}\n"
        "----------------------------------------\n"
    )
    line_tmpl: str = "{{no}}. {{product}} x{{qty}} @ {{price}} = {{line}}\n"
    footer: str = (
        "----------------------------------------\n"
        "Subtotal: {{subtotal}}\n"
        "Tax (8%): {{tax}}\n"
        "Grand total: {{grand}}\n"
    )
    parts: list[str] = []
    parts.append(render_scalar(header, ctx))
    for row in rows:
        parts.append(render_scalar(line_tmpl, row))
    parts.append(render_scalar(footer, ctx))
    doc: str = "".join(parts)
    print(doc, end="")
    print("rendered_chars", len(doc))
    print("grand_cents", grand_cents)


if __name__ == "__main__":
    main()
