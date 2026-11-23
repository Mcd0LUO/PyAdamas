# tools/lsp/lsp_diagnostics.py
# 诊断：写临时文件 -> subprocess 调 "<pyadamas> check" -> 解析 stderr -> publishDiagnostics。
import os
import time
import subprocess
import minijson

from lsp_text import is_digit_str
from lsp_protocol import compiler_path, env_str, log, range_json, rpc_notification, send_frame

tmp_seq: int = 0

def temp_file_path() -> str:
    global tmp_seq
    tmp_seq = tmp_seq + 1
    base: str = env_str("TMPDIR")
    if base == "":
        base = "/tmp"
    if base.endswith("/"):
        base = base[:len(base) - 1]
    stamp: int = int(time.time() * 1000000.0)
    return base + "/pya-lsp-" + str(stamp) + "-" + str(tmp_seq) + ".py"

def write_file(path: str, text: str) -> bool:
    try:
        f = open(path, "w")
        f.write(text)
        f.close()
        return True
    except OSError:
        return False

def remove_file(path: str) -> None:
    try:
        os.remove(path)
    except OSError:
        pass

# ---------------- 诊断（subprocess 调 pyadamas check） ----------------

def parse_compiler_diagnostics(stderr_text: str, tmp_path: str) -> list[str]:
    out: list[str] = []
    prefix: str = tmp_path + ":"
    lines: list[str] = stderr_text.split("\n")
    i: int = 0
    while i < len(lines):
        raw: str = lines[i]
        i = i + 1
        stripped: str = raw.strip()
        if not stripped.startswith(prefix):
            continue
        rest: str = stripped[len(prefix):]
        p1: int = rest.find(":")
        if p1 < 0:
            continue
        lnum_s: str = rest[:p1]
        rest = rest[p1 + 1:]
        p2: int = rest.find(":")
        if p2 < 0:
            continue
        cnum_s: str = rest[:p2]
        rest = rest[p2 + 1:].strip()
        if not is_digit_str(lnum_s) or not is_digit_str(cnum_s):
            continue
        severity: int = 1
        msg: str = rest
        if rest.startswith("error:"):
            msg = rest[len("error:"):].strip()
        elif rest.startswith("warning:"):
            severity = 2
            msg = rest[len("warning:"):].strip()
        elif rest.startswith("note:"):
            severity = 3
            msg = rest[len("note:"):].strip()
        else:
            continue
        line0: int = int(lnum_s) - 1
        col0: int = int(cnum_s) - 1
        if line0 < 0:
            line0 = 0
        if col0 < 0:
            col0 = 0
        out.append('{"range":' + range_json(line0, col0, line0, col0 + 1) + ',"severity":' + str(severity) + ',"source":"pyadamas","message":' + minijson.quote(msg) + '}')
    return out

def compute_diagnostics(text: str) -> list[str]:
    tmp: str = temp_file_path()
    if not write_file(tmp, text):
        log("cannot write temp file " + tmp)
        return []
    stderr_text: str = ""
    try:
        r = subprocess.run([compiler_path(), "check", tmp], capture_output=True, text=True, timeout=30.0)
        stderr_text = r.stderr
        log("check rc=" + str(r.returncode) + " stderr-bytes=" + str(len(stderr_text)))
    except TimeoutExpired:
        log("pyadamas check timed out")
        stderr_text = ""
    remove_file(tmp)
    return parse_compiler_diagnostics(stderr_text, tmp)

def publish_diagnostics(uri: str, text: str, version: int) -> None:
    diags: list[str] = compute_diagnostics(text)
    params: str = '{"uri":' + minijson.quote(uri) + ',"version":' + str(version) + ',"diagnostics":[' + ",".join(diags) + ']}'
    send_frame(rpc_notification("textDocument/publishDiagnostics", params))
    log("published " + str(len(diags)) + " diagnostic(s) for " + uri)
