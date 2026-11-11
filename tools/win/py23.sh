#!/usr/bin/env bash
# py23.sh - bash entry point for the py23 compiler.
#
# Works both inside WSL (talks to the local docker daemon) and in Git Bash
# on Windows (hands the work over to WSL through wsl.exe).
#
# Runs py23 in a throwaway container that mounts:
#   * the WSL repo        -> /w     (compiler binary + module cache)
#   * the script's folder -> /work  (the program's own working directory)
# so relative paths, data files and build output all behave normally.
#
#   py23.sh run x.py
#   py23.sh check x.py
#   py23.sh build x.py -o out
#
# Env overrides: PY23_WSL_DISTRO (Debian), PY23_IMAGE (py23-env:gcc14),
#                PY23_REPO (/home/lenovo/cpp/cpy23)
#
# NOTE: keep this file pure ASCII, and keep it LF-terminated (a shebang
# needs LF, CRLF breaks the interpreter path).

set -euo pipefail

IMAGE="${PY23_IMAGE:-py23-env:gcc14}"
REPO="${PY23_REPO:-/home/lenovo/cpp/cpy23}"
DISTRO="${PY23_WSL_DISTRO:-Debian}"
REPO_CTR=/w
WORK_CTR=/work

if [ $# -eq 0 ]; then
  echo "usage: py23 <check|run|build|tokens|ast|types|ir|asm> <file.py> [args...]" >&2
  exit 2
fi

# locate the script argument
script=""
for a in "$@"; do
  case "$a" in
    *.py|*.py23i)
      if [ -f "$a" ]; then script="$a"; break; fi
      ;;
  esac
done
if [ -z "$script" ]; then
  echo "[py23] no .py file argument found" >&2
  exit 2
fi

script_dir=$(cd "$(dirname "$script")" && pwd)
script_base=$(basename "$script")

# rewrite the script argument to its in-container path
args=()
for a in "$@"; do
  if [ "$a" = "$script" ]; then
    args+=("$WORK_CTR/$script_base")
  else
    args+=("$a")
  fi
done

if [ -n "${WSL_DISTRO_NAME:-}" ]; then
  # ---- inside WSL: the docker daemon is right here ----
  exec docker run --rm --user 1000:1000 -e HOME=/tmp \
    -v "$REPO:$REPO_CTR" -v "$script_dir:$WORK_CTR" -w "$WORK_CTR" \
    "$IMAGE" "$REPO_CTR/build/dev/py23" "${args[@]}"
fi

# ---- Git Bash on Windows: hand over to WSL ----
# MSYS would rewrite POSIX-looking arguments; WSL_UTF8 makes wsl.exe emit UTF-8.
export MSYS_NO_PATHCONV=1
export WSL_UTF8=1

# wsl.exe eats backslashes in its arguments, so hand it a forward-slash path
wsl_dir=$(wsl.exe -d "$DISTRO" -- wslpath -a "$(cygpath -m "$script_dir")" | tr -d '\r\n')

quoted=""
for a in "${args[@]}"; do
  quoted="$quoted '$(printf "%s" "$a" | sed "s/'/'\\\\''/g")'"
done

cmd="docker run --rm --user 1000:1000 -e HOME=/tmp"
cmd="$cmd -v '$REPO:$REPO_CTR' -v '$wsl_dir:$WORK_CTR' -w $WORK_CTR"
cmd="$cmd $IMAGE $REPO_CTR/build/dev/py23$quoted"

exec wsl.exe -d "$DISTRO" -- bash -lc "$cmd"
