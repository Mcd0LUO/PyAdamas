#!/usr/bin/env python3
"""Codon 对比（收编 0.482）：PyAdamas vs Codon 0.20.7，绑核、交替 N 轮取最小、输出对 CPython。

用法：
  python3 tools/codon_cmp.py [--rounds 5] [用例 ...]

口径（与 docs/experiment-switches.md §2、bench/compare/RESULTS-0.480.md 一致）：
  - 同一份 .py；PyAdamas 走 build/lead/pyadamas build -O2 --rebuild；Codon 走
    .ref_proj/tools/codon-0.20.7/bin/codon build -release；
  - taskset 绑 cpuset 首核；每轮交换 A/B 先后；取各侧最小值；
  - 两侧输出都必须与 CPython 逐字节一致，否则标 out=False 并在汇总里暴露；
  - obj_dispatch 用 .ref_proj/benchmarks/obj_dispatch_codon.py（Codon 需要显式字段标注）。

已知不可比：binary_trees / fannkuch 用 Codon 0.20.7 编不过（本仓源码的泛型标注不兼容）。
"""
import argparse, json, math, os, subprocess, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PA = os.path.join(ROOT, 'build/lead/pyadamas')
CODON = os.path.join(ROOT, '.ref_proj/tools/codon-0.20.7/bin/codon')
CASES = ['binary_trees', 'fannkuch', 'fib', 'mandelbrot', 'matmul', 'nbody',
         'nqueens', 'spectral_norm', 'obj_dispatch']


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cases', nargs='*', default=None)
    ap.add_argument('--rounds', type=int, default=5)
    ap.add_argument('--out', default='')
    args = ap.parse_args()
    cases = args.cases or CASES
    cpu = sh("awk '/Cpus_allowed_list/ {print $2}' /proc/self/status").stdout.strip().split(',')[0].split('-')[0]
    outdir = '/tmp/codon_cmp'
    os.makedirs(outdir, exist_ok=True)
    res = {}
    for c in cases:
        src = (os.path.join(ROOT, 'bench/realistic/obj_dispatch.py') if c == 'obj_dispatch'
               else os.path.join(ROOT, 'bench/compare', c + '.py'))
        csrc = (os.path.join(ROOT, '.ref_proj/benchmarks/obj_dispatch_codon.py')
                if c == 'obj_dispatch' else src)
        pa_bin = os.path.join(outdir, 'pa_' + c)
        co_bin = os.path.join(outdir, 'co_' + c)
        if sh('PYADAMAS_CACHE_DIR=%s/cache %s build -O2 --rebuild -o %s %s' % (outdir, PA, pa_bin, src)).returncode != 0:
            print('skip %s: pyadamas build fail' % c); continue
        if sh('%s build -release -o %s %s' % (CODON, co_bin, csrc)).returncode != 0:
            print('skip %s: codon build fail' % c); continue
        ref = sh('python3 %s' % src).stdout.strip()
        pa_out = sh('taskset -c %s %s' % (cpu, pa_bin)).stdout.strip()
        co_out = sh('taskset -c %s %s' % (cpu, co_bin)).stdout.strip()
        ta, tb = [], []
        for i in range(args.rounds):
            for side in (('a', 'b') if i % 2 else ('b', 'a')):
                b_ = pa_bin if side == 'a' else co_bin
                t0 = time.perf_counter(); sh('taskset -c %s %s' % (cpu, b_)); dt = (time.perf_counter() - t0) * 1000
                (ta if side == 'a' else tb).append(dt)
        pa, co = min(ta), min(tb)
        res[c] = {'pa_ms': round(pa, 1), 'codon_ms': round(co, 1), 'ratio': round(pa / co, 3),
                  'out_pa': pa_out == ref, 'out_codon': co_out == ref}
        print('%-15s pa=%8.1f codon=%8.1f ratio=%5.3f out=%s' % (c, pa, co, pa / co, pa_out == ref and co_out == ref))
    if res:
        g = math.exp(sum(math.log(v['ratio']) for v in res.values()) / len(res))
        print('geomean PA/Codon = %.3f (n=%d)' % (g, len(res)))
    if args.out:
        json.dump(res, open(args.out, 'w'), indent=1)
        print('json:', args.out)


if __name__ == '__main__':
    main()