#!/usr/bin/env bash
# 用 PyAdamas 自己构建 LSP 服务器（dogfooding）。
#   tools/lsp/build.sh [输出路径]     # 默认 tools/lsp/pya-lsp
#
# 编译器查找：$PYADAMAS 优先（显式指定，失败即报错）；否则按 mtime 从新到旧遍历
# build/*/pyadamas，并对每个候选做「构建 + 空 stdin 自检」（老版本编译器构建出的
# 程序可能在退出时崩，例如 refcount increment on a freed object），第一个自检通过的胜出。
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${1:-$here/pya-lsp}"

self_check() {
  local bin="$1" err rc
  err="$(timeout 10 "$bin" </dev/null 2>&1 >/dev/null)" && rc=0 || rc=$?
  if [[ $rc -ne 0 ]]; then
    echo "  self-check failed (rc=$rc): $err" >&2
    return 1
  fi
  if [[ -n "$err" ]]; then
    echo "  self-check stderr: $err" >&2
    return 1
  fi
  return 0
}

build_with() {
  local cc="$1"
  echo "trying compiler: $cc"
  if ! "$cc" build "$here/main.py" -o "$out"; then
    echo "  build failed" >&2
    return 1
  fi
  if [[ ! -x "$out" ]]; then
    echo "  no output binary" >&2
    return 1
  fi
  self_check "$out"
}

candidates=()
if [[ -n "${PYADAMAS:-}" ]]; then
  candidates+=("$PYADAMAS")
else
  while IFS= read -r f; do
    candidates+=("$f")
  done < <(ls -t "$root"/build/*/pyadamas 2>/dev/null || true)
  if command -v pyadamas >/dev/null 2>&1; then
    candidates+=("$(command -v pyadamas)")
  fi
fi

if [[ ${#candidates[@]} -eq 0 ]]; then
  echo "error: pyadamas compiler not found; set PYADAMAS=/path/to/pyadamas" >&2
  exit 2
fi

for cc in "${candidates[@]}"; do
  [[ -x "$cc" ]] || continue
  if build_with "$cc"; then
    echo "ok: $out (compiler: $cc)"
    exit 0
  fi
  echo "  -> skip $cc" >&2
done

echo "error: no working pyadamas compiler found (tried: ${candidates[*]})" >&2
exit 2
