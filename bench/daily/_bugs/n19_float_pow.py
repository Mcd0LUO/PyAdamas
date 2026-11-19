# NEW BUG N19b (silent wrong float): a float local initialized from math.pow()
# and reused in arithmetic miscompiles under whole-program register pressure.
# monthly_payment(300000, 0.045, 360) -> -269472.137276 (should 1520.055929).
# Isolated small cases are correct (Heisenbug, same family as N19a). Named-temp
# refactor did NOT fix it in this context. Suspect: math.pow return (xmm0) /
# float slot promotion clobbered across subsequent ops under register pressure.
# Workaround: call-free integer-exponent power (ipow, repeated squaring) -- both
# engines run the same pure-float code so outputs still match; verified correct.
import math


def monthly_payment(principal: float, annual_rate: float, months: int) -> float:
    r: float = annual_rate / 12.0
    if r == 0.0:
        return principal / float(months)
    factor: float = math.pow(1.0 + r, float(months))
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
    for t in range(len(flows)):
        p: float = math.pow(1.0 + rate, float(t))
        s += flows[t] / p
    return s


def compound(principal: float, rate: float, years: int, per_year: int) -> float:
    n: int = years * per_year
    f: float = math.pow(1.0 + rate / float(per_year), float(n))
    return principal * f


def main() -> None:
    pay: float = monthly_payment(300000.0, 0.045, 360)
    print("monthly_payment", format(pay, ".6f"))   # CPython 1520.055929, pa -269472.137276
    rows: list[float] = amortize(300000.0, 0.045, 360)
    print("total_interest", format(rows[len(rows) - 2], ".4f"))
    flows: list[float] = [-10000.0, 2000.0, 2500.0, 3000.0, 3500.0, 4000.0]
    print("npv0", format(npv(0.0, flows), ".6f"))
    print("compound", format(compound(10000.0, 0.06, 10, 12), ".6f"))


if __name__ == "__main__":
    main()
