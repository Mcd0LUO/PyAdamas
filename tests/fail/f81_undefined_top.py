# expect: undefined name 'VALU'
# pya-lsp 缺口 4（0.484）：模块顶层语句里的未定义名字也要在 check 阶段报。
# 之前只有函数体内才报（tests/fail/f09），顶层拼错要等 build 才暴露。
VALUE: int = 1
print(VALU)