#!/bin/bash
# 清理 pya 编译缓存（modcache）
#
# 用法：
#   bash tools/clean_cache.sh              # 默认：progcache 保留最近 200 个，entry 保留最近 100 个
#   bash tools/clean_cache.sh --all        # 全清（下次构建全部重编）
#   bash tools/clean_cache.sh --prog-only  # 只清 progcache
#   KEEP_PROG=500 KEEP_ENTRY=200 bash tools/clean_cache.sh
#
# 背景：modcache 分两块
#   <module>-v0-<hash>/   模块级缓存（.o/.s/manifest/mod.pya 接口签名）
#   progcache/<key>.bin   程序级缓存（整个可执行文件）
# 两块都只增不减，实测 lead 构建曾累积到 11 GB（progcache 56634 个 .bin）。
set -u

BUILD=${PYA_BUILD_DIR:-build/lead}
C="${BUILD}/modcache"
KEEP_PROG=${KEEP_PROG:-200}
KEEP_ENTRY=${KEEP_ENTRY:-100}

MODE=${1:-default}

if [ ! -d "$C" ]; then
  echo "找不到缓存目录: $C（用 PYA_BUILD_DIR 指定构建目录，如 build/dev）"
  exit 1
fi

before=$(du -sk "$C" | cut -f1)
echo "缓存目录: $C"
echo "清理前: $(du -sh "$C" | cut -f1)"

case "$MODE" in
  --all)
    # 保留目录本身，清空内容
    find "$C" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
    ;;
  --prog-only)
    if [ -d "$C/progcache" ]; then
      mapfile -t bins < <(ls -t "$C/progcache"/*.bin 2>/dev/null)
      n=${#bins[@]}
      if [ "$n" -gt "$KEEP_PROG" ]; then
        printf '%s\0' "${bins[@]:$KEEP_PROG}" | xargs -0 -r rm -f
        echo "progcache: $n -> $KEEP_PROG"
      else
        echo "progcache: $n 个，未超上限 $KEEP_PROG，跳过"
      fi
    fi
    ;;
  *)
    # progcache 按 LRU 保留
    if [ -d "$C/progcache" ]; then
      mapfile -t bins < <(ls -t "$C/progcache"/*.bin 2>/dev/null)
      n=${#bins[@]}
      if [ "$n" -gt "$KEEP_PROG" ]; then
        printf '%s\0' "${bins[@]:$KEEP_PROG}" | xargs -0 -r rm -f
        echo "progcache: $n -> $KEEP_PROG"
      else
        echo "progcache: $n 个，未超上限 $KEEP_PROG"
      fi
    fi
    # entry 模块缓存按 LRU 保留（每次编译新程序都会新建，复用率最低）
    cd "$C" || exit 1
    mapfile -t dirs < <(ls -dt _1_1entry_1_1-v0-*/ 2>/dev/null)
    n=${#dirs[@]}
    if [ "$n" -gt "$KEEP_ENTRY" ]; then
      for i in "${!dirs[@]}"; do
        [ "$i" -ge "$KEEP_ENTRY" ] && rm -rf "${dirs[$i]}"
      done
      echo "entry 缓存: $n -> $KEEP_ENTRY"
    else
      echo "entry 缓存: $n 个，未超上限 $KEEP_ENTRY"
    fi
    cd - >/dev/null || true
    ;;
esac

after=$(du -sk "$C" | cut -f1)
echo "清理后: $(du -sh "$C" | cut -f1)  (释放 $(( (before-after)/1024 )) MB)"
