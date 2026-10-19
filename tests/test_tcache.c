/* test_tcache.c —— M8/M9 线程缓存用例(白盒:直接读缓存水位与注册表)
 *
 * 覆盖(tasks.md M8/M9 自测清单 + 快路径的边界行为):
 *   TC1 命中率:单线程对同一尺寸连续分配 10 万次,命中缓存的比例
 *         ≥ 90%。命中判据是白盒的——每次分配前后比较目标 bin 的
 *         head:水位恰好降一格 = 弹栈命中;空栈后先涨到 nfill-1 =
 *         填充事件。只看"分配成功"区分不了快慢路径:慢路径也能
 *         成功,只有水位轨迹能证明对象确实来自缓存。
 *   TC2 容量与满栈 GC:任何时刻 head ≤ ncached_max、low_water ≤ head
 *         恒成立;满栈释放先跑 GC(留 retain = 用量×1.25 或种子 2),
 *         热 bin 退回 3/4 部分刷出兜底——落点按 GC 公式重放精确断言。
 *   TC3 偶尔用一次的类:填充后晾着不动,显式 GC 把驻留压回 retain
 *         (used==0 → 2 个种子;used>0 → used×1.25),head ≤ retain。
 *   容量公式:每类 ncached_max == clamp(2*nregs, 20, 200),nfill 落
 *         在自适应范围 [2, ncm/2](创建态 = ncm/2,由 lifecycle 的新
 *         线程块白盒报告);栈数组紧贴结构体、按类顺序排布,各段
 *         长度之和与 map_size 精确相等。
 *   线程注册表与退出:工作线程首次分配时建缓存并挂链;pthread 析构
 *         在线程返回时把缓存刷回 bin、摘链、munmap——缓存对象数、
 *         全局 live、注册表节点数三项都回到进场基线。
 *   G5 千次线程增删:1000 个线程反复创建/退出,Σ live、缓存总量、
 *         注册表节点全部回到基线,RSS 无量级漂移(粗检,精确账在
 *         stress 窗口预算)。
 *   关闭开关:enabled=false 时分配/释放直走 bin,缓存水位纹丝不动。
 *   跨线程释放:从未分配过的线程 free 别的线程的对象,走慢路径直接
 *         归还,不为自己建缓存,也不碰原线程的栈。
 *   fork 单线程:子进程注册表只剩调用线程;缓存水位与 live 原样
 *         保留;子进程分配仍命中缓存。
 *   fork 幽灵缓存:另一线程死前留在缓存里的对象,子进程侧被钩子刷
 *         回 bin(而不是永久泄漏);注册表只剩调用线程,live 恰好
 *         少掉"死线程缓存"这一块——死线程仍被应用持有的指针随
 *         线程消失,fork 语义使然,不在此列。
 *
 * 每个用例自清自净:进场先 wmm_tcache_flush_all 把主线程缓存剥空,
 * 出场再刷一次并对回进场 live,组与组之间互不继承缓存状态。
 * 需要确定性的组(TC2/TC3)先把 gc_evt/gc_next 归零,关掉事件轮转
 * 的噪声,让 GC 只由本用例显式触发。
 */
#include "wmm_internal.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <sched.h>
#include <unistd.h>
#include <sys/wait.h>

static int g_fail;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("  FAIL @%d: %s\n", __LINE__, #cond);               \
            g_fail++;                                                  \
        }                                                              \
    } while (0)

static uint64_t total_live(void) {
    uint64_t sum = 0;
    for (int i = 0; i < WMM_NBINS; i++)
        sum += g_bins[i].live;
    return sum + g_large.live;
}

/* 注册表节点数。锁保护链的增删;调用时工作线程要么已停稳要么已
   加入完毕,读数不是"快照近似",就是整数 */
static int tc_nodes(void) {
    int n = 0;

    pthread_mutex_lock(&g_threads_lock);
    for (wmm_tcache_t *t = g_threads; t != NULL; t = t->next)
        n++;
    pthread_mutex_unlock(&g_threads_lock);
    return n;
}

/* 剥空本线程缓存后取 live 基线:缓存的存量对象不该混进任何
   "回到基线"的等式中 */
static uint64_t flush_baseline(void) {
    wmm_tcache_flush_all();
    return total_live();
}

/* ---- 容量公式与整块布局 ---- */
static void check_capacity_formula(void) {
    wmm_tcache_t *t = g_tcache;
    size_t total = sizeof(wmm_tcache_t);
    size_t off   = 0;

    CHECK(t != NULL);
    if (t == NULL)
        return;

    CHECK(t->enabled);
    CHECK(t->map_size >= sizeof(wmm_tcache_t));

    /* 栈区从结构体之后开始,按类号顺序无缝排布。map_size 是整块
       映射的长度而不是任一数组的长度——线程退出按它 munmap,这里
       逐字节对账,算错一格就是静默截断或越界释放 */
    char *base = (char *)(t + 1);
    for (szind_t i = 0; i < WMM_NBINS; i++) {
        wmm_cache_bin_t *cb = &t->bins[i];
        unsigned want = 2u * wmm_bin_info[i].nregs;
        if (want < WMM_TCACHE_NCACHED_MIN)
            want = WMM_TCACHE_NCACHED_MIN;
        if (want > WMM_TCACHE_NCACHED_MAX)
            want = WMM_TCACHE_NCACHED_MAX;

        CHECK(cb->ncached_max == want);
        /* M9 起 nfill 自适应:创建态是 ncm/2,GC 后会在 [2, ncm/2]
           里漂(封顶容量一半、下限 2)。主线程的块还被 stress 等前序
           用例磨过,这里断言的是自适应范围;创建公式由 lifecycle 的
           新线程块白盒报告覆盖 */
        CHECK(cb->nfill >= 2 && cb->nfill <= cb->ncached_max / 2);
        CHECK(cb->head <= cb->ncached_max);
        CHECK(cb->low_water <= cb->head);
        CHECK(cb->stack == (void **)(base + off));

        size_t seg = (size_t)cb->ncached_max * sizeof(void *);
        CHECK(seg <= t->map_size - sizeof(wmm_tcache_t) - off);
        off += seg;
        total += seg;
    }
    CHECK(total == t->map_size);

    /* 大对象槽(下标 36)预留未启用 */
    CHECK(t->bins[WMM_NBINS].stack == NULL);
    CHECK(t->bins[WMM_NBINS].ncached_max == 0);
    CHECK(t->bins[WMM_NBINS].head == 0);
    CHECK(t->bins[WMM_NBINS].nfill == 0);
}

/* ---- TC1:命中率 ----
 * 工作负载:始终保持一个对象在应用手里,分配新的、释放旧的。
 * 这正是一般代码 churn 小对象的形态,也把快路径的两个方向
 * (弹栈、压栈)都压进同一条循环。稳态下除第一次分配外每次
 * 都该命中;命中率门槛按清单取 90%,留足余量给实现演化。 */
static void check_hit_rate(void) {
    enum { NALLOC = 100000, BIN_SIZE = 64 };
    uint64_t base = flush_baseline();
    szind_t  bin  = wmm_size2index(BIN_SIZE);
    wmm_cache_bin_t *cb = &g_tcache->bins[bin];
    unsigned nfill = cb->nfill;
    unsigned hits  = 0;
    void    *prev  = NULL;

    CHECK(cb->head == 0);
    CHECK(nfill > 0);

    for (int k = 0; k < NALLOC; k++) {
        unsigned before = cb->head;
        void    *p      = wmm_alloc(BIN_SIZE);
        CHECK(p != NULL);

        if (before > 0 && (unsigned)cb->head + 1 == before) {
            hits++;                       /* 水位恰好降一格:弹栈命中 */
        } else {
            /* 唯一的非命中形态:空栈触发填充,填充后立刻弹掉一个。
               别的水位变化都说明快路径做了预期之外的事 */
            CHECK(before == 0);
            CHECK((unsigned)cb->head + 1 == nfill);
        }
        if ((k & 1023) == 0)
            CHECK(wmm_usable_size(p) == BIN_SIZE);

        if (prev != NULL)
            wmm_free(prev);
        prev = p;
    }
    wmm_free(prev);

    CHECK(hits * 100u >= (unsigned)NALLOC * 90u);   /* TC1 */
    CHECK(cb->head <= cb->ncached_max);

    wmm_tcache_flush_all();
    CHECK(total_live() == base);
}

/* ---- TC2:容量上限与满栈 GC ----
 * 先攒满一批对象再逐个释放:缓冲会反复涨到 ncm,触发 M9 的满栈
 * GC(先按 retain 公式刷、热 bin 退回 3/4 兜底)再压一个。每步
 * 核对水位;满栈事件的落点由测试自己按 GC 公式重放,精确断言——
 * GC 是 M9 新增的行为,公式若改,这里同步改,落点仍然钉得住。
 *
 * 重放要点:lw_tracked 镜像真实的 low_water。真实 low_water 只在
 * 两处变:弹栈时取 min(快路径,释放阶段没有弹栈),刷出时重置为
 * 刷后 head(tcache_flush_bins 与 GC 都这么做)。释放阶段只有刷出
 * 会动它,所以 lw_tracked 在满栈事件之间恒等于真实值。
 * 事件轮转先归零:轮转 GC 落在别的 bin(下一个目标固定为 0),不
 * 掺和本用例的确定性。 */
static void check_head_capacity(void) {
    enum { NOBJ = 300, BIN_SIZE = 64 };
    uint64_t base = flush_baseline();
    szind_t  bin  = wmm_size2index(BIN_SIZE);
    wmm_cache_bin_t *cb = &g_tcache->bins[bin];
    unsigned ncm       = cb->ncached_max;
    unsigned lw_tracked = 0;   /* 重放用的 low_water 镜像 */
    unsigned nflush    = 0;
    static void *o[NOBJ];

    CHECK(cb->head == 0);
    g_tcache->gc_evt  = 0;
    g_tcache->gc_next = 0;

    for (int k = 0; k < NOBJ; k++) {
        o[k] = wmm_alloc(BIN_SIZE);
        CHECK(o[k] != NULL);
        CHECK(cb->head <= ncm);           /* TC2:分配侧不越顶 */
        CHECK(cb->low_water <= cb->head);
    }
    /* 分配阶段把缓存吃空过多次,low_water 钉在 0——重放从这起算 */

    for (int k = 0; k < NOBJ; k++) {
        unsigned before = cb->head;

        wmm_free(o[k]);
        CHECK(cb->head <= ncm);           /* TC2:释放侧不越顶 */
        CHECK(cb->low_water <= cb->head);

        if (before == ncm) {
            /* 满栈:GC 按 retain 公式刷出;用量顶到容量时 GC 全留,
               退回 3/4 部分刷出兜底,落点 = target + 1 */
            unsigned used   = ncm - lw_tracked;
            unsigned retain = (used == 0) ? 2 : used + (used >> 2);
            if (retain > ncm)
                retain = ncm;
            unsigned target = (retain == ncm) ? (ncm / 4) : retain;
            CHECK(cb->head == target + 1);
            lw_tracked = target;          /* 刷出把水位重置到 target */
            nflush++;
        } else {
            CHECK(cb->head == before + 1);
            if (lw_tracked > cb->head)    /* 释放阶段 head 只涨,不触发 */
                lw_tracked = cb->head;
        }
    }
    /* NOBJ 大于任何一类的容量,满栈事件必然发生 */
    CHECK(nflush >= 1);

    wmm_tcache_flush_all();
    CHECK(total_live() == base);
}

/* ---- TC3:偶尔用一次的类,GC 压回 retain ----
 * 白盒构造:冷 bin 先填充一批(填充把 low_water 抬到新水位),然后
 * 晾着——不再弹栈,GC 看到的 used==0,retain 缩到种子 2,整批压回;
 * 再构造"用过一点"的场景(弹掉一半再放回),used>0 时 retain =
 * used×1.25,刷出的落点不多不少。事件轮转归零,GC 只由本用例触发。 */
static void check_gc_occasional(void) {
    enum { BIN_SIZE = 4096 };              /* nregs=1, ncm=20 的最小类 */
    uint64_t base = flush_baseline();
    szind_t  bin  = wmm_size2index(BIN_SIZE);
    wmm_cache_bin_t *cb = &g_tcache->bins[bin];
    void    *o[WMM_TCACHE_NCACHED_MAX];

    CHECK(cb->head == 0);
    g_tcache->gc_evt  = 0;
    g_tcache->gc_next = 0;

    /* 场景 A:完全没用过——填充后原样晾着 */
    unsigned nf = cb->nfill;
    wmm_tcache_fill(bin, nf);              /* head = nf, low_water = nf */
    CHECK(cb->head == nf);
    wmm_tcache_gc(bin);
    CHECK(cb->head == 2);                  /* used==0 → 留 2 个种子 */
    CHECK(cb->head <= 2);                  /* TC3:head ≤ retain */

    /* 场景 B:用过一点。弹栈把 low_water 压到 nf2−half,放回不碰它,
       GC 看到 used == half,retain = half×1.25,刷到刚好 retain */
    wmm_tcache_fill(bin, cb->nfill);
    unsigned nf2  = cb->head;
    unsigned half = nf2 / 2;
    CHECK(half > 0);
    for (unsigned k = 0; k < half; k++)
        o[k] = wmm_tcache_alloc(bin);
    for (unsigned k = 0; k < half; k++)
        wmm_tcache_free(bin, o[k]);
    wmm_tcache_gc(bin);
    unsigned used   = nf2 - (nf2 - half);  /* = half */
    unsigned retain = used + (used >> 2);
    CHECK(cb->head == retain);             /* 刷到 retain,不多不少 */
    CHECK(cb->head <= retain);             /* TC3 */

    wmm_tcache_flush_all();
    CHECK(total_live() == base);
}

/* ---- 线程注册表与线程退出 ----
 * 工作线程:nfill 次分配恰好用光一个填充批次(水位回到 0),
 * 再释放一半——这半批对象停在它的缓存里。主线程在它停稳后
 * 检查注册表与缓存总量,然后放行;线程返回触发 pthread 析构,
 * 刷空、摘链、munmap 一次做完。
 * M9 起批次大小取工作线程自己块的 nfill:主线程的 nfill 被前序
 * 用例的 GC 磨过,不再等于新块的创建值。worker 白盒报告新块的
 * ncm/nfill——创建公式 nfill==ncm/2 只对新建块成立,据此断言。 */
typedef struct {
    szind_t    bin;
    unsigned   ncm_fresh;    /* 新块创建态:容量(经 ready 发布) */
    unsigned   nfill;        /* 新块的 nfill,即批次大小 */
    unsigned   half;
    atomic_int ready;        /* 工作线程:状态已发布 */
    atomic_int go;           /* 主线程:可以继续收了 */
    unsigned   err;          /* 工作线程写的错误位,经 ready 发布 */
    unsigned   head_alloc;   /* nfill 次分配后的水位 */
    unsigned   head_half;    /* 再释放 half 个后的水位 */
} reg_arg_t;

static void *reg_worker(void *arg) {
    reg_arg_t *a = (reg_arg_t *)arg;
    void     *o[WMM_TCACHE_NCACHED_MAX];
    unsigned  err = 0;

    wmm_thread_init();
    if (g_tcache == NULL) {
        err |= 1u;
    } else {
        a->ncm_fresh = g_tcache->bins[a->bin].ncached_max;
        a->nfill     = g_tcache->bins[a->bin].nfill;
        a->half      = a->nfill / 2;

        for (unsigned k = 0; k < a->nfill; k++) {
            o[k] = wmm_alloc(64);
            if (o[k] == NULL)
                err |= 2u;
        }
        a->head_alloc = g_tcache->bins[a->bin].head;

        for (unsigned k = 0; k < a->half; k++)
            wmm_free(o[k]);
        a->head_half = g_tcache->bins[a->bin].head;
    }

    a->err = err;
    atomic_store_explicit(&a->ready, 1, memory_order_release);
    while (!atomic_load_explicit(&a->go, memory_order_acquire))
        sched_yield();

    for (unsigned k = a->half; k < a->nfill; k++)
        wmm_free(o[k]);
    return NULL;
}

static void check_thread_lifecycle(void) {
    uint64_t base   = flush_baseline();
    int      nodes0 = tc_nodes();
    szind_t  bin    = wmm_size2index(64);
    reg_arg_t a;

    a.bin        = bin;
    a.ncm_fresh  = 0;
    a.nfill      = 0;
    a.half       = 0;
    a.err        = 0;
    a.head_alloc = 0;
    a.head_half  = 0;
    atomic_init(&a.ready, 0);
    atomic_init(&a.go, 0);

    pthread_t th;
    if (pthread_create(&th, NULL, reg_worker, &a) != 0) {
        printf("  FAIL @%d: pthread_create\n", __LINE__);
        g_fail++;
        return;
    }
    while (!atomic_load_explicit(&a.ready, memory_order_acquire))
        sched_yield();

    CHECK(a.err == 0);
    CHECK(a.nfill >= 2);                  /* 新块批次至少 2,才有半批 */
    CHECK(a.nfill == a.ncm_fresh / 2);    /* 创建公式:填充目标 = 一半 */
    CHECK(a.head_alloc == 0);             /* 一个填充批次正好被 nfill
                                             次分配用光,不多不少 */
    CHECK(a.head_half == a.half);         /* 释放的半批全压在缓存栈上 */
    CHECK(tc_nodes() == nodes0 + 1);      /* 注册表多一个线程节点 */
    CHECK(wmm_tcache_cached_total() == a.half);   /* 主线程已刷空,
                                                     缓存总量就是它的 */

    atomic_store_explicit(&a.go, 1, memory_order_release);
    CHECK(pthread_join(th, NULL) == 0);

    /* 线程返回 = 析构刷空 + 摘链 + munmap;三项都回基线 */
    CHECK(tc_nodes() == nodes0);
    CHECK(wmm_tcache_cached_total() == 0);
    CHECK(total_live() == base);
}

/* ---- RSS 粗检读数:statm 第二个字段是物理驻留页数 ---- */
static size_t rss_kb(void) {
    long total = 0, resident = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f == NULL)
        return 0;
    if (fscanf(f, "%ld %ld", &total, &resident) != 2)
        resident = 0;
    fclose(f);
    return (size_t)resident * (sysconf(_SC_PAGESIZE) >> 10);
}

/* ---- G5:千次线程创建/退出 ----
 * 每个线程分配一小批、释放一半,另一半留给 pthread 析构刷回——
 * 缓存块随线程退出整块 munmap。千次下来,live / 缓存总量 / 注册表
 * 节点必须全部回到基线,一个块都不许漏。RSS 只做粗检:线程栈与
 * ASan 记账有各自开销(glibc 还留一个小栈缓存),量级级漂移才叫
 * 失败;精确的内存账在 stress 的窗口预算里。 */
typedef struct {
    unsigned err;
} churn_arg_t;

static void *churn_worker(void *arg) {
    churn_arg_t *a = (churn_arg_t *)arg;
    void *a64[8], *b4k[4];
    unsigned err = 0;

    wmm_thread_init();
    if (g_tcache == NULL)
        err |= 1u;
    for (int k = 0; k < 8; k++) {
        a64[k] = wmm_alloc(64);
        if (a64[k] == NULL)
            err |= 2u;
    }
    for (int k = 0; k < 4; k++) {
        b4k[k] = wmm_alloc(4096);
        if (b4k[k] == NULL)
            err |= 4u;
    }
    /* 全部归还:对象要么回到缓存栈(填充余量 + 刚释放的),要么直接
       回 bin;一个都不许留在应用手里——线程退出后没人替它释放,局部
       数组里的指针不会自己消失。缓存里的存量由 pthread 析构统一
       刷空 + munmap,这正是本用例要验证的回收路径 */
    for (int k = 0; k < 8; k++)
        wmm_free(a64[k]);
    for (int k = 0; k < 4; k++)
        wmm_free(b4k[k]);
    a->err = err;
    return NULL;
}

static void check_thread_churn(void) {
    enum { NCHURN = 1000 };
    uint64_t base   = flush_baseline();
    int      nodes0 = tc_nodes();
    size_t   rss0   = rss_kb();
    churn_arg_t a;

    for (int k = 0; k < NCHURN; k++) {
        a.err = 0;
        pthread_t th;
        if (pthread_create(&th, NULL, churn_worker, &a) != 0) {
            printf("  FAIL @%d: pthread_create at %d\n", __LINE__, k);
            g_fail++;
            return;
        }
        CHECK(pthread_join(th, NULL) == 0);
        CHECK(a.err == 0);
    }

    CHECK(tc_nodes() == nodes0);              /* G5:注册表回到基线 */
    CHECK(wmm_tcache_cached_total() == 0);    /* 每块都经析构刷空 */
    CHECK(total_live() == base);              /* Σ live 分毫不差 */

    size_t rss1 = rss_kb();
    CHECK(rss1 <= rss0 + 20480);              /* 粗检:20MB 容差内不漂 */
}

/* ---- 关闭开关:直通慢路径 ---- */
static void check_disabled_path(void) {
    uint64_t base  = flush_baseline();
    szind_t  bin   = wmm_size2index(64);
    wmm_cache_bin_t *cb = &g_tcache->bins[bin];
    bool     saved = g_tcache->enabled;
    uint64_t live0 = total_live();
    uint64_t nd0   = g_bins[bin].ndalloc;
    unsigned head0 = cb->head;

    g_tcache->enabled = false;

    void *p = wmm_alloc(64);
    CHECK(p != NULL);
    CHECK(cb->head == head0);             /* 水位纹丝不动 */
    CHECK(total_live() == live0 + 1);

    wmm_free(p);
    CHECK(cb->head == head0);
    CHECK(total_live() == live0);
    CHECK(g_bins[bin].ndalloc == nd0 + 1);   /* 这次释放确由 bin 记账 */

    g_tcache->enabled = saved;
    CHECK(total_live() == base);
}

/* ---- 跨线程释放:释放者没有缓存 ----
 * 惰性创建只挂在分配路径上:pure-free 的线程不该为一次慢路径
 * 释放付一整块 mmap。同时确认原线程的缓存栈没有被别人动过。 */
typedef struct {
    void *p;
    bool  tcache_before;
    bool  tcache_after;
} xfree_arg_t;

static void *xfree_worker(void *arg) {
    xfree_arg_t *a = (xfree_arg_t *)arg;

    a->tcache_before = (g_tcache != NULL);
    wmm_free(a->p);
    a->tcache_after = (g_tcache != NULL);
    return NULL;
}

static void check_cross_thread_free(void) {
    uint64_t base   = flush_baseline();
    int      nodes0 = tc_nodes();
    szind_t  bin    = wmm_size2index(64);
    wmm_cache_bin_t *cb = &g_tcache->bins[bin];
    uint64_t nd0    = g_bins[bin].ndalloc;

    void *p = wmm_alloc(64);
    CHECK(p != NULL);
    unsigned head_after_alloc = cb->head;
    uint64_t live_after_alloc = total_live();

    xfree_arg_t a;
    a.p            = p;
    a.tcache_before = true;
    a.tcache_after  = true;

    pthread_t th;
    if (pthread_create(&th, NULL, xfree_worker, &a) != 0) {
        printf("  FAIL @%d: pthread_create\n", __LINE__);
        g_fail++;
        return;
    }
    CHECK(pthread_join(th, NULL) == 0);

    CHECK(!a.tcache_before);
    CHECK(!a.tcache_after);                  /* 慢路径不替它建缓存 */
    CHECK(tc_nodes() == nodes0);             /* 注册表无增无减 */
    CHECK(cb->head == head_after_alloc);     /* 原线程栈未被碰 */
    CHECK(g_bins[bin].ndalloc == nd0 + 1);   /* 直接归还 bin */
    CHECK(total_live() == live_after_alloc - 1);

    wmm_tcache_flush_all();
    CHECK(total_live() == base);
}

/* ---- fork 单线程:缓存与注册表原样过继 ---- */
static szind_t  g_fork_bin;
static unsigned g_fork_head;
static uint64_t g_fork_live;

static void check_fork_single_thread(void) {
    uint64_t base  = flush_baseline();
    szind_t  bin   = wmm_size2index(64);
    wmm_cache_bin_t *cb = &g_tcache->bins[bin];
    unsigned nfill = cb->nfill;
    void    *o[WMM_TCACHE_NCACHED_MAX];

    for (unsigned k = 0; k < nfill; k++) {
        o[k] = wmm_alloc(64);
        CHECK(o[k] != NULL);
    }
    for (unsigned k = 0; k < nfill; k++)
        wmm_free(o[k]);              /* 全进缓存:水位回到 nfill */
    CHECK(cb->head == nfill);

    g_fork_bin  = bin;
    g_fork_head = cb->head;
    g_fork_live = total_live();

    pid_t pid = fork();
    if (pid == 0) {
        /* 子进程:只用退出码汇报,不碰 stdio(缓冲会被复制两份) */
        int bad = 0;
        unsigned h0 = g_tcache->bins[g_fork_bin].head;

        if (g_threads == NULL || g_threads != g_tcache)
            bad |= 1;                /* 注册表头必须就是本线程的块 */
        if (g_threads != NULL &&
            (g_threads->prev != NULL || g_threads->next != NULL))
            bad |= 2;                /* 且是唯一节点 */
        if (wmm_tcache_cached_total() != g_fork_head)
            bad |= 4;                /* 缓存内容原样过继 */
        if (total_live() != g_fork_live)
            bad |= 8;                /* 没有对象被凭空归还或吞掉 */

        void *p = wmm_alloc(64);     /* 必须命中:水位恰降一格 */
        if (p == NULL)
            bad |= 16;
        if ((unsigned)g_tcache->bins[g_fork_bin].head + 1 != h0)
            bad |= 32;
        wmm_free(p);

        _exit(bad);
    }

    CHECK(pid > 0);
    if (pid > 0) {
        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    wmm_tcache_flush_all();
    CHECK(total_live() == base);
}

/* ---- fork 幽灵缓存:死线程的缓存由子进程侧归还 ----
 * 工作线程缓一半、持一半后停稳。fork 时子进程只带走调用线程,
 * 工作线程的节点成死节点:它的缓存对象若留在账上,子进程永远
 * 达不到全空退休。钩子把它们刷回 bin 并 munmap 整块,所以子进程
 * 的 live 恰好比 fork 前少掉缓存这一块;被死线程持有的另一半
 * 仍是活指针,按 fork 语义留在账上。 */
typedef struct {
    atomic_int ready;
    atomic_int go;
    unsigned   err;
    unsigned   head_at_ready;
} ghost_arg_t;

static void *ghost_worker(void *arg) {
    ghost_arg_t *a = (ghost_arg_t *)arg;
    szind_t  bin = wmm_size2index(64);
    void    *o[WMM_TCACHE_NCACHED_MAX];
    unsigned err = 0;

    wmm_thread_init();
    if (g_tcache == NULL)
        err |= 1u;

    unsigned n = (g_tcache != NULL) ? g_tcache->bins[bin].nfill : 0;
    for (unsigned k = 0; k < n; k++) {
        o[k] = wmm_alloc(64);
        if (o[k] == NULL)
            err |= 2u;
    }
    unsigned half = n / 2;
    for (unsigned k = 0; k < half; k++)
        wmm_free(o[k]);

    a->head_at_ready = (g_tcache != NULL)
                         ? g_tcache->bins[bin].head : 0;
    a->err = err;
    atomic_store_explicit(&a->ready, 1, memory_order_release);
    while (!atomic_load_explicit(&a->go, memory_order_acquire))
        sched_yield();

    for (unsigned k = half; k < n; k++)
        wmm_free(o[k]);
    return NULL;
}

static uint64_t g_ghost_pre_live;
static uint64_t g_ghost_pre_cached;

static void check_fork_ghost(void) {
    uint64_t base   = flush_baseline();
    int      nodes0 = tc_nodes();
    ghost_arg_t a;

    a.err           = 0;
    a.head_at_ready = 0;
    atomic_init(&a.ready, 0);
    atomic_init(&a.go, 0);

    pthread_t th;
    if (pthread_create(&th, NULL, ghost_worker, &a) != 0) {
        printf("  FAIL @%d: pthread_create\n", __LINE__);
        g_fail++;
        return;
    }
    while (!atomic_load_explicit(&a.ready, memory_order_acquire))
        sched_yield();

    CHECK(a.err == 0);
    CHECK(tc_nodes() == nodes0 + 1);
    CHECK(a.head_at_ready > 0);   /* 幽灵部分必须非空,否则这一组
                                     在子进程里什么也没验证到 */

    g_ghost_pre_live   = total_live();
    g_ghost_pre_cached = wmm_tcache_cached_total();
    CHECK(g_ghost_pre_cached == a.head_at_ready);   /* 主线程缓存已空 */

    pid_t pid = fork();
    if (pid == 0) {
        int bad = 0;

        if (g_threads == NULL || g_threads != g_tcache)
            bad |= 1;                /* 钩子后注册表只留调用线程 */
        if (g_threads != NULL &&
            (g_threads->prev != NULL || g_threads->next != NULL))
            bad |= 2;
        if (wmm_tcache_cached_total() != 0)
            bad |= 4;                /* 幽灵缓存已刷回 bin */
        /* 死线程缓存的那一块回到了 bin:live 恰好少这么多 */
        if (total_live() != g_ghost_pre_live - g_ghost_pre_cached)
            bad |= 8;

        void *p = wmm_alloc(64);     /* 幽灵块已 munmap,分配照常 */
        if (p == NULL)
            bad |= 16;
        wmm_free(p);

        _exit(bad);
    }

    CHECK(pid > 0);
    if (pid > 0) {
        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    /* 父进程侧:工作线程一直活着,放行后释放剩余对象并退出,
       析构刷空缓存,一切回到基线 */
    atomic_store_explicit(&a.go, 1, memory_order_release);
    CHECK(pthread_join(th, NULL) == 0);
    CHECK(tc_nodes() == nodes0);
    CHECK(wmm_tcache_cached_total() == 0);
    CHECK(total_live() == base);
}

int test_tcache(void) {
    g_fail = 0;
    wmm_boot();
    wmm_thread_init();

    /* 幂等性:再调一次不得换块、不得多挂节点 */
    wmm_tcache_t *t0 = g_tcache;
    int nodes0 = tc_nodes();
    wmm_thread_init();
    CHECK(g_tcache == t0);
    CHECK(tc_nodes() == nodes0);

    check_capacity_formula();
    check_hit_rate();
    check_head_capacity();
    check_gc_occasional();
    check_thread_lifecycle();
    check_thread_churn();
    check_disabled_path();
    check_cross_thread_free();
    check_fork_single_thread();
    check_fork_ghost();

    if (g_fail == 0)
        printf("  all tcache checks passed\n");
    return g_fail;
}
