"""a1_cli_calc -- 快速往返型：命令行计算器。

解析 argv 里的算式（运算符 操作数...），支持整数/浮点混合、
错误处理与格式化输出。无参数时跑一组内置演示算式。
脚本短、运行快：编译+启动开销占比大，如实对比 compile+run。
"""
import sys


def parse_num(tok: str) -> float:
    """把字符串解析成数值；整数也按 float 处理以便统一运算。"""
    if "." in tok or "e" in tok or "E" in tok:
        return float(tok)
    return float(int(tok))


def is_int_tok(tok: str) -> bool:
    if tok.startswith("-") or tok.startswith("+"):
        tok = tok[1:]
    return tok.isdigit()


def calc(op: str, a: float, b: float) -> float:
    if op == "add":
        return a + b
    if op == "sub":
        return a - b
    if op == "mul":
        return a * b
    if op == "div":
        if b == 0.0:
            raise ValueError("division by zero")
        return a / b
    if op == "mod":
        if b == 0.0:
            raise ValueError("modulo by zero")
        return a % b
    if op == "pow":
        return a ** b
    raise ValueError("unknown operator: " + op)


def fmt_result(op: str, a: str, b: str, v: float) -> str:
    if is_int_tok(a) and is_int_tok(b) and op != "div" and v == float(int(v)):
        return str(int(v))
    return format(v, ".6g")


def run_expr(tokens: list[str]) -> None:
    if len(tokens) != 3:
        print("ERR need 3 tokens: op a b")
        return
    op: str = tokens[0]
    try:
        a: float = parse_num(tokens[1])
        b: float = parse_num(tokens[2])
        v: float = calc(op, a, b)
    except ValueError:
        print("ERR bad expression:", " ".join(tokens))
        return
    out: str = fmt_result(op, tokens[1], tokens[2], v)
    print(tokens[1], op, tokens[2], "=", out)


def main() -> None:
    args: list[str] = sys.argv[1:]
    if len(args) == 0:
        demos: list[list[str]] = [
            ["add", "17", "25"],
            ["sub", "100", "58"],
            ["mul", "12", "12"],
            ["div", "355", "113"],
            ["mod", "17", "5"],
            ["pow", "2", "10"],
            ["div", "1", "0"],
            ["add", "x", "2"],
        ]
        for d in demos:
            run_expr(d)
    else:
        i: int = 0
        while i + 2 < len(args) + 1 and i + 3 <= len(args):
            run_expr(args[i:i + 3])
            i += 3
        if i < len(args):
            run_expr(args[i:])


if __name__ == "__main__":
    main()
