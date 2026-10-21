# Makefile —— libwmm 构建
#
# 变体:debug(-O0 -g + ASan/UBSan,跑测试用)/ release(-O2)。
# 两个编译约束与优化无关,是功能依赖,任何变体都不许去掉:
#   -fno-omit-frame-pointer   调用点采样靠帧指针链回溯栈
#   -fno-builtin-malloc       --wrap 接管时防止编译器把 malloc 调用
#                              内联优化掉、绕过 wrap
# 产物:libwmm.a(debug/release 各一套)+ release 的 libwmm.so。
# 用法:make debug | make release | make test | make tsan | make bench |
#       make monitor | make playbook | make clean |
#       make validate | make soak | make forkstress | make oomcheck |
#       make cgroup-oom | make validate-full

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
# TSan 不能与 ASan 混用,单独一套目标目录。-O0 同时保证自定义
# seqlock 的收发函数不被内联,tsan.supp 才能按函数名稳定匹配。
CFLAGS_TSAN    := $(CFLAGS_COMMON) -O0 -g -fsanitize=thread

SRC := $(wildcard src/*.c)

# 目标文件除头文件外还依赖 Makefile 本身:改了编译选项而没有头文件
# 变化时,依赖里没有 Makefile 就会复用过期的 .o——"加了 -fPIC 却还在
# 链旧对象"这类诡异错误就是这么来的。
HDRS := include/wmm.h src/wmm_internal.h src/wmm_sc.h src/wmm_shm.h Makefile

BUILD_DEBUG   := build/debug
BUILD_RELEASE := build/release
BUILD_TSAN    := build/tsan

.PHONY: all debug release test tsan bench monitor playbook clean \
        validate-bin soak forkstress oomcheck cgroup-oom validate \
        validate-full

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
# test_wrap.c 是例外:它自带 main,且必须以 --wrap 链接。主 run
# 二进制故意不 wrap——test_oracle 的 glibc 对照模式要在同一进程里
# 调用真的 malloc 家族——所以它单独构建成 build/test/wrap。
TEST_SRC := $(filter-out tests/test_wrap.c,$(wildcard tests/test_*.c))

WRAP_BIN   := build/test/wrap
WRAP_FUNCS := malloc free calloc realloc reallocarray posix_memalign aligned_alloc
# 不能用 addprefix:它的参数按逗号分隔,前缀里的 -Wl,--wrap= 会被
# 拆成两段。foreach 对每个名字单独展开,逗号原样留在命令行里。
WRAP_LDFLAGS := $(foreach f,$(WRAP_FUNCS),-Wl,--wrap=$(f))
# --wrap 只改写符号引用;编译器若把这些函数识别成内建并直接化简,
# 链接器就看不到引用。malloc 已在 CFLAGS_COMMON 里关掉,这里补齐
# 其余六个入口。
WRAP_CFLAGS := -fno-builtin-calloc -fno-builtin-realloc \
               -fno-builtin-reallocarray -fno-builtin-free \
               -fno-builtin-posix_memalign -fno-builtin-aligned_alloc

$(WRAP_BIN): tests/test_wrap.c $(BUILD_DEBUG)/libwmm.a $(HDRS)
	@mkdir -p build/test
	$(CC) $(CFLAGS_DEBUG) $(WRAP_CFLAGS) $(WRAP_LDFLAGS) -o $@ $< \
	    $(BUILD_DEBUG)/libwmm.a -lpthread

test: debug $(WRAP_BIN)
	@mkdir -p build/test
	$(CC) $(CFLAGS_DEBUG) -o build/test/run tests/main.c $(TEST_SRC) \
	    $(BUILD_DEBUG)/libwmm.a -lpthread
	./build/test/run
	@echo "---- wrap audit ----"
	@if nm -u $(WRAP_BIN) | grep -Eq 'U (malloc|free|calloc|realloc|reallocarray|posix_memalign|aligned_alloc)(@|$$)'; then \
	    echo "wrap audit: 仍有未接管的 libc 分配符号:"; \
	    nm -u $(WRAP_BIN) | grep -E 'U (malloc|free|calloc|realloc|reallocarray|posix_memalign|aligned_alloc)(@|$$)'; \
	    exit 1; \
	fi
	@for f in $(WRAP_FUNCS); do \
	    if ! nm $(WRAP_BIN) | grep -q " T __wrap_$$f"; then \
	        echo "wrap audit: 缺少 __wrap_$$f"; exit 1; \
	    fi; \
	done
	@echo "wrap audit: all allocator references go through __wrap_*"
	./$(WRAP_BIN)

# tsan:数据竞争检查。只跑主测试二进制(带 --wrap 的 wrap 程序是
# 单线程冒烟,对竞争检测没有增量;而 TSan 与 --wrap 一起用时,接管
# 范围之外的解释器行为更容易制造噪声)。
# tests/tsan.supp 只抑制两处自实现 seqlock 的 payload 假阳性,见文件
# 内注释;其余任何 race 报告都当失败。
TSAN_BIN := build/test/run_tsan

$(BUILD_TSAN)/%.o: src/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_TSAN) -c $< -o $@

$(BUILD_TSAN)/libwmm.a: $(patsubst src/%.c,$(BUILD_TSAN)/%.o,$(SRC))
	$(AR) rcs $@ $^

$(TSAN_BIN): tests/main.c $(TEST_SRC) $(BUILD_TSAN)/libwmm.a $(HDRS)
	@mkdir -p build/test
	$(CC) $(CFLAGS_TSAN) -o $@ tests/main.c $(TEST_SRC) \
	    $(BUILD_TSAN)/libwmm.a -lpthread

tsan: $(TSAN_BIN)
	@echo "---- tsan ----"
	TSAN_OPTIONS="suppressions=$(abspath tests/tsan.supp) halt_on_error=1" \
	    ./$(TSAN_BIN)

# 监控进程是纯读端:只包含两个协议头,不链接 libwmm,也不共用
# 库的编译约束。这样它可以单独部署、单独升级,目标进程甚至不需要
# 知道监控端的存在。
MON_CFLAGS := -std=c11 -D_DEFAULT_SOURCE -O2 -g -Wall -Wextra \
              -Iinclude -Isrc
MON_BIN    := build/monitor/monitor

monitor: $(MON_BIN)

$(MON_BIN): monitor/monitor.c src/wmm_shm.h include/wmm.h Makefile
	@mkdir -p build/monitor
	$(CC) $(MON_CFLAGS) -o $@ $<

# 基准与 glibc 对照。Makefile 把实现与场景拆成独立进程调用:
# 同一进程连跑四个场景时,后一行 RSS 会带上前一行没归还的分配器
# 存量,横向不可比。BENCH_ARGS 可追加参数,例如
# make bench BENCH_ARGS="--ops 5000000 --threads 8"
BENCH_BIN := build/bench/bench

$(BENCH_BIN): bench/bench.c $(BUILD_RELEASE)/libwmm.a $(HDRS)
	@mkdir -p build/bench
	$(CC) $(CFLAGS_RELEASE) -o $@ bench/bench.c \
	    $(BUILD_RELEASE)/libwmm.a -lpthread

bench: release $(BENCH_BIN)
	@echo "impl,scenario,ops,seconds,ops_per_sec,p50_ns,p99_ns,rss_kb,p999_ns,p9999_ns,max_ns,reqs,req_per_sec,alloc_fail"
	@for sc in seq mt mixed large server; do \
	    for impl in wmm glibc; do \
	        ./$(BENCH_BIN) $$impl --scenario $$sc --no-header \
	            --requests $(BENCH_REQUESTS) $(BENCH_ARGS) || exit 1; \
	    done; \
	done

# 生产验证基座。所有探针链接 release 库,不挂 sanitizer:压测关注
# 的是真实吞吐与 RSS,ASan 的 shadow 与插桩会改变两者,竞态与越界
# 由 make test / make tsan 覆盖。
#
# 每个探针的参数都可以用变量追加;SOAK_ARGS 例如
#   make soak SOAK_SECONDS=3600 SOAK_ARGS="--threads 8"
VALIDATE_DIR    := build/validate
VALIDATE_CFLAGS := $(CFLAGS_RELEASE)
VALIDATE_BINS   := $(VALIDATE_DIR)/soak $(VALIDATE_DIR)/forkstress \
                   $(VALIDATE_DIR)/oomcheck

SOAK_SECONDS  ?= 60
BENCH_REQUESTS ?= 200000
SOAK_ARGS     ?=
FORK_ARGS     ?=
OOM_ARGS      ?=
CGROUP_ARGS   ?=
VALIDATE_ARGS ?=

$(VALIDATE_DIR):
	mkdir -p $@

$(VALIDATE_DIR)/%: validate/%.c validate/validate_common.h \
                    $(BUILD_RELEASE)/libwmm.a $(HDRS) | $(VALIDATE_DIR)
	$(CC) $(VALIDATE_CFLAGS) $< -o $@ $(BUILD_RELEASE)/libwmm.a -lpthread

validate-bin: $(VALIDATE_BINS)

soak: release $(VALIDATE_DIR)/soak
	./$(VALIDATE_DIR)/soak --seconds $(SOAK_SECONDS) $(SOAK_ARGS)

forkstress: release $(VALIDATE_DIR)/forkstress
	./$(VALIDATE_DIR)/forkstress $(FORK_ARGS)

oomcheck: release $(VALIDATE_DIR)/oomcheck
	./$(VALIDATE_DIR)/oomcheck --window-fill $(OOM_ARGS)

cgroup-oom: release $(VALIDATE_DIR)/oomcheck
	@bash validate/cgroup_oom.sh --probe $(VALIDATE_DIR)/oomcheck \
	    $(CGROUP_ARGS); \
	rc=$$?; \
	if [ $$rc -eq 77 ] && [ "$${REQUIRE_CGROUP:-0}" != 1 ]; then \
	    echo "cgroup: SKIP (无 cgroup v2 memory 权限,也没有可用的 systemd)"; \
	    exit 0; \
	fi; \
	exit $$rc

validate: release $(BENCH_BIN) validate-bin
	@mkdir -p $(VALIDATE_DIR)
	python3 validate/gate.py --bench-bin $(BENCH_BIN) \
	    --soak-bin $(VALIDATE_DIR)/soak \
	    --fork-bin $(VALIDATE_DIR)/forkstress \
	    --oom-bin $(VALIDATE_DIR)/oomcheck \
	    --out-dir $(VALIDATE_DIR) \
	    --bench-requests $(BENCH_REQUESTS) \
	    $(VALIDATE_ARGS)

validate-full: release $(BENCH_BIN) validate-bin
	@mkdir -p $(VALIDATE_DIR)
	python3 validate/gate.py --bench-bin $(BENCH_BIN) \
	    --soak-bin $(VALIDATE_DIR)/soak \
	    --fork-bin $(VALIDATE_DIR)/forkstress \
	    --oom-bin $(VALIDATE_DIR)/oomcheck \
	    --out-dir $(VALIDATE_DIR) \
	    --bench-requests $(BENCH_REQUESTS) \
	    --soak-seconds 1800 --repeats 3 \
	    $(VALIDATE_ARGS)

# 演练负载用 release 库:故障模式故意写坏内存,ASan 会抢在分配器
# 之前终止进程。-no-pie 让事件与采样里的 pc 与文件虚拟地址一致,
# addr2line -e build/demo/playbook 可以直接解析。
PLAYBOOK_BIN := build/demo/playbook

$(PLAYBOOK_BIN): demo/playbook.c $(BUILD_RELEASE)/libwmm.a $(HDRS)
	@mkdir -p build/demo
	$(CC) $(CFLAGS_RELEASE) -no-pie -o $@ demo/playbook.c \
	    $(BUILD_RELEASE)/libwmm.a -lpthread

playbook: $(PLAYBOOK_BIN)

clean:
	rm -rf build
