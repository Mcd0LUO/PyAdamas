#!/usr/bin/env bash
# LSP 冒烟测试入口：构建服务器（build.sh 会挑一个能用的 pyadamas）然后跑 smoke.py。
#   tests/lsp/smoke.sh
# 环境变量：
#   PYADAMAS          指定 pyadamas 编译器（构建 + 诊断都用它）
#   PYA_LSP           指定现成的 pya-lsp 二进制（默认 tools/lsp/pya-lsp）
#   PYA_LSP_SKIP_BUILD=1  跳过构建，直接用现有二进制
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
server="${PYA_LSP:-$root/tools/lsp/pya-lsp}"

if [[ "${PYA_LSP_SKIP_BUILD:-0}" != "1" ]]; then
  build_log="$("$root/tools/lsp/build.sh" "$server")"
  printf "%s\n" "$build_log"
  if [[ -z "${PYADAMAS:-}" ]]; then
    chosen="$(printf "%s\n" "$build_log" | sed -n "s/^ok: .*(compiler: \(.*\))$/\1/p" | tail -n 1)"
    if [[ -n "$chosen" ]]; then export PYADAMAS="$chosen"; fi
  fi
fi

exec python3 "$here/smoke.py"
