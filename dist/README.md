# dist/ —— 预编译发布包

这里放 `tools/make_release.sh` 打出来的自包含包（编译器二进制 + stdlib），由 CI/本地构建后提交，
方便不装编译环境的机器直接用。

```bash
tar xzf pyadamas-<ver>-linux-x86_64.tar.gz
./pyadamas-<ver>-linux-x86_64/bin/pyadamas run hello.py
```

- 依赖：x86-64 Linux + glibc + binutils 的 `as`/`ld`；
- 运行期从 `bin/pyadamas` 旁边找 `stdlib/`（顺序见 `src/driver/util.cpp` 的 `stdlib_dir()`），
  所以整个目录一起搬走即可，不需要设环境变量；
- 校验：同目录的 `.sha256`；
- 重新打包：`bash tools/make_release.sh`（默认用 `build/lead`）。
