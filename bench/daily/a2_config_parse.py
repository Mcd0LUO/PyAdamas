"""a2_config_parse -- 快速往返型：INI 风格配置解析。

从内嵌配置文本解析 section/key/value，跳过注释与空行，
按声明的类型表做 int/float/bool/str 强制转换与校验，
合并默认值后按 section+key 排序输出规范化结果。
"""


def strip_comment(line: str) -> str:
    out: list[str] = []
    in_quote: bool = False
    for ch in line:
        if ch == '"':
            in_quote = not in_quote
        if ch == "#" and not in_quote:
            break
        out.append(ch)
    return "".join(out).strip()


def coerce(kind: str, raw: str) -> str:
    if kind == "int":
        return str(int(raw))
    if kind == "float":
        return format(float(raw), ".6g")
    if kind == "bool":
        low: str = raw.lower()
        if low == "true" or low == "yes" or low == "1" or low == "on":
            return "true"
        if low == "false" or low == "no" or low == "0" or low == "off":
            return "false"
        raise ValueError("bad bool: " + raw)
    return raw


def parse_config(text: str, types: dict[str, str]) -> dict[str, dict[str, str]]:
    sections: dict[str, dict[str, str]] = {}
    current: str = "default"
    sections[current] = {}
    for raw_line in text.splitlines():
        line: str = strip_comment(raw_line)
        if len(line) == 0:
            continue
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1].strip()
            if current not in sections:
                sections[current] = {}
            continue
        if "=" not in line:
            continue
        parts: list[str] = line.split("=", 1)
        key: str = parts[0].strip()
        val: str = parts[1].strip()
        if len(val) >= 2 and val.startswith('"') and val.endswith('"'):
            val = val[1:-1]
        full: str = current + "." + key
        kind: str = types.get(full, "str")
        sections[current][key] = coerce(kind, val)
    return sections


def main() -> None:
    text: str = (
        "# server config\n"
        "[server]\n"
        "host = example.internal   # hostname\n"
        "port = 8080\n"
        "tls = yes\n"
        "timeout = 2.5\n"
        "name = \"edge node\"\n"
        "\n"
        "[cache]\n"
        "enabled = on\n"
        "size_mb = 256\n"
        "ttl = 300\n"
        "ratio = 0.75\n"
        "\n"
        "[logging]\n"
        "level = INFO\n"
        "verbose = no\n"
    )
    types: dict[str, str] = {
        "server.port": "int",
        "server.tls": "bool",
        "server.timeout": "float",
        "cache.enabled": "bool",
        "cache.size_mb": "int",
        "cache.ttl": "int",
        "cache.ratio": "float",
        "logging.verbose": "bool",
    }
    defaults: dict[str, dict[str, str]] = {
        "server": {"host": "localhost", "port": "80", "tls": "false", "timeout": "5.0", "name": "node"},
        "cache": {"enabled": "false", "size_mb": "64", "ttl": "60", "ratio": "0.5"},
        "logging": {"level": "WARNING", "verbose": "false"},
    }
    parsed: dict[str, dict[str, str]] = parse_config(text, types)
    names: list[str] = sorted(defaults.keys())
    total: int = 0
    for sec in names:
        merged: dict[str, str] = defaults[sec].copy()
        if sec in parsed:
            for k in sorted(parsed[sec].keys()):
                merged[k] = parsed[sec][k]
        keys: list[str] = sorted(merged.keys())
        for k in keys:
            print(sec + "." + k + " = " + merged[k])
            total += 1
    print("total_keys", total)
    bad: bool = False
    try:
        coerce("bool", "maybe")
    except ValueError:
        bad = True
    print("validation_rejects_bad_bool", bad)


if __name__ == "__main__":
    main()
