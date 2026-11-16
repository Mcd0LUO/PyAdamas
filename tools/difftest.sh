#!/bin/bash
# 差分测试：同一份源码在 -O0 / -O1 / -O2 下输出必须一致。
# 用途：优化遍引入静默错误（野指针、越界、算错）时立刻抓出来。
cd /home/lenovo/cpp/cpy23 || exit 1
PY=${1:-build/dev/pyadamas}
fail=0; n=0
for f in tests/run/*.py; do
  name=$(basename "$f" .py)
  case "$name" in
    f*) continue ;;   # 反例组：预期编译失败
  esac
  n=$((n+1))
  out0=$(timeout -s KILL 20 "$PY" build -O0 -o /tmp/dt0 "$f" >/dev/null 2>&1 && timeout -s KILL 20 /tmp/dt0 2>&1)
  rc0=$?
  out2=$(timeout -s KILL 20 "$PY" build -O2 -o /tmp/dt2 "$f" >/dev/null 2>&1 && timeout -s KILL 20 /tmp/dt2 2>&1)
  rc2=$?
  if [ "$out0" != "$out2" ] || [ $rc0 -ne $rc2 ]; then
    echo "  DIFF $name (O0 rc=$rc0, O2 rc=$rc2)"
    echo "    O0: $(echo "$out0" | head -2 | tr '\n' ' ')"
    echo "    O2: $(echo "$out2" | head -2 | tr '\n' ' ')"
    fail=$((fail+1))
  fi
done
echo "差分测试: $n 个用例, $fail 个不一致"
exit $([ $fail -eq 0 ] && echo 0 || echo 1)