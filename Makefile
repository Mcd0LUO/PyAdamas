# py23 —— 增量构建
#
#   make                   开发构建（-O0 -g）→ build/dev/py23
#   make MODE=release      发布构建（-O2）   → build/release/py23
#   make -j                并行编译（本机 32 核，全量约 3s）
#   make test              跑语法/类型回归（正例 + 反例）
#   make clean
#
# 为什么默认 dev：这个项目自身的运行时间只有十几毫秒，-O0 完全够用；
# 而 -O2 会让标准库模板的优化阶段占掉八成编译时间。真正需要跑分时
# 再 make MODE=release。
#
# 增量原理：源码按翻译单元拆开，改哪个 .cpp 只重编哪个 .o。
# -MMD -MP 生成的头文件依赖记录在 build/*.d，改 .h 时只重编依赖它的单元。
# 两个 MODE 用不同目录，避免 -O 级别不同的 .o 混在一起。

MODE ?= dev

ifeq ($(MODE),dev)
  OPT   := -O0 -g
  BUILD := build/dev
else
  OPT   := -O2
  BUILD := build/release
endif

CXX      ?= g++
CC       ?= cc
CXXFLAGS := -std=c++23 $(OPT) -Wall -Wextra -Isrc
TARGET   := $(BUILD)/py23

SRCDIR := src
SRCS   := $(SRCDIR)/main.cpp $(SRCDIR)/parser.cpp $(SRCDIR)/typecheck.cpp \
          $(SRCDIR)/lower.cpp $(SRCDIR)/codegen.cpp $(SRCDIR)/ir.cpp \
          $(SRCDIR)/desugar.cpp
OBJS   := $(SRCS:$(SRCDIR)/%.cpp=$(BUILD)/%.o)
DEPS   := $(OBJS:.o=.d)

# 运行时库：生成的程序要链接它，py23 自身不需要。
RT_SRC := runtime/rt.c
RT_OBJ := $(BUILD)/rt.o
CXXFLAGS += -DPY23_RT_OBJ=\"$(abspath $(RT_OBJ))\"

.PHONY: all clean test dev

all: $(TARGET) $(RT_OBJ)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(RT_OBJ): $(RT_SRC)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra -c $< -o $@

-include $(DEPS)

# tests/*.py       必须通过 check（原文里写清楚的合法程序）
# tests/fail/*.py  必须报错，且第一行 "# expect: xxx" 给出期望的错误片段。
#                  只测「能通过」会让检查器越改越松，必须同时钉住「该报的还得报」。
# tests/run/*.py   必须能编译成可执行文件，且运行输出与文件末尾
#                  "# --- expect ---" 之后的注释逐行一致。
# tests/unsupported/*.py
#                  语法与类型都得过，但后端必须拒绝。第一行
#                  "# unsupported: xxx" 给出 check -v 里期望的标记。
#                  这类用例防止「标了没建模、实际却悄悄编译过去」。
test: $(TARGET) $(RT_OBJ)
	@fail=0; \
	for f in tests/*.py; do \
		printf '  %-28s ' "$$(basename $$f)"; \
		./$(TARGET) check "$$f" | head -1 || fail=1; \
	done; \
	for f in tests/fail/*.py; do \
		want=$$(head -1 "$$f" | sed -n 's/^# expect: //p'); \
		got=$$(./$(TARGET) check "$$f" 2>&1 >/dev/null | head -1); \
		if [ -z "$$got" ]; then \
			printf '  %-28s MISSED (expected: %s)\n' "$$(basename $$f)" "$$want"; fail=1; \
		elif ! printf '%s' "$$got" | grep -qF "$$want"; then \
			printf '  %-28s WRONG: %s\n' "$$(basename $$f)" "$$got"; fail=1; \
		else \
			printf '  %-28s rejected (%s)\n' "$$(basename $$f)" "$$want"; \
		fi; \
	done; \
	for f in tests/run/*.py; do \
		name=$$(basename $$f); \
		if ! ./$(TARGET) run -o $(BUILD)/prog.tmp "$$f" > $(BUILD)/out.txt 2>&1; then \
			printf '  %-28s FAILED\n' "$$name"; sed -n '1,6p' $(BUILD)/out.txt; fail=1; continue; \
		fi; \
		sed -n '/^# --- expect ---$$/,$$p' "$$f" | tail -n +2 | sed -E 's/^# ?//' > $(BUILD)/exp.txt; \
		if diff -q $(BUILD)/out.txt $(BUILD)/exp.txt >/dev/null 2>&1; then \
			printf '  %-28s ran ok\n' "$$name"; \
		else \
			printf '  %-28s OUTPUT DIFF\n' "$$name"; \
			diff $(BUILD)/exp.txt $(BUILD)/out.txt | head -12; fail=1; \
		fi; \
	done; \
	for f in tests/unsupported/*.py; do \
		want=$$(head -1 "$$f" | sed -n 's/^# unsupported: //p'); \
		name=$$(basename $$f); \
		got=$$(./$(TARGET) check -v "$$f" 2>&1 >/dev/null | head -1); \
		if ! printf '%s' "$$got" | grep -qF "$$want"; then \
			printf '  %-28s NOT MARKED (expected: %s)\n' "$$name" "$$want"; fail=1; \
		elif ./$(TARGET) build -o $(BUILD)/prog.tmp "$$f" >/dev/null 2>&1; then \
			printf '  %-28s COMPILED ANYWAY\n' "$$name"; fail=1; \
		else \
			printf '  %-28s parsed, refused by backend\n' "$$name"; \
		fi; \
	done; \
	rm -f $(BUILD)/prog.tmp $(BUILD)/out.txt $(BUILD)/exp.txt; \
	exit $$fail

clean:
	rm -rf build py23

# 打印当前实际使用的编译命令，便于排查
info:
	@echo "MODE=$(MODE)  OPT=$(OPT)  BUILD=$(BUILD)"
	@echo "sources: $(SRCS)"
