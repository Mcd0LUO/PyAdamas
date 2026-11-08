# 词法边界：转义、数字、raw 串、多行串、制表符、续行
#
# 这一组用例都是为了固定住「词法层的几处真 bug」：
#   1) 八进制转义 \101 以前不解码
#   2) f-string 字面量段以前不解码转义，且 rf"" 与 f"" 不分
#   3) 数字下划线以前不校验位置（1__0 / 1_ 都放行）
#   4) 多行字符串 token 的行号以前记的是结束行

oct_escape = "\101\102\103"          # ABC
hex_escape = "\x41\x42"
uni_escape = "\u4e2d\u6587"
wide_escape = "\U0001F600"
named_escape = "tab:\there\nnewline:\nquote:\"backslash:\\"
line_continuation = "abc\
def"

# 数字：下划线只能夹在数字之间
big = 1_000_000
hexnum = 0xFF_FF
octnum = 0o7_5_5
binnum = 0b1010_0101
floatnum = 1_234.5_6
expnum = 1e1_0
hex_lead = 0x_FF                     # 前缀后的下划线合法

# raw 字符串：反斜杠不转义
win_path = r"C:\Users\name\file.txt"
raw_quote = r"a\"b"

# 三引号：跨行、内含引号与反斜杠
multi = """first
second "quoted" \n literal
third"""
multi_raw = r'''raw
\n stays'''

# 制表符在缩进里按 8 列展开，与空格等价
if True:
	tab_indented = 1

# 空行、行尾注释、只有注释的行都要能过
# comment only

x = 1  # trailing

# 一元/二元混合、幂运算右结合
pow_right = 2 ** 3 ** 2
unary = -2 ** 2
chain = 1 < 2 < 3
