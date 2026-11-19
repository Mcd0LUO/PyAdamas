"""d5_finance -- 数值算子型：金融数学。

等额本息贷款摊销表（360 期）、总利息、NPV 与 IRR（二分法）、
复利终值、年金现值。幂运算用纯浮点快速幂 ipow（无 libm 调用，
两边同序 IEEE 逐位一致；规避 math.pow 结果在寄存器压力下被破坏的 bug）。
"""
import math


def ipow(base: float, n: int) -> float:
    # repeated squaring, non-negative int exponent; pure float arithmetic
    result: float = 1.0
    b: float = base
    e: int = n
    while e > 0:
        if e % 2 == 1:
            result = result * b
        b = b * b
        e = e // 2
    return result


def monthly_payment(principal: float, annual_rate: float, months: int) -> float:
    r: float = annual_rate / 12.0
    if r == 0.0:
        return principal / float(months)
    factor: float = ipow(1.0 + r, months)
    num: float = principal * r * factor
    den: float = factor - 1.0
    return num / den


def amortize(principal: float, annual_rate: float, months: int) -> list[float]:
    r: float = annual_rate / 12.0
    pay: float = monthly_payment(principal, annual_rate, months)
    bal: float = principal
    interest_total: float = 0.0
    rows: list[float] = []
    for m in range(months):
        interest: float = bal * r
        principal_part: float = pay - interest
        bal = bal - principal_part
        interest_total += interest
        if m < 3 or m >= months - 3:
            rows.append(float(m + 1))
            rows.append(interest)
            rows.append(principal_part)
            rows.append(bal)
    rows.append(interest_total)
    rows.append(bal)
    return rows


def npv(rate: float, flows: list[float]) -> float:
    s: float = 0.0
    base: float = 1.0 + rate
    p: float = 1.0
    for t in range(len(flows)):
        s += flows[t] / p
        p = p * base
    return s


def irr(flows: list[float]) -> float:
    lo: float = -0.99
    hi: float = 1.0
    for it in range(200):
        mid: float = (lo + hi) / 2.0
        v: float = npv(mid, flows)
        if v > 0.0:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2.0


def compound(principal: float, rate: float, years: int, per_year: int) -> float:
    n: int = years * per_year
    f: float = ipow(1.0 + rate / float(per_year), n)
    return principal * f


def annuity_pv(payment: float, rate: float, periods: int) -> float:
    if rate == 0.0:
        return payment * float(periods)
    f: float = ipow(1.0 + rate, periods)
    return payment * (1.0 - 1.0 / f) / rate


def cents(x: float) -> int:
    return int(math.floor(x * 100.0 + 0.5))


def main() -> None:
    principal: float = 300000.0
    rate: float = 0.045
    months: int = 360
    pay: float = monthly_payment(principal, rate, months)
    print("loan_principal", format(principal, ".2f"), "annual_rate", format(rate, ".4f"), "months", months)
    print("monthly_payment", format(pay, ".6f"), "cents", cents(pay))
    rows: list[float] = amortize(principal, rate, months)
    nshown: int = (len(rows) - 2) // 4
    print("-- schedule (first/last 3) --")
    for i in range(nshown):
        m: int = int(rows[i * 4])
        interest: float = rows[i * 4 + 1]
        pp: float = rows[i * 4 + 2]
        bal: float = rows[i * 4 + 3]
        print(m, format(interest, ".4f"), format(pp, ".4f"), format(bal, ".4f"))
    total_interest: float = rows[len(rows) - 2]
    final_bal: float = rows[len(rows) - 1]
    print("total_interest", format(total_interest, ".4f"), "cents", cents(total_interest))
    print("final_balance", format(final_bal, ".8f"))
    print("-- NPV / IRR --")
    flows: list[float] = [-10000.0, 2000.0, 2500.0, 3000.0, 3500.0, 4000.0]
    for r in [0.0, 0.05, 0.1, 0.15]:
        print("npv", format(r, ".2f"), format(npv(r, flows), ".6f"))
    r_irr: float = irr(flows)
    print("irr", format(r_irr, ".8f"), "npv_at_irr", format(npv(r_irr, flows), ".3e"))
    print("-- compound / annuity --")
    for py in [1, 4, 12, 365]:
        fv: float = compound(10000.0, 0.06, 10, py)
        print("compound", py, format(fv, ".6f"))
    print("annuity_pv", format(annuity_pv(1000.0, 0.05, 20), ".6f"))


if __name__ == "__main__":
    main()
