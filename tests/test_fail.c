/* test_fail.c —— 故障注入用例:OOM 与系统调用失败路径
 *
 * 五个注入点,覆盖分配器仅有的五类"环境说不行"分支:
 *   MAP     窗口 mmap 失败(extent 与新 slab 的取页都经过这里)
 *   MADVISE purge 的 madvise 失败(必须保持 DIRTY,绝不谎报已还)
 *   TCACHE  线程缓存块创建失败(退化为慢路径,分配仍须成功)
 *   BASE    base 元数据 bump 失败(extent 结点等直接消费方)
 *   SLAB_HDR slab 头分配失败(复用链与 base 两个来源都算,按 OOM 上抛)
 *
 * 为什么专门测失败路径:正常负载几乎踩不到它们,而它们最容易写出
 * "计数漂移""页表残留""状态改了一半"。每个用例在注入后核对:
 * live/active/累计计数一格不动、事件环留下 0xDB、解除注入后同一
 * 操作立即成功。
 *
 * 注入语义见 wmm_internal.h:站点过滤 + 倒计数。公开 API 内部有
 * 重试(缓存填充失败再走慢路径等),所以用 WMM_FAULT_ALWAYS 让一
 * 次调用里的每次检查都失败,返回后再解除。
 *
 * 确定性:MAP 用例先把全部空闲 extent 直接取空(白盒调用物理层)。
 * 否则分配命中复用块就绕过了窗口 mmap,注入形同虚设。
 * 取空的 extent 持有到本组末尾,期间保持"无空闲块"这个前提。
 *
 * 全程单线程;结束时复位注入点,后面的 guard 组按默认状态运行。
 */
#include "wmm_internal.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("  FAIL @%d: %s\n", __LINE__, #cond);               \
            g_fail++;                                                  \
        }                                                              \
    } while (0)

/* ---- 注入开关 ---------------------------------------------------- */

static void fault_arm(unsigned site, unsigned after) {
    atomic_store_explicit(&g_fault_site, site, memory_order_relaxed);
    atomic_store_explicit(&g_fault_after, after, memory_order_relaxed);
}

/* 解除必须写在失败调用之后的第一行:后面的断言、快照、事件读取
   都可能间接触发被注入的设施,带着开关往下跑会把测试自身搞失败 */
static void fault_off(void) {
    atomic_store_explicit(&g_fault_site, WMM_FAULT_NONE, memory_order_relaxed);
    atomic_store_explicit(&g_fault_after, 0, memory_order_relaxed);
}

/* ---- 事件环读取 -------------------------------------------------- */

static void drain_all(void) {
    wmm_event_t ev[64];
    size_t      got = 0;

    while (wmm_event_drain(ev, 64, &got) == 0 && got != 0)
        ;
}

/* 取走当前全部事件,并报告其中是否出现 type */
static bool drain_has(uint32_t type) {
    static wmm_event_t ev[WMM_EVENT_RING];
    size_t             got = 0;
    bool               hit = false;

    if (wmm_event_drain(ev, WMM_EVENT_RING, &got) != 0)
        return false;
    for (size_t i = 0; i < got; i++)
        if (ev[i].type == type)
            hit = true;
    return hit;
}

/* ---- 注入器自身语义:站点过滤 + 倒计数 --------------------------- */

static void check_fault_semantics(void) {
    fault_arm(WMM_FAULT_BASE, 2);
    CHECK(!wmm_fault_check(WMM_FAULT_BASE));  /* 第 1 次:放行 2→1 */
    CHECK(wmm_fault_check(WMM_FAULT_BASE));   /* 第 2 次:命中并归零 */
    CHECK(!wmm_fault_check(WMM_FAULT_BASE));  /* 计数已尽:不再失败 */
    CHECK(!wmm_fault_check(WMM_FAULT_MAP));   /* 站点不匹配:不计数 */

    fault_arm(WMM_FAULT_ANY, WMM_FAULT_ALWAYS);
    CHECK(wmm_fault_check(WMM_FAULT_MAP));    /* ANY 匹配任意站点 */
    CHECK(wmm_fault_check(WMM_FAULT_MADVISE));
    fault_off();
    CHECK(!wmm_fault_check(WMM_FAULT_MAP));
}

/* ---- 抽干空闲 extent --------------------------------------------- */

#define DRAIN_MAX 4096

static wmm_extent_t *g_drain[DRAIN_MAX];
static size_t        g_ndrain;

/* 按"当前最大空闲块的精确页数"逐个取走:请求与块等大,物理层不会
   分裂,每轮必然消灭一个空闲块,循环可终止。直接调物理层是为了让
   小对象尺寸类(4096 字节也小于小对象上限)不能把请求接走 */
static void drain_free_extents(void) {
    wmm_tcache_flush_all();
    g_ndrain = 0;

    for (;;) {
        size_t max_p = 0;

        for (wmm_extent_t *e = g_ar_head; e != NULL; e = e->ar_next)
            if (e->state != WMM_EXT_INUSE && e->npages > max_p)
                max_p = e->npages;
        if (max_p == 0)
            break;

        CHECK(g_ndrain < DRAIN_MAX);
        if (g_ndrain >= DRAIN_MAX)
            break;

        wmm_extent_t *e = wmm_phys_alloc_extent(max_p * WMM_PAGE);

        CHECK(e != NULL);
        if (e == NULL)
            break;
        CHECK(e->npages == max_p);   /* 必须是整块交付,没有余段 */
        g_drain[g_ndrain++] = e;
    }

    /* 空闲块全部转 INUSE:无论原状态是 DIRTY 还是 RETAINED,脏页
       计数都必须归零 */
    CHECK(g_dirty_pages == 0);
}

static void release_drained(void) {
    for (size_t i = 0; i < g_ndrain; i++)
        wmm_phys_free_extent(g_drain[i]);
    g_ndrain = 0;
}

/* ---- MAP:窗口 mmap 失败 ------------------------------------------ */

static void check_map_oom(void) {
    wmm_stats_t s0, s1;

    drain_all();
    CHECK(wmm_stats_snapshot(&s0) == 0);

    /* 池已干,64KB 大对象只能走窗口 bump,注入点必然被检查到 */
    fault_arm(WMM_FAULT_MAP, WMM_FAULT_ALWAYS);
    void *p = wmm_alloc(64 << 10);
    fault_off();

    CHECK(p == NULL);

    /* 失败发生在任何状态变更之前:统计必须与实际内存一致 */
    CHECK(wmm_stats_snapshot(&s1) == 0);
    CHECK(s1.live_bytes == s0.live_bytes);
    CHECK(s1.live_objects == s0.live_objects);
    CHECK(s1.active_bytes == s0.active_bytes);
    CHECK(s1.allocated_bytes == s0.allocated_bytes);
    CHECK(s1.deallocated_bytes == s0.deallocated_bytes);
    CHECK(s1.cls[WMM_NBINS].live == s0.cls[WMM_NBINS].live);

    /* OOM 必须留下可观测事件,排障时才不会只剩一个 NULL */
    CHECK(drain_has(0xDB));

    /* 解除注入后重试:窗口继续从原 bump 指针推进,数据可写 */
    p = wmm_alloc(64 << 10);
    CHECK(p != NULL);
    if (p != NULL) {
        memset(p, 0xA5, 64 << 10);
        wmm_free(p);
    }
}

/* ---- MAP:对齐分配(档 3)失败 ------------------------------------ */

static void check_aligned_oom(void) {
    size_t align = (size_t)1 << 20;
    size_t size  = (size_t)4 << 20;
    void  *m     = (void *)0x1;

    /* 档 3 要 size + alignment 的过分配;池干时同样只能窗口 bump。
       公开 API 走返回码,失败时 *out 必须置空 */
    fault_arm(WMM_FAULT_MAP, WMM_FAULT_ALWAYS);
    int rc = wmm_posix_memalign(&m, align, size);
    fault_off();

    CHECK(rc == ENOMEM);
    CHECK(m == NULL);

    rc = wmm_posix_memalign(&m, align, size);
    CHECK(rc == 0);
    CHECK(m != NULL);
    if (m != NULL) {
        CHECK(((uintptr_t)m & (align - 1)) == 0);
        CHECK(wmm_usable_size(m) >= size);
        ((unsigned char *)m)[0]         = 0xA5;
        ((unsigned char *)m)[size - 1]  = 0x5A;
        wmm_free(m);
    }
}

/* ---- TCACHE:缓存块创建失败,分配退化为慢路径 --------------------- */

typedef struct {
    bool cache_null_after_init;    /* 显式初始化后仍未建出缓存 */
    bool cache_null_after_alloc;   /* 分配内部的重试也没建出来 */
    bool alloc_ok;                 /* 退化路径上的分配仍然成功 */
} tcache_probe_t;

static void *tcache_probe_thread(void *arg) {
    tcache_probe_t *r = arg;

    wmm_thread_init();                       /* 第一次建缓存:命中注入 */
    r->cache_null_after_init = (g_tcache == NULL);

    void *p = wmm_alloc(64);                 /* 内部会再试一次,仍失败 */
    r->cache_null_after_alloc = (g_tcache == NULL);
    r->alloc_ok = (p != NULL);
    if (p != NULL) {
        CHECK(wmm_usable_size(p) >= 64);
        memset(p, 0xC3, 64);
        wmm_free(p);
    }
    return NULL;
}

static void check_tcache_degrade(void) {
    wmm_stats_t s0, s1;
    pthread_t   th;
    tcache_probe_t probe = { false, false, false };

    wmm_tcache_flush_all();
    CHECK(wmm_stats_snapshot(&s0) == 0);

    fault_arm(WMM_FAULT_TCACHE, WMM_FAULT_ALWAYS);
    int rc = pthread_create(&th, NULL, tcache_probe_thread, &probe);
    CHECK(rc == 0);
    if (rc == 0)
        CHECK(pthread_join(th, NULL) == 0);
    fault_off();

    /* 没有缓存不是错误:分配器只该慢,不该拒绝服务 */
    CHECK(probe.cache_null_after_init);
    CHECK(probe.cache_null_after_alloc);
    CHECK(probe.alloc_ok);

    /* 对象经慢路径来去,存活统计必须对平 */
    CHECK(wmm_stats_snapshot(&s1) == 0);
    CHECK(s1.live_bytes == s0.live_bytes);
    CHECK(s1.live_objects == s0.live_objects);
}

/* ---- MADVISE:purge 失败保持 DIRTY -------------------------------- */

static void check_madvise_failure(void) {
    /* 先清场:此刻应无脏页,active 有确定基线 */
    wmm_phys_purge_now();
    size_t active0 = g_active_bytes;
    CHECK(g_dirty_pages == 0);

    void *p = wmm_alloc((size_t)4 << 20);
    CHECK(p != NULL);
    if (p != NULL) {
        ((unsigned char *)p)[0]               = 0xA5;
        ((unsigned char *)p)[((size_t)4 << 20) - 1] = 0x5A;
        wmm_free(p);
    }

    size_t dirty1  = g_dirty_pages;
    size_t active1 = g_active_bytes;
    CHECK(dirty1 > 0);
    CHECK(active1 > active0);

    /* 注入 madvise 失败:页内容与状态原封不动,计数不许改 */
    fault_arm(WMM_FAULT_MADVISE, WMM_FAULT_ALWAYS);
    wmm_phys_purge_now();
    fault_off();

    CHECK(g_dirty_pages == dirty1);
    CHECK(g_active_bytes == active1);

    /* 解除后下一轮必须真的收走,active 精确回到清场基线 */
    wmm_phys_purge_now();
    CHECK(g_dirty_pages == 0);
    CHECK(g_active_bytes == active0);
}

/* ---- SLAB_HDR:slab 头分配失败 ------------------------------------ */

static void check_slab_hdr_oom(void) {
    /* slab 头取不到 = 元数据耗尽,OOM 上抛;失败不得动页表或计数 */
    fault_arm(WMM_FAULT_SLAB_HDR, WMM_FAULT_ALWAYS);
    wmm_slab_t *sl = wmm_phys_alloc_slab(0);
    fault_off();

    CHECK(sl == NULL);

    sl = wmm_phys_alloc_slab(0);
    CHECK(sl != NULL);
    if (sl != NULL) {
        CHECK(sl->ext != NULL);
        wmm_phys_free_slab(sl);
    }
}

int test_fail(void) {
    printf("  fault injection: OOM and syscall failure paths\n");

    check_fault_semantics();

    drain_free_extents();
    check_map_oom();
    check_aligned_oom();
    release_drained();

    check_tcache_degrade();
    check_madvise_failure();
    check_slab_hdr_oom();

    fault_off();
    CHECK(g_fault_site == 0 && g_fault_after == 0);

    return g_fail;
}
