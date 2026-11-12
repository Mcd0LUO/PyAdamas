#!/usr/bin/env bash
# pyadamas.sh - bash entry point for the pyadamas compiler.
#
# Works both inside WSL (talks to the local docker daemon) and in Git Bash
# on Windows (hands the work over to WSL through wsl.exe).
#
# Runs pyadamas in a throwaway container that mounts:
#   * the WSL repo        -> /w     (compiler binary + module cache)
#   * the script's folder -> /work  (the program's own working directory)
# so relative paths, data files and build output all behave normally.
#
#   pyadamas.sh run x.py
#   pyadamas.sh check x.py
#   pyadamas.sh build x.py -o out
#
# Env overrides: PYADAMAS_WSL_DISTRO (Debian), PYADAMAS_IMAGE (pyadamas-env:gcc14),
#                PYADAMAS_REPO (/home/lenovo/cpp/cpyadamas)
#
# NOTE: keep this file pure ASCII, and keep it LF-terminated (a shebang
# needs LF, CRLF breaks the interpreter path).

set -euo pipefail

IMAGE="${PYADAMAS_IMAGE:-pyadamas-env:gcc14}"
REPO="${PYADAMAS_REPO:-/home/lenovo/cpp/cpyadamas}"
DISTRO="${PYADAMAS_WSL_DISTRO:-Debian}"
REPO_CTR=/w
WORK_CTR=/work

if [ $# -eq 0 ]; then
  echo "usage: pyadamas <check|run|build|tokens|ast|types|ir|asm> <file.py> [args...]" >&2
  exit 2
fi

# locate the script argument
script=""
for a in "$@"; do
  case "$a" in
    *.py|*.pya)
      if [ -f "$a" ]; then script="$a"; break; fi
      ;;
  esac
done
if [ -z "$script" ]; then
  echo "[pyadamas] no .py file argument found" >&2
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
    "$IMAGE" "$REPO_CTR/build/dev/pyadamas" "${args[@]}"
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
cmd="$cmd $IMAGE $REPO_CTR/build/dev/pyadamas$quoted"

exec wsl.exe -d "$DISTRO" -- bash -lc "$cmd"
