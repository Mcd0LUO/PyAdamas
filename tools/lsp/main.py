# tools/lsp/main.py
# PyAdamas 写的 Language Server —— 入口：stdio 读循环、生命周期、文档同步、诊断调度。
#
# 模块划分（0.467 从单文件纯搬移）：
#   lsp_text.py        文本/位置换算/顶层符号扫描（无 lsp_* 依赖）
#   lsp_protocol.py    JSON-RPC 帧、响应构造、环境/日志
#   lsp_state.py       文档表、增量编辑、workspace 根/文件遍历
#   lsp_diagnostics.py 临时文件 + pyadamas check + publishDiagnostics
#   lsp_intel.py       definition/hover/semanticTokens/signatureHelp/documentHighlight
#   lsp_doc.py         文档模型（Symbol/补全候选）+ documentSymbol/foldingRange/formatting/workspace&symbol
#   lsp_refs.py        references/rename（词法扫描 + 粗作用域）
#
# 构建：tools/lsp/build.sh  →  pya-lsp
# 运行：./pya-lsp [--stdio] [<pyadamas 编译器路径>]
#   - 诊断通过 subprocess 调 "<编译器> check <tmpfile>"；编译器路径优先级：
#     $PYADAMAS > argv[1]（不以 '-' 开头时）> PATH 里的 pyadamas
#   - $PYA_LSP_DEBUG 非空时把调试日志打到 stderr
#   - 协议帧：Content-Length 头 + 空行 + 恰好 N 字节 JSON；回包同样的帧并 flush
import sys
import asyncio
import time
import minijson

from lsp_text import is_digit_str, list_has
from lsp_protocol import env_str, log, open_stdin, params_of, read_frame, rpc_error, rpc_notification, rpc_result, send_frame, uri_of
from lsp_state import apply_change, capture_workspace, doc_index, doc_put, doc_remove
import lsp_state
from lsp_diagnostics import publish_diagnostics
from lsp_intel import TOKEN_TYPES, handle_definition, handle_document_highlight, handle_hover, handle_semantic_tokens, handle_signature_help
from lsp_doc import handle_completion, handle_document_symbol, handle_folding_range, handle_formatting, handle_workspace_symbol
from lsp_refs import handle_references, handle_rename

# ---------------- 各方法处理 ----------------

def init_result() -> str:
    # 能力表：文档同步 + hover + documentSymbol + completion + definition +
    # semanticTokens。semanticTokens 的 legend 顺序必须和 semantic_tokens()
    # 里 TOKEN_* 常量一致（客户端按索引解释）。
    types_json: list[str] = []
    i: int = 0
    while i < len(TOKEN_TYPES):
        types_json.append(minijson.quote(TOKEN_TYPES[i]))
        i = i + 1
    caps: str = ('{"textDocumentSync":{"openClose":true,"change":1},'
                 '"hoverProvider":true,"documentSymbolProvider":true,'
                 '"completionProvider":{"triggerCharacters":["."]},'
                 '"definitionProvider":true,'
                 '"referencesProvider":true,"renameProvider":true,'
                 '"documentFormattingProvider":true,"workspaceSymbolProvider":true,'
                 '"semanticTokensProvider":{"legend":{"tokenTypes":[' +
                 ",".join(types_json) + '],"tokenModifiers":[]},"full":true},'
                 '"signatureHelpProvider":{"triggerCharacters":["(",","]},'
                 '"documentHighlightProvider":true,"foldingRangeProvider":true}')
    return '{"capabilities":' + caps + ',"serverInfo":{"name":"pya-lsp","version":"0.2.0"}}'

def handle_did_open(params: unknown) -> None:
    td: unknown = minijson.get_obj(params, "textDocument")
    uri: str = minijson.get_str(td, "uri")
    text: str = minijson.get_str(td, "text")
    version: int = minijson.get_int(td, "version")
    if uri == "":
        return
    doc_put(uri, text, version)
    publish_diagnostics(uri, text, version)

def apply_did_change(params: unknown) -> str:
    # 把一帧 didChange 的全部 contentChanges 合并进文档表，返回 uri。
    td: unknown = minijson.get_obj(params, "textDocument")
    uri: str = minijson.get_str(td, "uri")
    version: int = minijson.get_int(td, "version")
    if uri == "":
        return ""
    idx: int = doc_index(uri)
    text: str = ""
    if idx >= 0:
        text = lsp_state.doc_texts[idx]
    changes: unknown = minijson.get_arr(params, "contentChanges")
    count: int = minijson.size(changes)
    k: int = 0
    while k < count:
        text = apply_change(text, minijson.at(changes, k))
        k = k + 1
    doc_put(uri, text, version)
    return uri

def handle_did_change(params: unknown, inp: file) -> None:
    uri: str = apply_did_change(params)
    if uri == "":
        return
    # debounce：把紧接着到达的 didChange 合并，只跑最后一次诊断。
    drain_did_changes(inp, uri)

def handle_did_close(params: unknown) -> None:
    uri: str = uri_of(params)
    if uri == "":
        return
    doc_remove(uri)
    clear: str = '{"uri":' + minijson.quote(uri) + ',"diagnostics":[]}'
    send_frame(rpc_notification("textDocument/publishDiagnostics", clear))

# ---------------- didChange 合并（debounce）+ 待处理消息队列 ----------------
#
# LSP 是单线程读循环，没有 timer 线程。这里用「读下一帧之前先等一小段」
# 实现去抖：didChange 到达后，最多等 debounce 毫秒，看 stdin 上是否紧跟
# 下一个帧；紧跟的 didChange 全部合并（只跑最后一次诊断），遇到请求则把
# 它排进队列、立刻发布诊断再继续主循环（不让窗口拖慢请求）。
# stdin 用 open(0, "rb", 0) 无缓冲读，fd 可读性 == 是否有待处理字节；
# 用 asyncio.wait_readable + wait_for 做带超时的 poll（不消费数据）。
pending_bodies: list[str] = []

def debounce_ms() -> int:
    v: str = env_str("PYA_LSP_DIAG_DEBOUNCE_MS")
    if not is_digit_str(v):
        return 300
    return int(v)

async def _wait_readable_for(ms: int) -> bool:
    try:
        await asyncio.wait_for(asyncio.wait_readable(0), float(ms) / 1000.0)
        return True
    except TimeoutError:
        return False

def stdin_ready(ms: int) -> bool:
    if ms < 1:
        return False
    try:
        return asyncio.run(_wait_readable_for(ms))
    except Exception:
        return False

def next_body(inp: file) -> str:
    if len(pending_bodies) > 0:
        b: str = pending_bodies[0]
        pending_bodies.pop(0)
        return b
    return read_frame(inp)

def parse_body(body: str) -> unknown:
    try:
        return minijson.parse(body)
    except ValueError:
        log("malformed JSON, ignored")
        return minijson.new_null()

def drain_did_changes(inp: file, uri: str) -> None:
    dirty: list[str] = [uri]
    window: int = debounce_ms()
    if window <= 0:
        idx0: int = doc_index(uri)
        if idx0 >= 0:
            publish_diagnostics(uri, lsp_state.doc_texts[idx0], lsp_state.doc_versions[idx0])
        return
    deadline: int = int(time.time() * 1000.0) + window
    while True:
        now: int = int(time.time() * 1000.0)
        remain: int = deadline - now
        if remain <= 0:
            break
        if not stdin_ready(remain):
            break
        body: str = read_frame(inp)
        if body == "":
            pending_bodies.append("")
            break
        msg: unknown = parse_body(body)
        method: str = minijson.get_str(msg, "method")
        if method == "textDocument/didChange":
            p2: unknown = params_of(msg)
            apply_did_change(p2)
            u2: str = uri_of(p2)
            if not list_has(dirty, u2):
                dirty.append(u2)
            # 每来一帧重置窗口：连续输入只保留最后一次诊断。
            deadline = int(time.time() * 1000.0) + window
        else:
            pending_bodies.append(body)
            break
    i: int = 0
    while i < len(dirty):
        u3: str = dirty[i]
        idx: int = doc_index(u3)
        if idx >= 0:
            publish_diagnostics(u3, lsp_state.doc_texts[idx], lsp_state.doc_versions[idx])
        i = i + 1

# ---------------- 主循环 ----------------

def run() -> int:
    shutdown_requested: bool = False
    inp: file = open_stdin()
    while True:
        body: str = next_body(inp)
        if body == "":
            log("stdin closed, exiting")
            return 0
        log("recv body bytes=" + str(len(body)))
        msg: unknown = parse_body(body)
        if minijson.is_null(msg):
            continue
        method: str = minijson.get_str(msg, "method")
        if method == "":
            # 客户端对服务端请求的响应：本服务器不发请求，忽略
            continue
        has_id: bool = minijson.has(msg, "id")
        id_text: str = "null"
        if has_id:
            id_text = minijson.stringify(minijson.get(msg, "id"))
        params: unknown = params_of(msg)
        log("method=" + method + " id=" + id_text)
        if method == "initialize":
            capture_workspace(params)
            send_frame(rpc_result(id_text, init_result()))
        elif method == "initialized":
            pass
        elif method == "shutdown":
            shutdown_requested = True
            send_frame(rpc_result(id_text, "null"))
        elif method == "exit":
            log("exit notification")
            if shutdown_requested:
                return 0
            return 1
        elif shutdown_requested:
            if has_id:
                send_frame(rpc_error(id_text, -32600, "server is shutting down"))
        elif method == "textDocument/didOpen":
            handle_did_open(params)
        elif method == "textDocument/didChange":
            handle_did_change(params, inp)
        elif method == "textDocument/didClose":
            handle_did_close(params)
        elif method == "textDocument/documentSymbol":
            handle_document_symbol(id_text, params)
        elif method == "textDocument/hover":
            handle_hover(id_text, params)
        elif method == "textDocument/completion":
            handle_completion(id_text, params)
        elif method == "textDocument/definition":
            handle_definition(id_text, params)
        elif method == "textDocument/semanticTokens/full":
            handle_semantic_tokens(id_text, params)
        elif method == "textDocument/references":
            handle_references(id_text, params)
        elif method == "textDocument/rename":
            handle_rename(id_text, params)
        elif method == "textDocument/signatureHelp":
            handle_signature_help(id_text, params)
        elif method == "textDocument/documentHighlight":
            handle_document_highlight(id_text, params)
        elif method == "textDocument/foldingRange":
            handle_folding_range(id_text, params)
        elif method == "textDocument/formatting":
            handle_formatting(id_text, params)
        elif method == "workspace/symbol":
            handle_workspace_symbol(id_text, params)
        else:
            if has_id:
                send_frame(rpc_error(id_text, -32601, "method not found: " + method))

def main() -> None:
    sys.exit(run())

main()
