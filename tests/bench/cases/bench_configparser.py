"""configparser 基准：解析 60 段配置 + 取值 + 插值。"""
import configparser

lines = []
i = 0
while i < 60:
    lines.append("[sec" + str(i) + "]")
    lines.append("alpha = " + str(i))
    lines.append("beta = value_%d" % i)
    lines.append("gamma = %(alpha)s/x")
    i += 1
text = "\n".join(lines)

total = 0
r = 0
while r < 120:
    cp = configparser.ConfigParser()
    cp.read_string(text)
    total += len(cp.sections())
    j = 0
    while j < 60:
        s = "sec" + str(j)
        total += len(cp.get(s, "beta"))
        total += len(cp.get(s, "gamma"))
        total += cp.getint(s, "alpha")
        j += 1
    r += 1
print(total)
