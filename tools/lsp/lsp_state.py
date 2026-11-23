# tools/lsp/lsp_state.py
# 服务器状态：内存文档表（URI -> 文本/版本）、增量编辑、workspace 根与文件遍历。
#
# 跨模块读文档文本用 lsp_state.doc_texts 模块属性，不要 from ... import：
# pyadamas 的 from-import 是快照绑定，模块级变量重新赋值后快照不会更新。
import os
from os.path import isdir

from lsp_text import offset_at
from lsp_protocol import j_arr, j_at, j_has, j_int, j_obj, j_size, j_str

# ---------------- 文档表（URI -> 文本/版本） ----------------

doc_uris: list[str] = []
doc_texts: list[str] = []
doc_versions: list[int] = []

def doc_index(uri: str) -> int:
    i: int = 0
    while i < len(doc_uris):
        if doc_uris[i] == uri:
            return i
        i = i + 1
    return -1

def doc_put(uri: str, text: str, version: int) -> None:
    i: int = doc_index(uri)
    if i < 0:
        doc_uris.append(uri)
        doc_texts.append(text)
        doc_versions.append(version)
        return
    doc_texts[i] = text
    doc_versions[i] = version

def doc_remove(uri: str) -> None:
    i: int = doc_index(uri)
    if i < 0:
        return
    doc_uris.pop(i)
    doc_texts.pop(i)
    doc_versions.pop(i)

def apply_change(text: str, change: unknown) -> str:
    new_text: str = j_str(change, "text")
    if not j_has(change, "range"):
        return new_text
    rng: unknown = j_obj(change, "range")
    st: unknown = j_obj(rng, "start")
    en: unknown = j_obj(rng, "end")
    a: int = offset_at(text, j_int(st, "line"), j_int(st, "character"))
    b: int = offset_at(text, j_int(en, "line"), j_int(en, "character"))
    if b < a:
        b = a
    return text[:a] + new_text + text[b:]

# ---------------- workspace：根目录 / 文件遍历 / URI ----------------

workspace_root: str = ""
workspace_uri: str = ""

def hex_val(c: str) -> int:
    o: int = ord(c)
    if o >= 48 and o <= 57:
        return o - 48
    if o >= 97 and o <= 102:
        return o - 87
    if o >= 65 and o <= 70:
        return o - 55
    return -1

def uri_decode(s: str) -> str:
    out: str = ""
    i: int = 0
    while i < len(s):
        if s[i] == "%" and i + 2 < len(s):
            hi: int = hex_val(s[i + 1])
            lo: int = hex_val(s[i + 2])
            if hi >= 0 and lo >= 0:
                out = out + chr(hi * 16 + lo)
                i = i + 3
                continue
        out = out + s[i]
        i = i + 1
    return out

def file_uri_to_path(uri: str) -> str:
    if not uri.startswith("file://"):
        return ""
    rest: str = uri[len("file://"):]
    if not rest.startswith("/"):
        slash: int = rest.find("/")
        if slash < 0:
            return ""
        rest = rest[slash:]
    path: str = uri_decode(rest)
    # Windows：file:///C:/x -> C:/x
    if len(path) >= 3 and path[0] == "/" and path[2] == ":":
        path = path[1:]
    return path

def uri_escape_path(p: str) -> str:
    out: str = ""
    i: int = 0
    while i < len(p):
        c: str = p[i]
        if c == " ":
            out = out + "%20"
        elif c == "%":
            out = out + "%25"
        elif c == "#":
            out = out + "%23"
        elif c == "?":
            out = out + "%3F"
        else:
            out = out + c
        i = i + 1
    return out

def path_to_file_uri(p: str) -> str:
    if p.startswith("/"):
        return "file://" + uri_escape_path(p)
    return "file:///" + uri_escape_path(p)

def capture_workspace(params: unknown) -> None:
    global workspace_root
    global workspace_uri
    ru: str = j_str(params, "rootUri")
    if ru == "":
        folders: unknown = j_arr(params, "workspaceFolders")
        if j_size(folders) > 0:
            ru = j_str(j_at(folders, 0), "uri")
    if ru == "":
        rp: str = j_str(params, "rootPath")
        if rp != "":
            workspace_root = rp
            workspace_uri = path_to_file_uri(rp)
        return
    workspace_uri = ru
    workspace_root = file_uri_to_path(ru)

def collect_py_files(root: str, depth: int, out: list[str]) -> None:
    if depth > 12 or len(out) >= 2000:
        return
    try:
        names: list[str] = os.listdir(root)
    except OSError:
        return
    i: int = 0
    while i < len(names) and len(out) < 2000:
        nm: str = names[i]
        full: str = root + "/" + nm
        if nm.startswith("."):
            i = i + 1
            continue
        is_dir: bool = False
        try:
            is_dir = isdir(full)
        except OSError:
            is_dir = False
        if is_dir:
            if nm != "__pycache__" and nm != "build" and nm != "node_modules":
                collect_py_files(full, depth + 1, out)
        elif nm.endswith(".py") or nm.endswith(".pyi"):
            out.append(full)
        i = i + 1

def read_text_file(path: str) -> str:
    try:
        f = open(path, "r")
        text: str = f.read()
        f.close()
        return text
    except OSError:
        return ""
