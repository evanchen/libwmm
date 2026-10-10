/*
 * jemalloc 使用示例
 * ============================================================================
 * 本示例演示 jemalloc 公开 API 的常见用法,与 doc_internal/DESIGN.md 配套:
 *
 *   [1] 标准 C 分配器接口  malloc / calloc / realloc / free / aligned_alloc
 *   [2] 扩展 API           mallocx / rallocx / xallocx / sallocx / nallocx /
 *                          dallocx / sdallocx(携带 MALLOCX_* 标志)
 *   [3] 对齐 / 清零 / 按 arena / 绕开 tcache 分配
 *   [4] mallctl 运行期配置与统计(含 MIB 快速路径)
 *   [5] malloc_stats_print 自定义输出回调
 *   [6] malloc_usable_size / malloc_message
 *
 * 编译(在 doc_internal/ 目录下,以源码树内的头文件为准):
 *   gcc -I../include -O2 jemalloc_example.c -o jemalloc_example \
 *       -L../lib -ljemalloc -lpthread -lm
 *
 * 或者用构建产物自动给出参数:
 *   gcc $(../bin/jemalloc-config --cflags) jemalloc_example.c -o jemalloc_example \
 *       $(../bin/jemalloc-config --libs)
 *
 * 运行(可用 MALLOC_CONF 注入配置,本 fork 额外支持 lg_page 等选项):
 *   MALLOC_CONF="narenas:4,background_thread:true" ./jemalloc_example
 *   ./jemalloc_example            # 不带配置,观察"版本/默认值"输出
 *
 * 注意:
 *   - 头文件已处理符号重命名(je_ 前缀),业务代码直接调用标准名字即可;
 *   - 若用 LD_PRELOAD 方式替换 libc 分配器,则不需要链接,直接用标准接口;
 *   - 本 fork 特有配置(如 opt.lg_page)在上游 jemalloc 上不存在,mallctl
 *     会返回 ENOENT,示例中的 helper 会打印错误并继续,不影响演示。
 */

#include <jemalloc/jemalloc.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * 小工具:封装 mallctl 的 get / set / write-only 调用
 * ------------------------------------------------------------------------- */

static int
mallctl_get(const char *name, void *oldp, size_t *oldlenp) {
	return mallctl(name, oldp, oldlenp, NULL, 0);
}

static int
mallctl_set(const char *name, const void *newp, size_t newlen) {
	return mallctl(name, NULL, NULL, (void *)newp, newlen);
}

/* 读取一个 unsigned 类型的 mallctl 值 */
static unsigned
ctl_get_unsigned(const char *name) {
	unsigned v = 0;
	size_t len = sizeof(v);
	int err = mallctl_get(name, &v, &len);
	if (err != 0) {
		fprintf(stderr, "  [mallctl] 读取 %-28s 失败 (err=%d)\n", name, err);
		return 0;
	}
	return v;
}

/* 读取一个 bool 类型的 mallctl 值 */
static bool
ctl_get_bool(const char *name) {
	bool v = false;
	size_t len = sizeof(v);
	int err = mallctl_get(name, &v, &len);
	if (err != 0) {
		fprintf(stderr, "  [mallctl] 读取 %-28s 失败 (err=%d)\n", name, err);
		return false;
	}
	return v;
}

/* ---------------------------------------------------------------------------
 * [1] 标准 C 接口
 * ------------------------------------------------------------------------- */

static void
demo_standard_apis(void) {
	printf("== [1] 标准 C 接口 =====================================\n");

	void *p = malloc(1024);
	if (p == NULL) {
		perror("malloc");
		exit(EXIT_FAILURE);
	}
	/* 实际可用大小可能大于请求值(向上取整到 size class) */
	printf("  malloc(1024)     -> %p, usable_size = %zu\n", p,
	    malloc_usable_size(p));
	memset(p, 0xAB, 1024);

	void *q = calloc(16, 64); /* 清零分配 */
	printf("  calloc(16, 64)   -> %p (已清零)\n", q);

	q = realloc(q, 4096); /* 原地或搬移扩展 */
	printf("  realloc(q, 4096) -> %p\n", q);

	void *al = aligned_alloc(256, 4096); /* C11 对齐分配,对齐必须是 2 的幂 */
	printf("  aligned_alloc(256, 4096) -> %p\n", al);

	void *pm = NULL;
	if (posix_memalign(&pm, 4096, 8192) == 0) {
		printf("  posix_memalign(4096, 8192) -> %p\n", pm);
	}

	free(p);
	free(q);
	free(al);
	free(pm);
}

/* ---------------------------------------------------------------------------
 * [2][3] 扩展 API:MALLOCX_* 标志
 * ------------------------------------------------------------------------- */

static void
demo_extended_apis(void) {
	printf("== [2][3] 扩展 API 与 MALLOCX_* 标志 ====================\n");

	/* nallocx:不分配,只查询某个请求大小实际会得到的可用大小 */
	size_t usable = nallocx(12345, 0);
	printf("  nallocx(12345, 0)      -> %zu (可用大小)\n", usable);

	/* mallocx:分配 + 标志 */
	void *p = mallocx(1024, MALLOCX_ZERO); /* 等价于 calloc */
	printf("  mallocx(1024, ZERO)     -> %p (已清零)\n", p);

	/* 对齐分配:MALLOCX_ALIGN 接受实际对齐值 */
	void *a = mallocx(512, MALLOCX_ALIGN(4096));
	printf("  mallocx(512, ALIGN(4096)) -> %p\n", a);

	/* 绕开 tcache:MALLOCX_TCACHE_NONE,对象直接进/出 arena,适合"一次性的
	 * 大对象"或长期驻留对象 */
	void *nt = mallocx(64 * 1024, MALLOCX_TCACHE_NONE);
	printf("  mallocx(64K, TCACHE_NONE) -> %p\n", nt);

	/* sallocx:反查某指针对应的实际分配大小 */
	size_t sz = sallocx(a, 0);
	printf("  sallocx(a, 0)           -> %zu\n", sz);

	/* xallocx:原地扩容/缩容,返回最终实际大小;失败(需搬移)返回 0 */
	size_t new_sz = xallocx(a, 2048, 4096, 0); /* 目标 2048,最多额外接受 4096 */
	printf("  xallocx(a, 2048, 4096)  -> %zu\n", new_sz);

	/* rallocx:重新分配(可搬移),new_size == 0 等价 free */
	void *r = rallocx(p, 8192, 0);
	printf("  rallocx(p, 8192)        -> %p\n", r);

	/* 显式释放 */
	dallocx(r, 0);
	dallocx(a, 0);
	dallocx(nt, MALLOCX_TCACHE_NONE); /* 释放也绕开 tcache */

	/* sdallocx:带尺寸的释放,省去 sallocx 反查,是 free 的更快变体;
	 * 传入的 size 必须是 sallocx()/nallocx() 的实际大小 */
	void *s = mallocx(1024, 0);
	size_t s_sz = sallocx(s, 0);
	sdallocx(s, s_sz, 0);
	printf("  sdallocx(s, %zu, 0)      done\n", s_sz);
}

/* ---------------------------------------------------------------------------
 * [4] mallctl:配置查询、arena 管理与统计
 * ------------------------------------------------------------------------- */

static void
demo_mallctl(void) {
	printf("== [4] mallctl ==========================================\n");

	/* 只读查询:字符串 / 布尔 / 无符号 */
	{
		char ver[64];
		size_t len = sizeof(ver);
		if (mallctl_get("version", ver, &len) == 0) {
			printf("  version               = %s\n", ver);
		}
	}
	printf("  config.prof           = %s\n",
	    ctl_get_bool("config.prof") ? "true" : "false");
	printf("  opt.background_thread = %s\n",
	    ctl_get_bool("opt.background_thread") ? "true" : "false");
	{
		/* 本 fork 的 lg_page(动态页大小)选项,类型为 unsigned;
		 * 上游无此选项会返回 ENOENT */
		unsigned lg = 0;
		size_t len = sizeof(lg);
		if (mallctl_get("opt.lg_page", &lg, &len) == 0) {
			printf("  opt.lg_page           = %u (page = %zu)\n", lg,
			    (size_t)1 << lg);
		} else {
			printf("  opt.lg_page           = <上游版本无此选项>\n");
		}
	}

	/* 动态创建 arena,并指定分配落到它里面。
	 * 语义:oldp 返回新 arena 的索引,newp 为可选的自定义 extent hooks */
	unsigned arena_ind = 0;
	size_t len = sizeof(arena_ind);
	if (mallctl("arenas.create", &arena_ind, &len, NULL, 0) == 0) {
		printf("  arenas.create         -> arena.%u\n", arena_ind);
		void *in_arena = mallocx(1 * 1024 * 1024, MALLOCX_ARENA(arena_ind));
		printf("  mallocx(1M, ARENA(%u))  -> %p\n", arena_ind, in_arena);

		/* 对某个 arena 手动触发 purge(把脏页还给 OS) */
		char purge[32];
		snprintf(purge, sizeof(purge), "arena.%u.purge", arena_ind);
		if (mallctl_set(purge, NULL, 0) == 0) {
			printf("  %-28s done\n", purge);
		}

		/* MIB 快速路径:名字先翻译成索引数组,之后免字符串比较 */
		size_t mib[3];
		size_t miblen = 3;
		if (mallctlnametomib("arena.0.purge", mib, &miblen) == 0) {
			mib[1] = arena_ind; /* 把 arena 下标改成刚创建的 */
			if (mallctlbymib(mib, miblen, NULL, NULL, NULL, 0) == 0) {
				printf("  mallctlbymib(arena.<%u>.purge) done\n", arena_ind);
			}
		}

		dallocx(in_arena, 0);
	} else {
		printf("  arenas.create         failed\n");
	}

	/* 统计汇总:allocated / active / resident / mapped / metadata */
	printf("  stats.allocated       = %zu B\n", (size_t)ctl_get_unsigned("stats.allocated"));
	printf("  stats.active          = %zu B\n", (size_t)ctl_get_unsigned("stats.active"));
	printf("  stats.resident        = %zu B\n", (size_t)ctl_get_unsigned("stats.resident"));
	printf("  stats.mapped          = %zu B\n", (size_t)ctl_get_unsigned("stats.mapped"));
	printf("  stats.metadata        = %zu B\n", (size_t)ctl_get_unsigned("stats.metadata"));
	printf("  arenas.narenas        = %u\n", ctl_get_unsigned("arenas.narenas"));
	printf("  thread.arena          = %u\n", ctl_get_unsigned("thread.arena"));
}

/* ---------------------------------------------------------------------------
 * [5] malloc_stats_print:自定义输出
 * ------------------------------------------------------------------------- */

static void
stats_write_cb(void *cbopaque, const char *s) {
	(void)cbopaque;
	fputs(s, stdout);
}

static void
demo_stats_print(void) {
	printf("== [5] malloc_stats_print ===============================\n");
	printf("  -- 默认(opts=NULL,合并统计)--\n");
	malloc_stats_print(stats_write_cb, NULL, NULL);
	printf("\n  -- JSON 紧凑格式(opts=\"J\")--\n");
	malloc_stats_print(stats_write_cb, NULL, "J");
	printf("\n");
	/* 其他常用 opts:
	 *   "a"  合并统计(default)
	 *   "d"  统计已销毁的 arena
	 *   "g"  未合并(逐 arena)统计
	 *   "j"/"J"  JSON / 紧凑 JSON */
}

/* ---------------------------------------------------------------------------
 * [6] malloc_message / je_ 前缀说明
 * ------------------------------------------------------------------------- */

static void
demo_misc(void) {
	printf("== [6] 其他 =============================================\n");
	printf("  版本宏:JEMALLOC_VERSION=\"%s\"\n", JEMALLOC_VERSION);

	/* malloc_message 是全局函数指针,可用于接管分配器内部告警输出
	 * (madvise 失败、OOM 提示等)。替换前保存旧指针即可恢复。 */
	printf("  malloc_message 当前指向 %s\n",
	    malloc_message != NULL ? "默认实现" : "NULL");

	/* 可通过 malloc_conf 在链接期注入默认配置:
	 *   const char *malloc_conf = "narenas:4,background_thread:true";
	 * 或通过环境变量 MALLOC_CONF 注入(优先级见 DESIGN.md §3.1)。 */
}

int
main(void) {
	demo_standard_apis();
	demo_extended_apis();
	demo_mallctl();
	demo_stats_print();
	demo_misc();

	printf("\n全部示例执行完毕。\n");
	return 0;
}
