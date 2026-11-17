#!/usr/bin/env bash
# sync-server.sh —— 把当前 checkout 同步到开发服务器（旧机与本地 WSL 都慢，
# 日常开发放到 110.42.51.202:/src/luo-porj）。
#
# **为什么需要它**：服务器连不上 GitHub，同步只能走 rsync；而 rsync 必须
# 排除构建产物 —— 本地 build/ 有 3GB，传上去既慢，又会让服务器直接用
# **本地工具链**编出来的中间产物（本地 gcc 14 / 服务器 gcc 15），
# 那是错的对象文件，链接出来的东西不可信。
#
# 用法：
#   tools/sync-server.sh                 # 推送当前 checkout 到服务器（默认）
#   tools/sync-server.sh --dry-run       # 只列出会传哪些文件，不实际传
#   tools/sync-server.sh --pull          # 反向：把服务器上的改动拉回本地
#   tools/sync-server.sh --build         # 推送后顺带在服务器上增量构建 + difftest
#
# 目标可覆盖：
#   PYADAMAS_SYNC_TARGET=user@host:/path  PYADAMAS_SYNC_KEY=~/.ssh/key
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

TARGET=${PYADAMAS_SYNC_TARGET:-root@110.42.51.202:/src/luo-porj/}
KEY=${PYADAMAS_SYNC_KEY:-$HOME/.ssh/id_ed25519_root_mc}
SSH="ssh -i $KEY -o BatchMode=yes -o StrictHostKeyChecking=accept-new"

MODE=${1:-push}
DRY=""
case "$MODE" in
    --dry-run) DRY="--dry-run"; MODE=push ;;
    --pull) MODE=pull ;;
    --build) MODE=build ;;
    push|pull|build) ;;
    *) echo "用法: $0 [--dry-run|--pull|--build]" >&2; exit 2 ;;
esac

# **排除清单**：构建产物、缓存、测试临时文件一律不传。
# 服务器要用自己的 gcc 15 重新编，绝不能拿本地 gcc 14 的 .o。
EXCLUDES=(
    --exclude 'build/'            # 本地构建目录（3GB）
    --exclude 'build*/'           # build/asan、build/nocet 等
    --exclude 'modcache/'         # 编译产物缓存
    --exclude '*.o'
    --exclude '*.a'
    --exclude '*.so'
    --exclude 'prog.tmp'          # run_tests.sh 的临时产物
    --exclude 'out.txt'
    --exclude 'exp.txt'
    --exclude '*.log'
    --exclude '__pycache__/'
    --exclude '.cache/'
    --exclude 'a.out'
    # 编辑器/系统噪音
    --exclude '.vscode/'
    --exclude '.idea/'
    --exclude '*~'
)

sync_push() {
    echo "==> 推送 $ROOT/  ->  $TARGET"
    rsync -az --delete $DRY "${EXCLUDES[@]}" -e "$SSH" "$ROOT/" "$TARGET"
}

sync_pull() {
    echo "==> 拉回 $TARGET  ->  $ROOT/"
    # 反向也排除，避免把服务器的 build/ 拉回来覆盖本地
    rsync -az --delete $DRY "${EXCLUDES[@]}" -e "$SSH" "$TARGET" "$ROOT/"
}

case "$MODE" in
    push) sync_push ;;
    pull) sync_pull ;;
    build)
        sync_push
        echo "==> 服务器增量构建 + difftest"
        $SSH "${TARGET%%:*}" "cd ${TARGET#*:} && cmake --build build/release -j32 2>&1 | tail -2 && bash tools/difftest.sh build/release/pyadamas 2>&1 | tail -2"
        ;;
esac
