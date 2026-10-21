/* bench.c —— libwmm 与 glibc malloc 的同负载对照基准
 *
 * 五种场景共用一套负载代码,分配器经函数指针选择:
 *   seq    单线程顺序:反复 alloc(64) 后立即 free;
 *   mt     N 个线程各自在随机槽位上 alloc/free,槽位与随机流互不共享;
 *   mixed  单线程大小混合:尺寸在 36 个小对象类里随机取;
 *   large  单线程大对象:16KB/64KB/256KB/1MB 随机,逐页触碰;
 *   server 模拟一次请求:取 1..6 个对象,少量 realloc,把一部分
 *          对象留在会话槽位里跨请求存活,周期性驱逐。
 *
 * server 支持 --server-big/realloc/session/touch 0|1 四个诊断开关,
 * 关闭时仍消耗同一串随机数,只跳过对应动作;每次运行把最慢请求的
 * 尺寸、调用数与标志打印到 stderr,CSV 格式与默认负载不变。
 *
 * 除 server 外,一次 op 定义为一次分配器调用(alloc 或 free),吞吐
 * 与分位都按调用计。server 的计时单位是一次请求(含请求内的全部
 * 分配器调用),CSV 里 ops 仍是分配器调用数、reqs 才是请求数,两者
 * 都输出,避免把"请求吞吐"和"调用吞吐"混为一谈。延迟在计时单位
 * 前后各读一次 CLOCK_MONOTONIC,含两次时钟读取的固定开销(约几十
 * 纳秒),两个分配器付同样的代价。
 *
 * 为什么要函数指针:同一份循环既跑 wmm_alloc 也跑 malloc,编译器
 * 无法在编译期把某一对调用折叠掉;两个实现走完全相同的代码路径,
 * 差别只剩分配器本身。
 *
 * 基准自身的元数据(槽位表、延迟数组、线程参数)全部用匿名 mmap,
 * 不经过任何一个被测分配器:否则 glibc 模式会把基准开销记进自己的
 * RSS,wmm 模式则会让元数据挤进被测堆。RSS 在延迟数组释放、槽位
 * 清理完成之后读取,尽量只剩分配器与进程本身的驻留页。
 *
 * 随机槽位而不是扫描空位:占用率才能在中间均衡,缓存水位有涨有落,
 * 不会一直钉在"全空"或"全满"的边界上。
 *
 * 输出 CSV:前 8 列保持旧格式,新列追加在 rss_kb 之后:
 *   impl,scenario,ops,seconds,ops_per_sec,p50_ns,p99_ns,rss_kb,
 *   p999_ns,p9999_ns,max_ns,reqs,req_per_sec,alloc_fail
 * 默认一次跑五个场景;Makefile 把每个实现/场景拆成独立进程调用,
 * --scenario 只跑一个,这样每行的 RSS 不含前一个场景留下的分配器
 * 存量,横向可比。
 *
 * RSS 口径是"默认保留策略":wmm 模式不启动后台 purge 线程,glibc
 * 也不主动 trim,两边都把释放后的页留在各自缓存里。这不是要比较
 * 谁更省 RSS,而是让同一场景下的驻留差异反映分配策略本身。
 *
 * 用法:
 *   bench <wmm|glibc> [--scenario seq|mt|mixed|large|server|all]
 *         [--threads N] [--ops N] [--large-ops N] [--requests N]
 *         [--seed N] [--no-header]
 */
#include "wmm.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

typedef void *(*alloc_fn_t)(size_t);
typedef void  (*free_fn_t)(void *);
typedef void *(*realloc_fn_t)(void *, size_t);

/* 被测分配器的入口;启动时二选一,之后所有场景只认这三个指针 */
static alloc_fn_t   g_alloc;
static free_fn_t    g_free;
static realloc_fn_t g_realloc;

static const char *g_impl;            /* 当前实现名,写进结果行 */
static const char *g_bench_scenario;  /* 当前场景名,写进结果行 */
static size_t      g_page = 4096;     /* 运行时页大小;大对象按页触碰 */

/* 分配失败计数:小对象场景不该失败,真失败了也要在表末提醒,否则
   吞吐数字会被"少做了工作"悄悄抬高。多线程下用原子加 */
static _Atomic uint64_t g_alloc_fail;

enum {
    MT_SLOTS     = 512,   /* 多线程场景每线程的随机槽位数 */
    SINGLE_SLOTS = 512,   /* 大小混合场景的槽位数 */
    LARGE_SLOTS  = 32,    /* 大对象场景同时在手的对象数 */
    SERVER_REQ_SLOTS   = 8,     /* 一次请求内同时在手的对象数 */
    SERVER_SESS_SLOTS  = 64,    /* 跨请求存活的会话槽位数 */
    SERVER_WARMUP      = 1000,  /* 计时前每线程的请求数 */
    SERVER_FLUSH_EVERY = 64,    /* 每 N 次请求清空一次会话槽位 */
};

/* server 场景的小对象权重:多数请求落在 16B..4KB,少量到 14KB。
   大对象用 1/64 的独立分支选择,避免权重表被三项大数主导。 */
static const struct {
    uint16_t size;
    uint8_t  weight;
} g_server_small[] = {
    {   16,  8 }, {   32, 12 }, {   48, 10 }, {   64, 14 },
    {   96,  9 }, {  128, 12 }, {  192,  8 }, {  256, 10 },
    {  384,  6 }, {  512,  8 }, {  768,  5 }, { 1024,  7 },
    { 1536,  4 }, { 2048,  5 }, { 3072,  3 }, { 4096,  4 },
    { 5120,  2 }, { 8192,  2 }, {12288,  1 }, {14336,  1 },
};
#define SERVER_SMALL_WEIGHT 131u

/* 小对象尺寸取自尺寸类边界附近与边界之间:既覆盖精确落类,也覆盖
   向上取整,避免只测到整齐尺寸的理想路径 */
static const size_t g_small_sizes[] = {
    8, 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768,
    1024, 2048, 4096, 8192, 14336
};

/* 从大到小覆盖页级与小 extent 级:1MB 在 glibc 通常走 mmap,在
   wmm 走大对象 extent,两种策略的差异会直接反映在 p99 与 RSS 上 */
static const size_t g_large_sizes[] = {
    16u << 10, 64u << 10, 256u << 10, 1u << 20
};

typedef struct {
    const char *impl;         /* "wmm" 或 "glibc",只用于输出 */
    const char *scenario;     /* seq/mt/mixed/large/server/all */
    unsigned    threads;      /* mt/server 场景的线程数 */
    uint64_t    ops;          /* seq/mt/mixed 的分配器调用总数 */
    uint64_t    large_ops;    /* large 场景的调用数 */
    uint64_t    requests;     /* server 场景的请求总数 */
    uint64_t    seed;         /* 全局种子;每个 worker 再派生 */
    bool        header;       /* 是否打印 CSV 表头 */
    bool        srv_big;      /* server 是否包含 >=16KB 大对象 */
    bool        srv_realloc;  /* server 是否执行 realloc */
    bool        srv_session;  /* server 是否使用会话槽位 */
    bool        srv_touch;    /* server 是否触碰已分配内存 */
} bench_options_t;

typedef struct {
    uint64_t calls;           /* 完成的分配器调用数 */
    uint64_t reqs;            /* 计时单位数:调用场景=calls,server=请求数 */
    double   seconds;         /* 计时窗口的墙钟秒数 */
    uint64_t p50_ns;          /* 计时单位延迟中位数 */
    uint64_t p99_ns;          /* 99% 分位 */
    uint64_t p999_ns;         /* 99.9% 分位 */
    uint64_t p9999_ns;        /* 99.99% 分位 */
    uint64_t max_ns;          /* 最慢一次 */
    uint64_t rss_kb;          /* 元数据释放后的进程 RSS */
} bench_result_t;

typedef struct {
    void  *p;
    size_t size;
} server_obj_t;

/* 最慢请求的构成,用于把尾延迟归因到具体路径;标志位见 SRV_* */
#define SRV_REALLOC  0x1u
#define SRV_GROW     0x2u
#define SRV_SESSION  0x4u
#define SRV_EVICT    0x8u
#define SRV_FLUSH    0x10u
#define SRV_ALLOCFAIL 0x20u

typedef struct {
    uint64_t ns;              /* 请求耗时 */
    uint32_t n;               /* 请求内对象数 */
    uint32_t max_size;        /* 最大单次请求尺寸 */
    uint32_t big;             /* >=16KB 对象数 */
    uint32_t calls;           /* 分配器调用数 */
    uint32_t flags;           /* SRV_* 组合 */
} server_slow_t;

typedef struct {
    size_t        per;                    /* 本线程请求数 */
    uint64_t      seed;
    uint64_t     *lat;                    /* per 个请求延迟样本 */
    atomic_int   *ready;
    atomic_int   *go;
    uint64_t      calls;                  /* 计时窗口内的分配器调用数 */
    uint64_t      req_count;              /* 已执行请求数,用于周期清会话 */
    const bench_options_t *opt;
    server_slow_t slow;                   /* 计时窗口内最慢的一次请求 */
    server_obj_t  req[SERVER_REQ_SLOTS];
    server_obj_t  sess[SERVER_SESS_SLOTS];
} server_worker_t;

typedef struct {
    size_t      per;          /* 本线程负责的调用数 */
    uint64_t    seed;         /* 本线程独立随机流,避免线程间同步 */
    void      **slots;        /* MT_SLOTS 个槽位;NULL 表示空槽 */
    uint64_t   *lat;          /* per 个延迟样本,落进共享大数组 */
    atomic_int *ready;        /* 预热完成计数 */
    atomic_int *go;           /* 同时起跑闸门 */
} mt_worker_t;

static uint64_t now_ns(void) {
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *bench_map(size_t bytes) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static void bench_unmap(void *p, size_t bytes) {
    if (p != NULL)
        (void)munmap(p, bytes);
}

static inline uint64_t rng_next(uint64_t *state) {
    uint64_t x = *state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static uint64_t make_seed(uint64_t base, unsigned id) {
    uint64_t s = base ^ ((uint64_t)(id + 1) * 0x9E3779B97F4A7C15ull);
    return s != 0 ? s : 0xD1B54A32D192ED03ull;
}

/* 小对象触碰首尾各一字节:保证页真的驻留,分配器不能只记账不落页;
   写动作算在分配延迟里,与真实首次使用一致 */
static void touch_small(void *p, size_t size) {
    volatile unsigned char *b = (volatile unsigned char *)p;

    if (p == NULL || size == 0)
        return;
    b[0] = 0xA5;
    b[size - 1] = 0x5A;
}

/* 大对象每页触碰一次:不触碰的话,内核只在页表里记账,RSS 指标
   就失去意义 */
static void touch_large(void *p, size_t size) {
    volatile unsigned char *b = (volatile unsigned char *)p;

    if (p == NULL || size == 0)
        return;
    for (size_t off = 0; off < size; off += g_page)
        b[off] = 0x5A;
    b[size - 1] = 0xA5;
}

static uint64_t rss_kb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (f == NULL)
        return 0;

    unsigned long total = 0, resident = 0;
    int rc = fscanf(f, "%lu %lu", &total, &resident);
    (void)fclose(f);
    if (rc != 2)
        return 0;

    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0)
        return 0;
    return (uint64_t)resident * (uint64_t)pg / 1024u;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;

    return (x > y) - (x < y);
}

/* 最近秩百分位:排序后取 n-1 的 p% 位置。样本量是调用数,不是
   时间片,这样 p99 表示"99% 的分配器调用不慢于这个值" */
static uint64_t percentile(const uint64_t *sorted, size_t n, double p) {
    if (n == 0)
        return 0;
    size_t idx = (size_t)((double)(n - 1) * p / 100.0 + 0.5);
    if (idx >= n)
        idx = n - 1;
    return sorted[idx];
}

/* 收尾统一在这里做:排序、取百分位、释放延迟数组、读 RSS。顺序
   很重要——先 unmap 延迟数组再读 RSS,否则基准自己的延迟数组会被
   算进两个分配器的 RSS,淹没真正想看的差异 */
static bench_result_t finish_result(uint64_t *lat, size_t n,
                                    uint64_t calls, uint64_t reqs,
                                    uint64_t t0, uint64_t t1) {
    bench_result_t r;

    qsort(lat, n, sizeof(*lat), cmp_u64);
    r.calls    = calls;
    r.reqs     = reqs;
    r.seconds  = (double)(t1 - t0) / 1e9;
    if (!(r.seconds > 0.0))
        r.seconds = 1e-9;
    r.p50_ns   = percentile(lat, n, 50.0);
    r.p99_ns   = percentile(lat, n, 99.0);
    r.p999_ns  = percentile(lat, n, 99.9);
    r.p9999_ns = percentile(lat, n, 99.99);
    r.max_ns   = n != 0 ? lat[n - 1] : 0;
    bench_unmap(lat, n * sizeof(*lat));
    r.rss_kb   = rss_kb();
    return r;
}

static void report_row(const bench_result_t *r) {
    double ops_ps = (double)r->calls / r->seconds;
    double req_ps = (double)r->reqs / r->seconds;
    uint64_t fails = atomic_load_explicit(&g_alloc_fail,
                                          memory_order_relaxed);

    printf("%s,%s,%" PRIu64 ",%.6f,%.0f,%" PRIu64 ",%" PRIu64 ",%" PRIu64
           ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.0f,%" PRIu64
           "\n",
           g_impl, g_bench_scenario, r->calls, r->seconds, ops_ps,
           r->p50_ns, r->p99_ns, r->rss_kb, r->p999_ns, r->p9999_ns,
           r->max_ns, r->reqs, req_ps, fails);
    fflush(stdout);
}

static bool run_seq(const bench_options_t *o) {
    /* 一次 op = 一次 alloc 或一次 free,所以配对循环一次产生两个
       样本;奇数请求向下取整到偶数 */
    size_t calls = (size_t)(o->ops & ~(uint64_t)1);
    if (calls < 2)
        calls = 2;

    uint64_t *lat = bench_map(calls * sizeof(*lat));
    if (lat == NULL) {
        fprintf(stderr, "bench: 延迟数组 mmap 失败: %s\n", strerror(errno));
        return false;
    }

    for (int i = 0; i < 128; i++) {
        void *p = g_alloc(64);
        if (p != NULL) {
            touch_small(p, 64);
            g_free(p);
        }
    }

    uint64_t t0 = now_ns();
    for (size_t i = 0; i < calls / 2; i++) {
        uint64_t a = now_ns();
        void *p = g_alloc(64);
        if (p != NULL)
            touch_small(p, 64);
        else
            atomic_fetch_add_explicit(&g_alloc_fail, 1, memory_order_relaxed);
        uint64_t b = now_ns();
        lat[2 * i] = b - a;

        a = now_ns();
        g_free(p);
        b = now_ns();
        lat[2 * i + 1] = b - a;
    }
    uint64_t t1 = now_ns();

    bench_result_t r = finish_result(lat, calls, calls, calls, t0, t1);
    g_bench_scenario = "seq";
    report_row(&r);
    return true;
}

static bool run_mixed(const bench_options_t *o) {
    size_t calls = (size_t)o->ops;
    if (calls == 0)
        calls = 1;

    uint64_t *lat = bench_map(calls * sizeof(*lat));
    void **slots = bench_map(SINGLE_SLOTS * sizeof(*slots));
    if (lat == NULL || slots == NULL) {
        fprintf(stderr, "bench: 工作区 mmap 失败: %s\n", strerror(errno));
        bench_unmap(lat, calls * sizeof(*lat));
        bench_unmap(slots, SINGLE_SLOTS * sizeof(*slots));
        return false;
    }

    uint64_t rng = make_seed(o->seed, 0);
    const size_t nsizes = sizeof(g_small_sizes) / sizeof(g_small_sizes[0]);

    for (int i = 0; i < 256; i++) {
        void *p = g_alloc(64);
        if (p != NULL) {
            touch_small(p, 64);
            g_free(p);
        }
    }

    uint64_t t0 = now_ns();
    for (size_t i = 0; i < calls; i++) {
        unsigned s = (unsigned)(rng_next(&rng) % SINGLE_SLOTS);
        uint64_t a = now_ns();

        if (slots[s] == NULL) {
            size_t sz = g_small_sizes[rng_next(&rng) % nsizes];
            void *p = g_alloc(sz);
            if (p != NULL)
                touch_small(p, sz);
            else
                atomic_fetch_add_explicit(&g_alloc_fail, 1,
                                          memory_order_relaxed);
            slots[s] = p;
        } else {
            g_free(slots[s]);
            slots[s] = NULL;
        }
        lat[i] = now_ns() - a;
    }
    uint64_t t1 = now_ns();

    for (unsigned s = 0; s < SINGLE_SLOTS; s++)
        if (slots[s] != NULL)
            g_free(slots[s]);
    bench_unmap(slots, SINGLE_SLOTS * sizeof(*slots));

    bench_result_t r = finish_result(lat, calls, calls, calls, t0, t1);
    g_bench_scenario = "mixed";
    report_row(&r);
    return true;
}

static bool run_large(const bench_options_t *o) {
    size_t calls = (size_t)o->large_ops;
    if (calls == 0)
        calls = 1;

    uint64_t *lat = bench_map(calls * sizeof(*lat));
    void **slots = bench_map(LARGE_SLOTS * sizeof(*slots));
    if (lat == NULL || slots == NULL) {
        fprintf(stderr, "bench: 工作区 mmap 失败: %s\n", strerror(errno));
        bench_unmap(lat, calls * sizeof(*lat));
        bench_unmap(slots, LARGE_SLOTS * sizeof(*slots));
        return false;
    }

    uint64_t rng = make_seed(o->seed, 1);
    const size_t nsizes = sizeof(g_large_sizes) / sizeof(g_large_sizes[0]);

    for (int i = 0; i < 8; i++) {
        void *p = g_alloc(64u << 10);
        if (p != NULL) {
            touch_large(p, 64u << 10);
            g_free(p);
        }
    }

    uint64_t t0 = now_ns();
    for (size_t i = 0; i < calls; i++) {
        unsigned s = (unsigned)(rng_next(&rng) % LARGE_SLOTS);
        uint64_t a = now_ns();

        if (slots[s] == NULL) {
            size_t sz = g_large_sizes[rng_next(&rng) % nsizes];
            void *p = g_alloc(sz);
            if (p != NULL)
                touch_large(p, sz);
            else
                atomic_fetch_add_explicit(&g_alloc_fail, 1,
                                          memory_order_relaxed);
            slots[s] = p;
        } else {
            g_free(slots[s]);
            slots[s] = NULL;
        }
        lat[i] = now_ns() - a;
    }
    uint64_t t1 = now_ns();

    for (unsigned s = 0; s < LARGE_SLOTS; s++)
        if (slots[s] != NULL)
            g_free(slots[s]);
    bench_unmap(slots, LARGE_SLOTS * sizeof(*slots));

    bench_result_t r = finish_result(lat, calls, calls, calls, t0, t1);
    g_bench_scenario = "large";
    report_row(&r);
    return true;
}

static void *mt_worker(void *arg) {
    mt_worker_t *w = arg;
    const size_t nsizes = sizeof(g_small_sizes) / sizeof(g_small_sizes[0]);

    /* 预热在线程缓存建立之后、计时开始之前:否则第一个线程的
       tcache 初始化会被算进吞吐,而 glibc 的 arena 初始化同理,
       两边都不该把"第一次"混进稳态数据 */
    for (int i = 0; i < 256; i++) {
        void *p = g_alloc(64);
        if (p != NULL) {
            touch_small(p, 64);
            g_free(p);
        }
    }

    atomic_fetch_add_explicit(w->ready, 1, memory_order_release);
    while (!atomic_load_explicit(w->go, memory_order_acquire))
        sched_yield();

    uint64_t rng = w->seed;
    for (size_t i = 0; i < w->per; i++) {
        unsigned s = (unsigned)(rng_next(&rng) % MT_SLOTS);
        uint64_t a = now_ns();

        if (w->slots[s] == NULL) {
            size_t sz = g_small_sizes[rng_next(&rng) % nsizes];
            void *p = g_alloc(sz);
            if (p != NULL)
                touch_small(p, sz);
            else
                atomic_fetch_add_explicit(&g_alloc_fail, 1,
                                          memory_order_relaxed);
            w->slots[s] = p;
        } else {
            g_free(w->slots[s]);
            w->slots[s] = NULL;
        }
        w->lat[i] = now_ns() - a;
    }

    for (unsigned s = 0; s < MT_SLOTS; s++)
        if (w->slots[s] != NULL)
            g_free(w->slots[s]);
    return NULL;
}

static bool run_mt(const bench_options_t *o) {
    if (o->threads == 0)
        return false;

    size_t per = (size_t)(o->ops / o->threads);
    if (per == 0)
        per = 1;
    size_t calls = per * o->threads;

    uint64_t *lat = bench_map(calls * sizeof(*lat));
    mt_worker_t *workers = bench_map(o->threads * sizeof(*workers));
    pthread_t *tids = bench_map(o->threads * sizeof(*tids));
    if (lat == NULL || workers == NULL || tids == NULL) {
        fprintf(stderr, "bench: 多线程工作区 mmap 失败: %s\n",
                strerror(errno));
        bench_unmap(lat, calls * sizeof(*lat));
        bench_unmap(workers, o->threads * sizeof(*workers));
        bench_unmap(tids, o->threads * sizeof(*tids));
        return false;
    }

    atomic_int ready = 0;
    atomic_int go = 0;
    unsigned created = 0;

    for (unsigned i = 0; i < o->threads; i++) {
        workers[i].per   = per;
        workers[i].seed  = make_seed(o->seed, i);
        workers[i].lat   = lat + (size_t)i * per;
        workers[i].ready = &ready;
        workers[i].go    = &go;
        workers[i].slots = bench_map(MT_SLOTS * sizeof(void *));
        if (workers[i].slots == NULL) {
            fprintf(stderr, "bench: 槽位表 mmap 失败: %s\n", strerror(errno));
            for (unsigned k = 0; k < i; k++)
                bench_unmap(workers[k].slots, MT_SLOTS * sizeof(void *));
            bench_unmap(lat, calls * sizeof(*lat));
            bench_unmap(workers, o->threads * sizeof(*workers));
            bench_unmap(tids, o->threads * sizeof(*tids));
            return false;
        }

        int rc = pthread_create(&tids[i], NULL, mt_worker, &workers[i]);
        if (rc != 0) {
            fprintf(stderr, "bench: pthread_create: %s\n", strerror(rc));
            /* 已创建的线程正等 go;放行并收尸,避免把失败变成挂死 */
            atomic_store(&go, 1);
            for (unsigned k = 0; k < created; k++)
                (void)pthread_join(tids[k], NULL);
            for (unsigned k = 0; k <= i; k++)
                bench_unmap(workers[k].slots, MT_SLOTS * sizeof(void *));
            bench_unmap(lat, calls * sizeof(*lat));
            bench_unmap(workers, o->threads * sizeof(*workers));
            bench_unmap(tids, o->threads * sizeof(*tids));
            return false;
        }
        created++;
    }

    while (atomic_load_explicit(&ready, memory_order_acquire) <
           (int)o->threads)
        sched_yield();

    uint64_t t0 = now_ns();
    atomic_store_explicit(&go, 1, memory_order_release);
    for (unsigned i = 0; i < o->threads; i++)
        (void)pthread_join(tids[i], NULL);
    uint64_t t1 = now_ns();

    for (unsigned i = 0; i < o->threads; i++)
        bench_unmap(workers[i].slots, MT_SLOTS * sizeof(void *));
    bench_unmap(workers, o->threads * sizeof(*workers));
    bench_unmap(tids, o->threads * sizeof(*tids));

    bench_result_t r = finish_result(lat, calls, calls, calls, t0, t1);
    g_bench_scenario = "mt";
    report_row(&r);
    return true;
}

/* 1/64 走 16KB/64KB/256KB 大对象,其余按权重表落在 16B..14KB。
   关闭大对象时仍消耗同一次抽取,改按小对象表选取,保证不同开关组合
   使用同一条随机流、工作负载可直接对照。 */
static size_t server_pick_size(const bench_options_t *o, uint64_t *rng) {
    uint64_t r = rng_next(rng);

    if (o->srv_big && (r & 63u) == 0) {
        static const size_t big[] = { 16u << 10, 64u << 10, 256u << 10 };
        return big[(size_t)((r >> 8) % 3)];
    }

    unsigned pick = (unsigned)((r >> 8) % SERVER_SMALL_WEIGHT);
    size_t   n    = sizeof(g_server_small) / sizeof(g_server_small[0]);
    for (size_t i = 0; i < n; i++) {
        if (pick < g_server_small[i].weight)
            return g_server_small[i].size;
        pick -= g_server_small[i].weight;
    }
    return g_server_small[n - 1].size;
}

static void server_touch(void *p, size_t size) {
    if (size >= (16u << 10))
        touch_large(p, size);
    else
        touch_small(p, size);
}

/* 一次请求:取 1..6 个对象,1/8 概率 realloc,1/8 概率把一个对象
   移到会话槽位,剩余释放;每 64 次请求清空会话槽位。计时包含请求
   内全部 alloc/realloc/free 与会话驱逐/周期清理。各开关关闭时仍按
   原顺序抽取随机数,只跳过对应动作,便于不同变体逐项对照。 */
static void server_request(server_worker_t *w, uint64_t *rng,
                           uint64_t *lat, size_t idx) {
    const bench_options_t *o = w->opt;
    uint64_t a = now_ns();
    unsigned n = 1u + (unsigned)(rng_next(rng) % 6u);
    unsigned calls = 0, big = 0, max_size = 0, flags = 0;

    for (unsigned i = 0; i < SERVER_REQ_SLOTS; i++)
        w->req[i].p = NULL;

    for (unsigned i = 0; i < n; i++) {
        size_t sz = server_pick_size(o, rng);
        void  *p  = g_alloc(sz);
        calls++;
        w->calls++;
        if (sz > max_size)
            max_size = (unsigned)sz;
        if (sz >= (16u << 10))
            big++;
        if (p == NULL) {
            flags |= SRV_ALLOCFAIL;
            atomic_fetch_add_explicit(&g_alloc_fail, 1,
                                      memory_order_relaxed);
            w->req[i].size = 0;
            continue;
        }
        if (o->srv_touch)
            server_touch(p, sz);
        w->req[i].p    = p;
        w->req[i].size = sz;
    }

    uint64_t r_re = rng_next(rng);
    unsigned i_re = 0, dir_re = 0;
    if ((r_re & 7u) == 0) {
        i_re = (unsigned)(rng_next(rng) % n);
        if (w->req[i_re].p != NULL)
            dir_re = (unsigned)(rng_next(rng) & 1u);
    }
    if (o->srv_realloc && (r_re & 7u) == 0 && w->req[i_re].p != NULL) {
        size_t old = w->req[i_re].size;
        size_t sz;

        if (dir_re != 0)
            sz = old > 64 ? old - old / 4 : old + 8;
        else
            sz = old + old / 4 + 8;

        if (sz > max_size)
            max_size = (unsigned)sz;

        void *q = g_realloc(w->req[i_re].p, sz);
        calls++;
        w->calls++;
        flags |= dir_re == 0 ? (SRV_REALLOC | SRV_GROW) : SRV_REALLOC;
        if (q == NULL) {
            flags |= SRV_ALLOCFAIL;
            atomic_fetch_add_explicit(&g_alloc_fail, 1,
                                      memory_order_relaxed);
        } else {
            if (o->srv_touch)
                server_touch(q, sz);
            w->req[i_re].p    = q;
            w->req[i_re].size = sz;
        }
    }

    uint64_t r_se = rng_next(rng);
    unsigned i_se = 0, s_se = 0;
    if ((r_se & 7u) == 0) {
        i_se = (unsigned)(rng_next(rng) % n);
        if (w->req[i_se].p != NULL)
            s_se = (unsigned)(rng_next(rng) % SERVER_SESS_SLOTS);
    }
    if (o->srv_session && (r_se & 7u) == 0 && w->req[i_se].p != NULL) {
        flags |= SRV_SESSION;
        if (w->sess[s_se].p != NULL) {
            g_free(w->sess[s_se].p);
            calls++;
            w->calls++;
            flags |= SRV_EVICT;
        }
        w->sess[s_se] = w->req[i_se];
        w->req[i_se].p = NULL;
        w->req[i_se].size = 0;
    }

    for (unsigned i = 0; i < n; i++) {
        if (w->req[i].p != NULL) {
            g_free(w->req[i].p);
            calls++;
            w->calls++;
            w->req[i].p = NULL;
        }
    }

    w->req_count++;
    if (w->req_count % SERVER_FLUSH_EVERY == 0) {
        for (unsigned s = 0; s < SERVER_SESS_SLOTS; s++) {
            if (w->sess[s].p != NULL) {
                g_free(w->sess[s].p);
                calls++;
                w->calls++;
                flags |= SRV_FLUSH;
                w->sess[s].p = NULL;
            }
        }
    }

    if (lat != NULL) {
        uint64_t dt = now_ns() - a;

        lat[idx] = dt;
        if (dt > w->slow.ns) {
            w->slow.ns       = dt;
            w->slow.n        = n;
            w->slow.max_size = max_size;
            w->slow.big      = big;
            w->slow.calls    = calls;
            w->slow.flags    = flags;
        }
    }
}

static void *server_worker(void *arg) {
    server_worker_t *w = arg;
    uint64_t rng = w->seed;

    for (int i = 0; i < SERVER_WARMUP; i++)
        server_request(w, &rng, NULL, 0);

    w->calls     = 0;
    w->req_count = 0;

    atomic_fetch_add_explicit(w->ready, 1, memory_order_release);
    while (!atomic_load_explicit(w->go, memory_order_acquire))
        sched_yield();

    for (size_t i = 0; i < w->per; i++)
        server_request(w, &rng, w->lat, i);
    return NULL;
}

/* 会话对象在计时结束后由主线程统一释放:它们不计入 ops,也不会
   把 worker 退出后留下的指针带进 RSS 统计。 */
static void server_free_state(server_worker_t *w) {
    for (unsigned s = 0; s < SERVER_SESS_SLOTS; s++) {
        if (w->sess[s].p != NULL) {
            g_free(w->sess[s].p);
            w->sess[s].p = NULL;
        }
    }
    for (unsigned i = 0; i < SERVER_REQ_SLOTS; i++) {
        if (w->req[i].p != NULL) {
            g_free(w->req[i].p);
            w->req[i].p = NULL;
        }
    }
}

static bool run_server(const bench_options_t *o) {
    if (o->threads == 0)
        return false;

    size_t per = (size_t)(o->requests / o->threads);
    if (per == 0)
        per = 1;
    size_t nreq = per * o->threads;

    uint64_t *lat = bench_map(nreq * sizeof(*lat));
    server_worker_t *workers = bench_map(o->threads * sizeof(*workers));
    pthread_t *tids = bench_map(o->threads * sizeof(*tids));
    if (lat == NULL || workers == NULL || tids == NULL) {
        fprintf(stderr, "bench: server 工作区 mmap 失败: %s\n",
                strerror(errno));
        bench_unmap(lat, nreq * sizeof(*lat));
        bench_unmap(workers, o->threads * sizeof(*workers));
        bench_unmap(tids, o->threads * sizeof(*tids));
        return false;
    }

    atomic_int ready = 0;
    atomic_int go = 0;
    unsigned created = 0;

    fprintf(stderr, "bench: server config big=%u realloc=%u session=%u "
            "touch=%u threads=%u\n", o->srv_big, o->srv_realloc,
            o->srv_session, o->srv_touch, o->threads);

    for (unsigned i = 0; i < o->threads; i++) {
        workers[i].opt   = o;
        workers[i].per   = per;
        workers[i].seed  = make_seed(o->seed, 64 + i);
        workers[i].lat   = lat + (size_t)i * per;
        workers[i].ready = &ready;
        workers[i].go    = &go;

        int rc = pthread_create(&tids[i], NULL, server_worker, &workers[i]);
        if (rc != 0) {
            fprintf(stderr, "bench: pthread_create: %s\n", strerror(rc));
            atomic_store(&go, 1);
            for (unsigned k = 0; k < created; k++)
                (void)pthread_join(tids[k], NULL);
            for (unsigned k = 0; k < o->threads; k++)
                server_free_state(&workers[k]);
            bench_unmap(lat, nreq * sizeof(*lat));
            bench_unmap(workers, o->threads * sizeof(*workers));
            bench_unmap(tids, o->threads * sizeof(*tids));
            return false;
        }
        created++;
    }

    while (atomic_load_explicit(&ready, memory_order_acquire) <
           (int)o->threads)
        sched_yield();

    uint64_t t0 = now_ns();
    atomic_store_explicit(&go, 1, memory_order_release);
    for (unsigned i = 0; i < o->threads; i++)
        (void)pthread_join(tids[i], NULL);
    uint64_t t1 = now_ns();

    uint64_t calls = 0;
    for (unsigned i = 0; i < o->threads; i++) {
        const server_slow_t *s = &workers[i].slow;

        calls += workers[i].calls;
        fprintf(stderr, "bench: server slowest t%u %" PRIu64 "ns n=%u "
                "max=%u big=%u calls=%u flags=0x%x\n", i, s->ns, s->n,
                s->max_size, s->big, s->calls, s->flags);
        server_free_state(&workers[i]);
    }

    bench_unmap(workers, o->threads * sizeof(*workers));
    bench_unmap(tids, o->threads * sizeof(*tids));

    bench_result_t r = finish_result(lat, nreq, calls, nreq, t0, t1);
    g_bench_scenario = "server";
    report_row(&r);
    return true;
}

static void usage(FILE *out) {
    fprintf(out,
        "用法: bench <wmm|glibc> [选项]\n"
        "  --scenario NAME   seq|mt|mixed|large|server|all(默认 all)\n"
        "  --threads N       mt/server 场景的线程数,1..64(默认 4)\n"
        "  --ops N           seq/mt/mixed 的分配器调用总数(默认 1000000)\n"
        "  --large-ops N     large 场景的调用数(默认 5000)\n"
        "  --requests N      server 场景的请求总数(默认 200000)\n"
        "  --server-big 0|1  server 是否含 >=16KB 大对象(默认 1)\n"
        "  --server-realloc 0|1  server 是否执行 realloc(默认 1)\n"
        "  --server-session 0|1  server 是否使用会话槽位(默认 1)\n"
        "  --server-touch 0|1    server 是否触碰已分配内存(默认 1)\n"
        "  --seed N          随机种子,必须非 0\n"
        "  --no-header       不打印 CSV 表头\n"
        "  --help            显示本帮助\n");
}

static int parse_u64(const char *s, uint64_t lo, uint64_t hi, uint64_t *out) {
    char *end = NULL;

    if (s == NULL || *s == '\0' || *s == '-')
        return -1;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi)
        return -1;
    *out = (uint64_t)v;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(stdout);
        return 0;
    }

    bench_options_t opt = {
        .impl      = argv[1],
        .scenario  = "all",
        .threads   = 4,
        .ops       = 1000000,
        .large_ops = 5000,
        .requests  = 200000,
        .seed      = 0x9E3779B97F4A7C15ull,
        .header    = true,
        .srv_big     = true,
        .srv_realloc = true,
        .srv_session = true,
        .srv_touch   = true,
    };

    if (strcmp(opt.impl, "wmm") != 0 && strcmp(opt.impl, "glibc") != 0) {
        fprintf(stderr, "bench: 未知实现 '%s'(应为 wmm 或 glibc)\n",
                opt.impl);
        usage(stderr);
        return 2;
    }

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--scenario") == 0 && i + 1 < argc) {
            opt.scenario = argv[++i];
            if (strcmp(opt.scenario, "seq") != 0 &&
                strcmp(opt.scenario, "mt") != 0 &&
                strcmp(opt.scenario, "mixed") != 0 &&
                strcmp(opt.scenario, "large") != 0 &&
                strcmp(opt.scenario, "server") != 0 &&
                strcmp(opt.scenario, "all") != 0) {
                fprintf(stderr, "bench: 未知场景 '%s'\n", opt.scenario);
                return 2;
            }
        } else if (strcmp(a, "--threads") == 0 && i + 1 < argc) {
            uint64_t v;
            if (parse_u64(argv[++i], 1, 64, &v) != 0)
                goto bad_arg;
            opt.threads = (unsigned)v;
        } else if (strcmp(a, "--ops") == 0 && i + 1 < argc) {
            if (parse_u64(argv[++i], 100, 20000000, &opt.ops) != 0)
                goto bad_arg;
        } else if (strcmp(a, "--large-ops") == 0 && i + 1 < argc) {
            if (parse_u64(argv[++i], 8, 5000000, &opt.large_ops) != 0)
                goto bad_arg;
        } else if (strcmp(a, "--requests") == 0 && i + 1 < argc) {
            if (parse_u64(argv[++i], 64, 20000000, &opt.requests) != 0)
                goto bad_arg;
        } else if (strcmp(a, "--server-big") == 0 && i + 1 < argc) {
            uint64_t v;
            if (parse_u64(argv[++i], 0, 1, &v) != 0)
                goto bad_arg;
            opt.srv_big = v != 0;
        } else if (strcmp(a, "--server-realloc") == 0 && i + 1 < argc) {
            uint64_t v;
            if (parse_u64(argv[++i], 0, 1, &v) != 0)
                goto bad_arg;
            opt.srv_realloc = v != 0;
        } else if (strcmp(a, "--server-session") == 0 && i + 1 < argc) {
            uint64_t v;
            if (parse_u64(argv[++i], 0, 1, &v) != 0)
                goto bad_arg;
            opt.srv_session = v != 0;
        } else if (strcmp(a, "--server-touch") == 0 && i + 1 < argc) {
            uint64_t v;
            if (parse_u64(argv[++i], 0, 1, &v) != 0)
                goto bad_arg;
            opt.srv_touch = v != 0;
        } else if (strcmp(a, "--seed") == 0 && i + 1 < argc) {
            if (parse_u64(argv[++i], 1, UINT64_MAX, &opt.seed) != 0)
                goto bad_arg;
        } else if (strcmp(a, "--no-header") == 0) {
            opt.header = false;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            goto bad_arg;
        }
        continue;
bad_arg:
        fprintf(stderr, "bench: 参数无效或缺少取值: %s\n", a);
        usage(stderr);
        return 2;
    }

    long pg = sysconf(_SC_PAGESIZE);
    if (pg > 0)
        g_page = (size_t)pg;

    if (strcmp(opt.impl, "wmm") == 0) {
        wmm_boot();
        g_alloc   = wmm_alloc;
        g_free    = wmm_free;
        g_realloc = wmm_realloc;
    } else {
        g_alloc   = malloc;
        g_free    = free;
        g_realloc = realloc;
    }
    g_impl = opt.impl;

    if (opt.header)
        printf("impl,scenario,ops,seconds,ops_per_sec,p50_ns,p99_ns,rss_kb,"
               "p999_ns,p9999_ns,max_ns,reqs,req_per_sec,alloc_fail\n");

    bool ok = true;
    bool all = strcmp(opt.scenario, "all") == 0;

    if (ok && (all || strcmp(opt.scenario, "seq") == 0))
        ok = run_seq(&opt);
    if (ok && (all || strcmp(opt.scenario, "mt") == 0))
        ok = run_mt(&opt);
    if (ok && (all || strcmp(opt.scenario, "mixed") == 0))
        ok = run_mixed(&opt);
    if (ok && (all || strcmp(opt.scenario, "large") == 0))
        ok = run_large(&opt);
    if (ok && (all || strcmp(opt.scenario, "server") == 0))
        ok = run_server(&opt);

    uint64_t fails = atomic_load_explicit(&g_alloc_fail, memory_order_relaxed);
    if (fails != 0)
        fprintf(stderr, "bench: %" PRIu64 " 次分配失败,吞吐与延迟仅覆盖"
                "成功的调用\n", fails);

    return ok ? 0 : 1;
}
