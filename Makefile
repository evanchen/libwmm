# Makefile —— libwmm 构建
#
# 变体:debug(-O0 -g + ASan/UBSan,跑测试用)/ release(-O2)。
# 两个编译约束与优化无关,是功能依赖,任何变体都不许去掉:
#   -fno-omit-frame-pointer   调用点采样靠帧指针链回溯栈
#   -fno-builtin-malloc       --wrap 接管时防止编译器把 malloc 调用
#                              内联优化掉、绕过 wrap
# 产物:libwmm.a(debug/release 各一套)+ release 的 libwmm.so。
# 用法:make debug | make release | make test | make bench | make clean

# CC 用直接赋值而不是条件赋值:make 对 CC 内建有 cc 默认值,而 ?= 只在
# 变量未定义时才生效、内建默认也算"已定义",于是 CC ?= gcc 永远解析
# 成 cc。Debian 上 cc 恰好是 gcc 的符号链接所以没暴露;换到没有 cc
# 的环境立即现形。命令行覆盖依然有效:make CC=clang
CC       = gcc
AR       = ar

CFLAGS_COMMON := -std=c11 -fno-omit-frame-pointer -fno-builtin-malloc \
                 -pthread -Wall -Wextra -Iinclude -Isrc
CFLAGS_DEBUG   := $(CFLAGS_COMMON) -O0 -g -fsanitize=address,undefined
CFLAGS_RELEASE := $(CFLAGS_COMMON) -O2 -g

SRC := $(wildcard src/*.c)

HDRS := include/wmm.h src/wmm_internal.h src/wmm_sc.h

BUILD_DEBUG   := build/debug
BUILD_RELEASE := build/release

.PHONY: all debug release test bench clean

all: release

debug: $(BUILD_DEBUG)/libwmm.a

release: $(BUILD_RELEASE)/libwmm.a $(BUILD_RELEASE)/libwmm.so

$(BUILD_DEBUG)/%.o: src/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_DEBUG) -c $< -o $@

$(BUILD_RELEASE)/%.o: src/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_RELEASE) -c $< -o $@

$(BUILD_DEBUG)/libwmm.a: $(patsubst src/%.c,$(BUILD_DEBUG)/%.o,$(SRC))
	$(AR) rcs $@ $^

$(BUILD_RELEASE)/libwmm.a: $(patsubst src/%.c,$(BUILD_RELEASE)/%.o,$(SRC))
	$(AR) rcs $@ $^

$(BUILD_RELEASE)/libwmm.so: $(patsubst src/%.c,$(BUILD_RELEASE)/%.o,$(SRC))
	$(CC) -shared -o $@ $^ -lpthread

# test:对 debug 库(带 sanitizer)编译并运行 tests/ 下的用例
test: debug
	@if [ -z "$(wildcard tests/test_*.c)" ]; then \
	    echo "tests/: 暂无用例(骨架阶段)"; exit 0; \
	fi
	@echo "tests/ 用例接入是首个里程碑的一部分,尚未落地" && exit 1

# bench:性能基准(与 glibc 对照);基准程序尚未实现
bench: release
	@echo "bench/bench.c 尚为空占位" && exit 1

clean:
	rm -rf build
