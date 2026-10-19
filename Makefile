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

# -D_DEFAULT_SOURCE:-std=c11 是严格模式,glibc 会隐藏 MAP_ANONYMOUS /
# MAP_NORESERVE / MADV_* 与部分 POSIX 声明(这些都在 _DEFAULT_SOURCE
# 门下);该宏恢复发行版默认可见集,是 feature_test_macros 的标准姿势。
# -fPIC:同一套 .o 既进 libwmm.a 也进 libwmm.so,而共享库必须是位置
# 无关代码——缺了它链接 .so 时报 R_X86_64_PC32 can not be used:非 PIC
# 的对象把全局变量与函数地址全编成 PC 相对重定位,加载期无法改写
# 只读代码段去补这个偏移。x86-64 的寻址本来就是 RIP 相对,PIC 几乎
# 零开销,不值得为 .a 与 .so 维护两套目标目录。
CFLAGS_COMMON := -std=c11 -D_DEFAULT_SOURCE -fPIC -fno-omit-frame-pointer \
                 -fno-builtin-malloc -pthread -Wall -Wextra -Iinclude -Isrc
CFLAGS_DEBUG   := $(CFLAGS_COMMON) -O0 -g -fsanitize=address,undefined
CFLAGS_RELEASE := $(CFLAGS_COMMON) -O2 -g

SRC := $(wildcard src/*.c)

# 目标文件除头文件外还依赖 Makefile 本身:改了编译选项而没有头文件
# 变化时,依赖里没有 Makefile 就会复用过期的 .o——"加了 -fPIC 却还在
# 链旧对象"这类诡异错误就是这么来的。
HDRS := include/wmm.h src/wmm_internal.h src/wmm_sc.h src/wmm_shm.h Makefile

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

# test:对 debug 库(带 sanitizer)编译并运行 tests/ 下的全部用例。
# 用例是白盒的:可直接包含 src/wmm_internal.h 检查内部状态;
# 入口 tests/main.c 维护用例表,新增 tests/test_xxx.c 时在表中登记。
TEST_SRC := $(wildcard tests/test_*.c)
test: debug
	@mkdir -p build/test
	$(CC) $(CFLAGS_DEBUG) -o build/test/run tests/main.c $(TEST_SRC) \
	    $(BUILD_DEBUG)/libwmm.a -lpthread
	./build/test/run

# bench:性能基准(与 glibc 对照);基准程序尚未实现
bench: release
	@echo "bench/bench.c 尚为空占位" && exit 1

clean:
	rm -rf build
