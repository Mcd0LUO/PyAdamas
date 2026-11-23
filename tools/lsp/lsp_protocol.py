# tools/lsp/lsp_protocol.py
# stdio 传输 + JSON-RPC 帧 + 响应构造 + 环境/日志。
# 帧：Content-Length 头 + 空行 + 恰好 N 字节；回包同样帧并 flush。
import sys
import os
import minijson

from lsp_text import is_digit_str

# ---------------- 环境 / 日志 ----------------

def env_str(name: str) -> str:
    v: str = os.getenv(name)
    if v == "":
        return ""
    return v

def flag_on(name: str) -> bool:
    return env_str(name) != ""

def log(msg: str) -> None:
    if flag_on("PYA_LSP_DEBUG"):
        sys.stderr.write("[pya-lsp] " + msg + "\n")
        sys.stderr.flush()

def compiler_path() -> str:
    v: str = env_str("PYADAMAS")
    if v != "":
        return v
    if len(sys.argv) > 1:
        c: str = sys.argv[1]
        if not c.startswith("-"):
            return c
    return "pyadamas"

# ---------------- JSON-RPC 帧 ----------------

def open_stdin() -> file:
    # 二进制帧读取：Content-Length 是字节数，帧体是 UTF-8，必须按字节读。
    # 0.447 修好了流式语义（非普通文件上一次 read(2)，不凑满 8192）：
    #   * 文本模式 sys.stdin 现在也能「有多少拿多少」，read(n) 按 UTF-8 字节
    #     计（实测 "hélloXX" read(6) 得到 6 字节）；
    #   * 但 LSP 帧是二进制语义，保留 open(0, "rb") 更稳（不依赖 stdin 的
    #     文本缓冲/换行翻译），也避免某些客户端发 CRLF 边界时被翻译。
    # buffering=0（无缓冲）：fd 层可读性 == 是否有待处理数据，为后续
    # didChange 合并（poll 一秒内是否有下一帧）留出可靠依据；每帧只有
    # 几百字节，逐字节 read(2) 的开销可以忽略。
    # 保底：/dev/stdin（部分平台 fd 0 不可 open）→ sys.stdin（最后手段）。
    try:
        return open(0, "rb", 0)
    except OSError:
        log("open(0, 'rb') failed, trying /dev/stdin")
        try:
            return open("/dev/stdin", "rb")
        except OSError:
            log("cannot open binary stdin, falling back to sys.stdin")
            return sys.stdin

def read_frame(inp: file) -> str:
    length: int = -1
    while True:
        line: str = inp.readline()
        if line == "":
            return ""
        stripped: str = line.strip()
        if stripped == "":
            break
        if stripped.startswith("Content-Length:"):
            num: str = stripped[len("Content-Length:"):].strip()
            if is_digit_str(num):
                length = int(num)
                log("content-length=" + num)
    if length <= 0:
        return ""
    body: str = inp.read(length)
    if body == "":
        return ""
    return body

def send_frame(body: str) -> None:
    # 所有响应体都是纯 ASCII（minijson.quote 把非 ASCII 转成 \uXXXX），
    # 用 byte_len 而不是字符数，保证多字节场景也符合 Content-Length 语义。
    frame: str = "Content-Length: " + str(minijson.byte_len(body)) + "\r\n\r\n" + body
    sys.stdout.write(frame)
    sys.stdout.flush()

def rpc_result(id_text: str, result_text: str) -> str:
    return '{"jsonrpc":"2.0","id":' + id_text + ',"result":' + result_text + '}'

def rpc_error(id_text: str, code: int, message: str) -> str:
    return '{"jsonrpc":"2.0","id":' + id_text + ',"error":{"code":' + str(code) + ',"message":' + minijson.quote(message) + '}}'

def rpc_notification(method: str, params_text: str) -> str:
    return '{"jsonrpc":"2.0","method":' + minijson.quote(method) + ',"params":' + params_text + '}'

def pos_json(line: int, character: int) -> str:
    return '{"line":' + str(line) + ',"character":' + str(character) + '}'

def range_json(sl: int, sc: int, el: int, ec: int) -> str:
    return '{"start":' + pos_json(sl, sc) + ',"end":' + pos_json(el, ec) + '}'

def location_json(uri: str, line: int, character: int, length: int) -> str:
    return ('{"uri":' + minijson.quote(uri) + ',"range":' +
            range_json(line, character, line, character + length) + '}')

def params_of(msg: unknown) -> unknown:
    return minijson.get_obj(msg, "params")

def uri_of(params: unknown) -> str:
    td: unknown = minijson.get_obj(params, "textDocument")
    return minijson.get_str(td, "uri")
