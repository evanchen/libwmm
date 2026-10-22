/* test_tcache.c —— 线程缓存用例(白盒:直接读缓存水位与注册表)
 *
 * 覆盖快路径边界与缓存生命周期:
 *   命中率:单线程对同一尺寸连续分配 10 万次,命中缓存的比例
 *         ≥ 90%。命中判据是白盒的——每次分配前后比较目标 bin 的
 *         head:水位恰好降一格 = 弹栈命中;空栈后先涨到 nfill-1 =
 *         填充事件。只看"分配成功"区分不了快慢路径:慢路径也能
 *         成功,只有水位轨迹能证明对象确实来自缓存。
 *   容量与满栈 GC:任何时刻 head ≤ ncached_max、low_water ≤ head
 *         恒成立;满栈释放先跑 GC(留 retain = 用量×1.25 或种子 2),
 *         热 bin 退回 3/4 部分刷出兜底——落点按 GC 公式重放精确断言。
 *   偶尔用一次的类:填充后晾着不动,显式 GC 把驻留压回 retain
 *         (used==0 → 2 个种子;used>0 → used×1.25),head ≤ retain。
 *   gc 命令:跨线程收缩。调用线程当场收缩到种子;其他线程只
 *         置请求位,由它在下一次 free 入口完成收缩——两段握手分别
 *         验证"请求已置位"与"处理后落点",不靠时序猜测。
 *   容量公式:每类 ncached_max == clamp(2*nregs, 20, 200),nfill 落
 *         在自适应范围 [2, ncm/2](创建态 = ncm/2,由 lifecycle 的新
 *         线程块白盒报告);栈数组紧贴结构体、按类顺序排布,各段
 *         长度之和与 map_size 精确相等。
 *   线程注册表与退出:工作线程首次分配时建缓存并挂链;pthread 析构
 *         在线程返回时把缓存刷回 bin、摘链、munmap——缓存对象数、
 *         全局 live、注册表节点数三项都回到进场基线。
 *   千次线程增删:1000 个线程反复创建/退出,Σ live、缓存总量、
 *         注册表节点全部回到基线,RSS 无量级漂移(粗检,精确账在
 *         stress 窗口预算)。
 *   关闭开关:enabled=false 时分配/释放直走 bin,缓存水位纹丝不动。
 *   大对象缓存:类 = 精确页数,每类 20 槽;默认单块上限 32 KiB(对齐
 *         jemalloc);16K 命中同一块,不同页数不串用;槽满、超单块上限
 *         或超预算回落物理层;flush 与线程退出归还且不重复计 ndalloc;
 *         enabled=false 一并旁路。
 *   大缓存 GC:按上次 GC 以来的取用次数收缩——未取用的类整类归还,
 *         用过的类保留取用量加 25% 的最新块;gc 命令、跨线程 gc_req
 *         是完整收缩,每 256 次 put 的滚动收缩按 128 页限额轮转页数
 *         类,部分归还的类保留取用信号等下一圈。
 *   大缓存旋钮:config.tcache_max 降低立即生效、提高只影响新线程;
 *         config.tcache_budget 只限制放入;非法值一律 EINVAL。
 *   跨线程释放:从未分配过的线程 free 别的线程的对象,走慢路径直接
 *         归还,不为自己建缓存,也不碰原线程的栈。
 *   fork 单线程:子进程注册表只剩调用线程;缓存水位、大缓存与 live
 *         原样保留;子进程分配仍命中缓存。
 *   fork 幽灵缓存:另一线程死前留在缓存里的对象(含大块),子进程侧
 *         被钩子刷回 bin(而不是永久泄漏);注册表只剩调用线程,live
 *         恰好少掉"死线程缓存"这一块——死线程仍被应用持有的指针随
 *         线程消失,fork 语义使然,不在此列。
 *
 * 每个用例自清自净:进场先 wmm_tcache_flush_all 把主线程缓存剥空,
 * 出场再刷一次并对回进场 live,组与组之间互不继承缓存状态。
 * 需要确定性的组(满栈 GC 与偶用类的落点)先把 gc_evt/gc_next 归零,
 * 关掉事件轮转
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

static uint32_t large_slots(const wmm_tcache_t *t) {
    return atomic_load_explicit(&t->nlarge, memory_order_relaxed);
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
        /* nfill 自适应:创建态是 ncm/2,GC 后会在 [2, ncm/2]
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
    /* 大缓存三段数组紧跟栈区:head、count、used,长度都是 maxp+1 */
    uint32_t maxp = t->large_max_pages;
    size_t head_bytes = ((size_t)maxp + 1) * sizeof(wmm_extent_t *);
    size_t u16_bytes  = ((size_t)maxp + 1) * sizeof(uint16_t);

    CHECK(maxp >= WMM_TCACHE_LARGE_MIN_PAGES);
    CHECK(maxp <= WMM_TCACHE_LARGE_MAX_PAGES);
    CHECK(t->large_head == (wmm_extent_t **)(void *)(base + off));
    off += head_bytes;
    CHECK(t->large_count == (uint16_t *)(void *)(base + off));
    off += u16_bytes;
    CHECK(t->large_used == (uint16_t *)(void *)(base + off));
    off += u16_bytes;
    total += head_bytes + 2 * u16_bytes;
    CHECK(total == t->map_size);

    /* 三段计数互相自洽:head 非空 <=> count 非零;Σcount 与 Σpages
       就是跨线程可读的 nlarge 与预算记账 */
    uint64_t cnt = 0;
    uint64_t pages = 0;
    for (uint32_t p = 0; p <= maxp; p++) {
        CHECK((t->large_head[p] != NULL) == (t->large_count[p] != 0));
        cnt += t->large_count[p];
        pages += (uint64_t)p * t->large_count[p];
    }
    CHECK(t->large_pages == pages);
    CHECK(large_slots(t) == cnt);

    /* 大对象槽(下标 36)预留未启用 */
    CHECK(t->bins[WMM_NBINS].stack == NULL);
    CHECK(t->bins[WMM_NBINS].ncached_max == 0);
    CHECK(t->bins[WMM_NBINS].head == 0);
    CHECK(t->bins[WMM_NBINS].nfill == 0);
}

/* ---- 命中率 ----
 * 工作负载:始终保持一个对象在应用手里,分配新的、释放旧的。
 * 这正是一般代码 churn 小对象的形态,也把快路径的两个方向
 * (弹栈、压栈)都压进同一条循环。稳态下除第一次分配外每次
 * 都该命中;门槛取 90%,留足余量给实现演化。 */
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

    CHECK(hits * 100u >= (unsigned)NALLOC * 90u);   /* 命中率下界 */
    CHECK(cb->head <= cb->ncached_max);

    wmm_tcache_flush_all();
    CHECK(total_live() == base);
}

/* ---- 容量上限与满栈 GC ----
 * 先攒满一批对象再逐个释放:缓冲会反复涨到 ncm,触发满栈 GC(先按
 * retain 公式刷、热 bin 退回 3/4 兜底)再压一个。每步核对水位;满栈
 * 事件的落点由测试自己按 GC 公式重放,精确断言——公式若改,这里
 * 同步改,落点仍然钉得住。
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
        CHECK(cb->head <= ncm);           /* 分配侧不越顶 */
        CHECK(cb->low_water <= cb->head);
    }
    /* 分配阶段把缓存吃空过多次,low_water 钉在 0——重放从这起算 */

    for (int k = 0; k < NOBJ; k++) {
        unsigned before = cb->head;

        wmm_free(o[k]);
        CHECK(cb->head <= ncm);           /* 释放侧不越顶 */
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

/* ---- 偶尔用一次的类:GC 压回 retain ----
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
    CHECK(cb->head <= 2);                  /* head ≤ retain */

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
    CHECK(cb->head <= retain);

    wmm_tcache_flush_all();
    CHECK(total_live() == base);
}

/* ---- wmm_ctl("gc"):本线程同步 + 跨线程请求位 ----
 * 本线程部分:直接填满一个 bin 且不弹栈(low_water == head),GC 判
 * 定"完全没用过",应压回 2 个种子——这正是命令"强制收缩"的落点。
 * 跨线程部分:控制线程不能改别人的栈,只能置 gc_req;工作线程在下
 * 一次 free 入口取走请求并收缩。测试分两段握手:置位后先确认请求
 * 位可见,再放行 free,最后核对落点。 */
static void check_ctl_gc_self(void) {
    uint64_t base = flush_baseline();
    szind_t  bin  = wmm_size2index(6144);   /* 本组其他用例未碰过的类 */
    wmm_cache_bin_t *cb = &g_tcache->bins[bin];
    unsigned ncm;

    CHECK(cb->head == 0);
    g_tcache->gc_evt  = 0;   /* 关掉事件轮转的噪声 */
    g_tcache->gc_next = 0;

    /* 填到容量顶,不依赖此前 nfill 被磨到多少 */
    ncm = cb->ncached_max;
    CHECK(ncm > 2);
    wmm_tcache_fill(bin, ncm);
    CHECK(cb->head == ncm);
    CHECK(cb->low_water == cb->head);

    CHECK(wmm_ctl("gc") == 0);
    CHECK(cb->head == 2);   /* used==0 → 留 2 个种子 */

    wmm_tcache_flush_all();
    CHECK(total_live() == base);
}

typedef struct {
    atomic_int ready;          /* 工作线程:块已填好,状态可见 */
    atomic_int go;             /* 主线程:可以触发请求处理 */
    atomic_int done;           /* 工作线程:head_after 已发布 */
    wmm_tcache_t *t;           /* 工作线程的缓存块(退出前有效) */
    void         *p;           /* 一次 free 的触发对象 */
    szind_t       bin;         /* 被填满的目标类 */
    unsigned      head_before;
    unsigned      head_after;
    unsigned      err;
} gc_req_arg_t;

static void *gc_req_worker(void *arg) {
    gc_req_arg_t *a = (gc_req_arg_t *)arg;
    unsigned err = 0;

    wmm_thread_init();
    a->t   = g_tcache;
    a->bin = wmm_size2index(6144);

    if (g_tcache == NULL) {
        err |= 1u;
    } else {
        a->p = wmm_alloc(64);
        if (a->p == NULL)
            err |= 2u;

        /* 填满且期间不弹栈:low_water == head,GC 会判成没动过 */
        wmm_tcache_fill(a->bin, g_tcache->bins[a->bin].ncached_max);
        a->head_before = g_tcache->bins[a->bin].head;
        if (a->head_before <= 2)
            err |= 4u;
    }
    a->err = err;
    atomic_store_explicit(&a->ready, 1, memory_order_release);

    while (!atomic_load_explicit(&a->go, memory_order_acquire))
        sched_yield();

    if (err == 0) {
        /* 任意一次小对象 free 都在缓存入口服务请求位 */
        wmm_free(a->p);
        a->head_after = g_tcache->bins[a->bin].head;
        if (atomic_load_explicit(&g_tcache->gc_req,
                                 memory_order_acquire) != 0)
            a->err |= 8u;      /* 请求应已被 exchange 取走 */
    }
    atomic_store_explicit(&a->done, 1, memory_order_release);
    return NULL;
}

static void check_ctl_gc_cross_thread(void) {
    uint64_t     base = flush_baseline();
    int          nodes0 = tc_nodes();
    gc_req_arg_t a;

    a.t           = NULL;
    a.p           = NULL;
    a.bin         = 0;
    a.head_before = 0;
    a.head_after  = 0;
    a.err         = 0;
    atomic_init(&a.ready, 0);
    atomic_init(&a.go, 0);
    atomic_init(&a.done, 0);

    pthread_t th;
    if (pthread_create(&th, NULL, gc_req_worker, &a) != 0) {
        printf("  FAIL @%d: pthread_create\n", __LINE__);
        g_fail++;
        return;
    }
    while (!atomic_load_explicit(&a.ready, memory_order_acquire))
        sched_yield();

    CHECK(a.err == 0);
    CHECK(a.t != NULL && a.t != g_tcache);
    if (a.t != NULL) {
        CHECK(a.head_before == a.t->bins[a.bin].ncached_max);

        /* 控制线程:自己同步收缩,工作线程只置位。工作线程此刻
           停在 go 上,不会消费请求,所以置位后可以稳定观察 1 */
        CHECK(wmm_ctl("gc") == 0);
        CHECK(atomic_load_explicit(&a.t->gc_req,
                                   memory_order_acquire) == 1);
    }

    atomic_store_explicit(&a.go, 1, memory_order_release);
    while (!atomic_load_explicit(&a.done, memory_order_acquire))
        sched_yield();
    CHECK(pthread_join(th, NULL) == 0);

    CHECK(a.err == 0);
    CHECK(a.head_after == 2);          /* 请求处理后的落点 */
    CHECK(tc_nodes() == nodes0);       /* 线程已退出并摘链 */
    CHECK(wmm_tcache_cached_total() == 0);
    CHECK(total_live() == base);       /* 缓存对象全部归还 */
}

/* ---- 线程注册表与线程退出 ----
 * 工作线程:nfill 次分配恰好用光一个填充批次(水位回到 0),
 * 再释放一半——这半批对象停在它的缓存里。主线程在它停稳后
 * 检查注册表与缓存总量,然后放行;线程返回触发 pthread 析构,
 * 刷空、摘链、munmap 一次做完。
 * 批次大小取工作线程自己块的 nfill:主线程的 nfill 被前序
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

/* ---- 千次线程创建/退出 ----
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

    CHECK(tc_nodes() == nodes0);              /* 注册表回到基线 */
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

/* ---- 整 extent 大对象缓存 ---- */
/* 大缓存清空后的统一断言:三段数组无残留,计数归零,live 回基线 */
static void large_expect_empty(uint64_t base) {
    wmm_tcache_t *t = g_tcache;
    uint64_t cnt = 0;

    CHECK(t != NULL);
    if (t == NULL)
        return;

    for (uint32_t p = WMM_TCACHE_LARGE_MIN_PAGES;
         p <= t->large_max_pages; p++) {
        CHECK(t->large_head[p] == NULL);
        CHECK(t->large_count[p] == 0);
        CHECK(t->large_used[p] == 0);
        cnt += t->large_count[p];
    }
    CHECK(t->large_pages == 0);
    CHECK(large_slots(t) == cnt);
    CHECK(large_slots(t) == 0);
    CHECK(t->large_gc_evt == 0);
    CHECK(t->large_gc_cursor == WMM_TCACHE_LARGE_MIN_PAGES);
    CHECK(total_live() == base);
}

static void large_reset(uint64_t base) {
    wmm_tcache_flush_all();
    large_expect_empty(base);
}

/* 白盒核对每条类链:链长、页数、nlarge 与头指针一致 */
static void large_check_counts(const wmm_tcache_t *t) {
    uint32_t total = 0;
    uint32_t pages = 0;

    for (uint32_t p = WMM_TCACHE_LARGE_MIN_PAGES;
         p <= t->large_max_pages; p++) {
        uint32_t n = 0;

        for (const wmm_extent_t *e = t->large_head[p]; e != NULL;
             e = e->cache_next)
            n++;
        CHECK(n == t->large_count[p]);
        CHECK((n != 0) == (t->large_head[p] != NULL));
        total += n;
        pages += n * p;
    }
    CHECK(large_slots(t) == total);
    CHECK(t->large_pages == pages);
}

/* 用不可缓存的页数块触发一次滚动 GC:先置事件计数再 free */
static void large_trigger_rolling(uint32_t npages) {
    wmm_tcache_t *t = g_tcache;

    t->large_gc_evt = WMM_TCACHE_LARGE_GC_EVERY - 1;
    void *p = wmm_alloc((size_t)npages * WMM_PAGE);
    CHECK(p != NULL);
    wmm_free(p);
    CHECK(t->large_gc_evt == 0);
}

/* 精确页数命中、类独立、每类 20 槽封顶、不同页数不串用 */
static void check_large_basic(uint64_t base) {
    wmm_tcache_t *t = g_tcache;

    void *p = wmm_alloc(16384);
    CHECK(p != NULL);
    CHECK(total_live() == base + 1);
    wmm_free(p);
    CHECK(t->large_head[4] != NULL && t->large_head[4]->addr == p);
    CHECK(t->large_count[4] == 1);
    CHECK(t->large_pages == 4);
    CHECK(large_slots(t) == 1);
    CHECK(wmm_tcache_cached_total() == 1);
    CHECK(total_live() == base + 1);

    void *q = wmm_alloc(16384);
    CHECK(q == p);
    CHECK(wmm_usable_size(q) == 16384);
    CHECK(t->large_head[4] == NULL);
    CHECK(t->large_count[4] == 0);
    CHECK(t->large_pages == 0);
    CHECK(large_slots(t) == 0);
    wmm_free(q);
    CHECK(t->large_count[4] == 1);
    large_reset(base);

    /* 两个页数类互不挤占:默认范围内最大的 8 页类满,不影响 6 页类 */
    void *a[WMM_TCACHE_LARGE_NSLOTS];
    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++) {
        a[k] = wmm_alloc(8 * WMM_PAGE);
        CHECK(a[k] != NULL);
    }
    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++)
        wmm_free(a[k]);
    CHECK(t->large_count[8] == WMM_TCACHE_LARGE_NSLOTS);
    CHECK(t->large_pages == 8 * WMM_TCACHE_LARGE_NSLOTS);

    void *b = wmm_alloc(6 * WMM_PAGE);
    CHECK(b != NULL);
    wmm_free(b);
    CHECK(t->large_count[6] == 1);
    CHECK(t->large_count[8] == WMM_TCACHE_LARGE_NSLOTS);
    CHECK(t->large_pages == 8 * WMM_TCACHE_LARGE_NSLOTS + 6);
    large_reset(base);

    /* 每类 20 槽封顶:第 21 块回落物理层 */
    void *h[WMM_TCACHE_LARGE_NSLOTS + 1];
    for (unsigned k = 0; k <= WMM_TCACHE_LARGE_NSLOTS; k++) {
        h[k] = wmm_alloc(16384);
        CHECK(h[k] != NULL);
    }
    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++)
        wmm_free(h[k]);
    CHECK(t->large_count[4] == WMM_TCACHE_LARGE_NSLOTS);

    uint64_t live_hold = total_live();
    uint64_t nd0 = atomic_load_explicit(&g_large.ndalloc,
                                        memory_order_relaxed);
    wmm_free(h[WMM_TCACHE_LARGE_NSLOTS]);
    CHECK(t->large_count[4] == WMM_TCACHE_LARGE_NSLOTS);
    CHECK(total_live() == live_hold - 1);
    CHECK(atomic_load_explicit(&g_large.ndalloc, memory_order_relaxed) ==
          nd0 + 1);
    large_reset(base);

    /* 精确页数:5 页块不满足 4 页请求,4 页块也不进 5 页类 */
    void *b5 = wmm_alloc(5 * WMM_PAGE);
    CHECK(b5 != NULL);
    wmm_free(b5);
    CHECK(t->large_count[5] == 1);
    CHECK(t->large_head[4] == NULL);

    void *a4 = wmm_alloc(16384);
    CHECK(a4 != NULL && a4 != b5);
    CHECK(wmm_usable_size(a4) == 16384);
    CHECK(t->large_count[5] == 1);
    wmm_free(a4);
    CHECK(t->large_count[4] == 1);      /* 刚释放的 4 页块进 4 页类 */

    void *c5 = wmm_alloc(5 * WMM_PAGE);
    CHECK(c5 == b5);
    CHECK(wmm_usable_size(c5) == 5 * WMM_PAGE);
    wmm_free(c5);
    large_reset(base);
}

/* 单块页数上限、预算与两个 ctl 命令的边界 */
static void check_large_limits(uint64_t base) {
    wmm_tcache_t *t = g_tcache;
    uint32_t max0 = wmm_tcache_max_pages_now();
    uint64_t budget0 = wmm_tcache_budget_pages_now();

    /* 超过创建时数组上界的块永不入缓存 */
    void *over = wmm_alloc(((size_t)max0 + 1) * WMM_PAGE);
    CHECK(over != NULL);
    wmm_free(over);
    CHECK(large_slots(t) == 0);
    CHECK(total_live() == base);

    /* 降低单块上限:已缓存的超限类由 GC 整类归还 */
    void *b5 = wmm_alloc(5 * WMM_PAGE);
    CHECK(b5 != NULL);
    wmm_free(b5);
    CHECK(t->large_count[5] == 1);

    CHECK(wmm_ctl("config.tcache_max", (int)(4 * WMM_PAGE)) == 0);
    CHECK(wmm_tcache_max_pages_now() == 4);
    CHECK(wmm_ctl("gc") == 0);
    CHECK(t->large_count[5] == 0);
    CHECK(large_slots(t) == 0);
    CHECK(total_live() == base);

    /* 上限 4 页:5 页块被拒,4 页块照常 */
    b5 = wmm_alloc(5 * WMM_PAGE);
    CHECK(b5 != NULL);
    uint64_t nd0 = atomic_load_explicit(&g_large.ndalloc,
                                        memory_order_relaxed);
    wmm_free(b5);
    CHECK(t->large_count[5] == 0);
    CHECK(atomic_load_explicit(&g_large.ndalloc, memory_order_relaxed) ==
          nd0 + 1);

    void *p4 = wmm_alloc(4 * WMM_PAGE);
    CHECK(p4 != NULL);
    wmm_free(p4);
    CHECK(t->large_count[4] == 1);
    void *q4 = wmm_alloc(4 * WMM_PAGE);
    CHECK(q4 == p4);
    wmm_free(q4);

    CHECK(wmm_ctl("config.tcache_max", (int)(max0 * WMM_PAGE)) == 0);
    CHECK(wmm_tcache_max_pages_now() == max0);
    large_reset(base);

    /* 预算只限制放入:16 页预算装 4 个 4 页块,第 5 个被拒 */
    CHECK(wmm_ctl("config.tcache_budget", (int)(16 * WMM_PAGE)) == 0);
    CHECK(wmm_tcache_budget_pages_now() == 16);

    void *b[5];
    for (int k = 0; k < 5; k++) {
        b[k] = wmm_alloc(4 * WMM_PAGE);
        CHECK(b[k] != NULL);
    }
    for (int k = 0; k < 4; k++)
        wmm_free(b[k]);
    CHECK(t->large_count[4] == 4);
    CHECK(t->large_pages == 16);

    uint64_t live_hold = total_live();
    nd0 = atomic_load_explicit(&g_large.ndalloc, memory_order_relaxed);
    wmm_free(b[4]);
    CHECK(t->large_count[4] == 4);
    CHECK(t->large_pages == 16);
    CHECK(total_live() == live_hold - 1);
    CHECK(atomic_load_explicit(&g_large.ndalloc, memory_order_relaxed) ==
          nd0 + 1);

    /* 0 = 不限:先把缓存四块全部取走,再放五块回去 */
    CHECK(wmm_ctl("config.tcache_budget", 0) == 0);
    CHECK(wmm_tcache_budget_pages_now() == 0);
    for (int k = 0; k < 5; k++) {
        b[k] = wmm_alloc(4 * WMM_PAGE);
        CHECK(b[k] != NULL);
    }
    CHECK(t->large_count[4] == 0);
    for (int k = 0; k < 5; k++)
        wmm_free(b[k]);
    CHECK(t->large_count[4] == 5);
    CHECK(t->large_pages == 20);

    /* 降低预算不清存量,但新 put 被拒;存量仍可 take */
    CHECK(wmm_ctl("config.tcache_budget", (int)(8 * WMM_PAGE)) == 0);
    void *x = wmm_alloc(4 * WMM_PAGE);
    CHECK(x != NULL);
    CHECK(t->large_count[4] == 4);
    CHECK(t->large_pages == 16);
    wmm_free(x);
    CHECK(t->large_count[4] == 4);
    CHECK(t->large_pages == 16);
    CHECK(total_live() == base + 4);

    /* 非法参数:负值、超硬上限、非零不足一页 */
    CHECK(wmm_ctl("config.tcache_max", -1) == -1);
    CHECK(wmm_ctl("config.tcache_max",
                  (int)((WMM_TCACHE_LARGE_MAX_PAGES + 1u) * WMM_PAGE)) == -1);
    CHECK(wmm_ctl("config.tcache_max", (int)(WMM_PAGE / 2)) == -1);
    CHECK(wmm_ctl("config.tcache_budget", -1) == -1);
    CHECK(wmm_ctl("config.tcache_budget",
                  (int)((WMM_TCACHE_LARGE_MAX_BUDGET_PAGES + 1u) *
                        WMM_PAGE)) == -1);
    CHECK(wmm_ctl("config.tcache_budget", (int)(WMM_PAGE / 2)) == -1);
    CHECK(wmm_ctl("config.tcache_max", 0) == 0);   /* 0 = 关闭,合法 */
    CHECK(wmm_ctl("config.tcache_max", (int)(max0 * WMM_PAGE)) == 0);

    CHECK(wmm_ctl("config.tcache_budget", (int)(budget0 * WMM_PAGE)) == 0);
    CHECK(wmm_tcache_budget_pages_now() == budget0);
    large_reset(base);
}

/* 提高单块上限只影响之后创建的线程缓存 */
typedef struct {
    atomic_int ready;
    atomic_int go;
    unsigned   err;
    uint32_t   maxp;
    uint16_t   cnt4;
    uint16_t   cnt256;
    uint32_t   pages;
    uint32_t   nlarge;
    uint16_t   cnt4_late;
    uint16_t   cnt256_late;
    uint16_t   cursor_late;
    uint32_t   pages_late;
    uint32_t   nlarge_late;
} raise_arg_t;

static void *raise_worker(void *arg) {
    raise_arg_t *a = (raise_arg_t *)arg;
    unsigned err = 0;

    wmm_thread_init();
    if (g_tcache == NULL) {
        a->err = 1u;
        atomic_store_explicit(&a->ready, 1, memory_order_release);
        while (!atomic_load_explicit(&a->go, memory_order_acquire))
            sched_yield();
        return NULL;
    }

    a->maxp = g_tcache->large_max_pages;

    void *big = wmm_alloc((size_t)1 << 20);   /* 恰 256 页 */
    if (big == NULL)
        err |= 2u;
    else
        wmm_free(big);
    void *p = wmm_alloc(16384);
    if (p == NULL)
        err |= 4u;
    else
        wmm_free(p);

    a->cnt4   = g_tcache->large_count[4];
    a->cnt256 = g_tcache->large_count[256];
    a->pages  = g_tcache->large_pages;
    a->nlarge = atomic_load_explicit(&g_tcache->nlarge, memory_order_relaxed);
    if (a->cnt4 != 1 || a->cnt256 != 1)
        err |= 8u;
    if (a->pages != 260 || a->nlarge != 2)
        err |= 16u;

    /* 单块超过滚动页预算也要归还一块,保证 GC 前进 */
    g_tcache->large_gc_cursor = 256;
    g_tcache->large_gc_evt = WMM_TCACHE_LARGE_GC_EVERY - 1;
    void *over = wmm_alloc(2049 * WMM_PAGE);
    if (over == NULL) {
        err |= 32u;
    } else {
        wmm_free(over);
    }
    a->cnt4_late   = g_tcache->large_count[4];
    a->cnt256_late = g_tcache->large_count[256];
    a->cursor_late = g_tcache->large_gc_cursor;
    a->pages_late  = g_tcache->large_pages;
    a->nlarge_late = atomic_load_explicit(&g_tcache->nlarge,
                                          memory_order_relaxed);
    if (a->cnt4_late != 1 || a->cnt256_late != 0)
        err |= 64u;
    if (a->pages_late != 4 || a->nlarge_late != 1)
        err |= 128u;
    if (a->cursor_late != 257)
        err |= 256u;
    a->err = err;

    atomic_store_explicit(&a->ready, 1, memory_order_release);
    while (!atomic_load_explicit(&a->go, memory_order_acquire))
        sched_yield();
    return NULL;
}

static void check_large_raise(uint64_t base) {
    uint32_t max0 = wmm_tcache_max_pages_now();
    int nodes0 = tc_nodes();

    CHECK(max0 < WMM_TCACHE_LARGE_MAX_PAGES);
    CHECK(g_tcache->large_max_pages == max0);
    CHECK(wmm_ctl("config.tcache_max",
                  (int)(WMM_TCACHE_LARGE_MAX_PAGES * WMM_PAGE)) == 0);
    CHECK(wmm_tcache_max_pages_now() == WMM_TCACHE_LARGE_MAX_PAGES);

    /* 主缓存的数组仍是创建时大小,1MB 进不来 */
    void *big = wmm_alloc((size_t)1 << 20);
    CHECK(big != NULL);
    wmm_free(big);
    CHECK(large_slots(g_tcache) == 0);
    CHECK(total_live() == base);

    raise_arg_t a;
    a.err    = 0;
    a.maxp   = 0;
    a.cnt4   = 0;
    a.cnt256 = 0;
    a.pages  = 0;
    a.nlarge = 0;
    a.cnt4_late   = 0;
    a.cnt256_late = 0;
    a.cursor_late = 0;
    a.pages_late  = 0;
    a.nlarge_late = 0;
    atomic_init(&a.ready, 0);
    atomic_init(&a.go, 0);

    pthread_t th;
    if (pthread_create(&th, NULL, raise_worker, &a) != 0) {
        printf("  FAIL @%d: pthread_create\n", __LINE__);
        g_fail++;
    } else {
        while (!atomic_load_explicit(&a.ready, memory_order_acquire))
            sched_yield();
        CHECK(a.err == 0);
        CHECK(a.maxp == WMM_TCACHE_LARGE_MAX_PAGES);
        CHECK(a.cnt4 == 1 && a.cnt256 == 1);
        CHECK(a.pages == 260 && a.nlarge == 2);
        CHECK(a.cnt4_late == 1 && a.cnt256_late == 0);
        CHECK(a.pages_late == 4 && a.nlarge_late == 1);
        CHECK(a.cursor_late == 257);

        atomic_store_explicit(&a.go, 1, memory_order_release);
        CHECK(pthread_join(th, NULL) == 0);
        /* 新线程退出:大缓存随析构归还 */
        CHECK(tc_nodes() == nodes0);
        CHECK(wmm_tcache_cached_total() == 0);
        CHECK(total_live() == base);
    }

    CHECK(wmm_ctl("config.tcache_max", (int)(max0 * WMM_PAGE)) == 0);
    CHECK(wmm_tcache_max_pages_now() == max0);
    large_reset(base);
}

/* flush、双释放与按取用量 GC */
static void check_large_flush_gc(uint64_t base) {
    wmm_tcache_t *t = g_tcache;

    CHECK(t->large_max_pages >= 8);   /* 本用例的三个类都在默认上限内 */

    /* flush:多类存量一次归零,ndalloc 已在应用释放时记过 */
    void *a[4], *b[2], *c[2];
    for (int k = 0; k < 4; k++) {
        a[k] = wmm_alloc(4 * WMM_PAGE);
        CHECK(a[k] != NULL);
    }
    for (int k = 0; k < 2; k++) {
        b[k] = wmm_alloc(6 * WMM_PAGE);
        CHECK(b[k] != NULL);
        c[k] = wmm_alloc(8 * WMM_PAGE);
        CHECK(c[k] != NULL);
    }
    for (int k = 0; k < 4; k++)
        wmm_free(a[k]);
    for (int k = 0; k < 2; k++) {
        wmm_free(b[k]);
        wmm_free(c[k]);
    }
    CHECK(t->large_count[4] == 4);
    CHECK(t->large_count[6] == 2);
    CHECK(t->large_count[8] == 2);

    uint64_t nd0 = atomic_load_explicit(&g_large.ndalloc,
                                        memory_order_relaxed);
    wmm_tcache_flush_all();
    large_expect_empty(base);
    CHECK(atomic_load_explicit(&g_large.ndalloc, memory_order_relaxed) == nd0);

    /* 缓存块再次释放:页表已清,走 0xBE 诊断,链表与计数不动 */
    void *p = wmm_alloc(16384);
    CHECK(p != NULL);
    wmm_free(p);
    CHECK(t->large_count[4] == 1);
    CHECK(t->large_head[4] != NULL && t->large_head[4]->addr == p);
    wmm_free(p);
    CHECK(t->large_count[4] == 1);
    CHECK(t->large_head[4] != NULL && t->large_head[4]->addr == p);
    CHECK(t->large_pages == 4);
    CHECK(large_slots(t) == 1);
    CHECK(total_live() == base + 1);
    CHECK(wmm_phys_audit() == 0);
    large_reset(base);

    /* GC:4 页类取用 1 次保留 1 块,6 页与 8 页类未取用整类归还 */
    void *q[4], *r[2], *s[2];
    for (int k = 0; k < 4; k++) {
        q[k] = wmm_alloc(4 * WMM_PAGE);
        CHECK(q[k] != NULL);
    }
    for (int k = 0; k < 2; k++) {
        r[k] = wmm_alloc(6 * WMM_PAGE);
        CHECK(r[k] != NULL);
        s[k] = wmm_alloc(8 * WMM_PAGE);
        CHECK(s[k] != NULL);
    }
    for (int k = 0; k < 4; k++)
        wmm_free(q[k]);
    for (int k = 0; k < 2; k++) {
        wmm_free(r[k]);
        wmm_free(s[k]);
    }
    CHECK(t->large_count[4] == 4);
    CHECK(t->large_count[6] == 2);
    CHECK(t->large_count[8] == 2);

    void *hit = wmm_alloc(16384);
    CHECK(hit != NULL);
    CHECK(t->large_count[4] == 3);
    CHECK(t->large_used[4] == 1);

    uint64_t live0 = total_live();
    nd0 = atomic_load_explicit(&g_large.ndalloc, memory_order_relaxed);
    t->large_gc_cursor = 5;   /* 白盒:完整 GC 应复位游标 */
    CHECK(wmm_ctl("gc") == 0);
    CHECK(t->large_gc_cursor == WMM_TCACHE_LARGE_MIN_PAGES);
    CHECK(t->large_count[4] == 1);
    CHECK(t->large_count[6] == 0);
    CHECK(t->large_count[8] == 0);
    CHECK(t->large_pages == 4);
    CHECK(large_slots(t) == 1);
    CHECK(t->large_used[4] == 0);
    CHECK(total_live() == live0 - 6);
    CHECK(atomic_load_explicit(&g_large.ndalloc, memory_order_relaxed) == nd0);
    CHECK(wmm_phys_audit() == 0);

    void *again = wmm_alloc(16384);
    CHECK(again != NULL && again != hit);
    CHECK(wmm_usable_size(again) == 16384);
    CHECK(t->large_count[4] == 0);
    wmm_free(again);
    wmm_free(hit);
    large_reset(base);

    /* flush 不受滚动页限额:8 页类 20 块共 160 页一次清空 */
    void *u[WMM_TCACHE_LARGE_NSLOTS];
    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++) {
        u[k] = wmm_alloc(8 * WMM_PAGE);
        CHECK(u[k] != NULL);
    }
    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++)
        wmm_free(u[k]);
    CHECK(t->large_count[8] == WMM_TCACHE_LARGE_NSLOTS);
    CHECK(t->large_pages == 8 * WMM_TCACHE_LARGE_NSLOTS);

    t->large_gc_cursor = 6;
    wmm_tcache_flush_all();
    large_expect_empty(base);
}

/* 跨线程 GC:只做大对象 free 的线程也必须在 put 入口消费 gc_req */
typedef struct {
    atomic_int    ready;
    atomic_int    go;
    atomic_int    done;
    wmm_tcache_t *t;
    void         *hold;
    unsigned      err;
    uint16_t      cnt4_after;
    uint16_t      cnt5_after;
    uint16_t      cnt8_after;
    uint32_t      nlarge_after;
} large_gc_arg_t;

static void *large_gc_worker(void *arg) {
    large_gc_arg_t *a = (large_gc_arg_t *)arg;
    unsigned err = 0;

    wmm_thread_init();
    if (g_tcache == NULL) {
        a->err = 1u;
        atomic_store_explicit(&a->ready, 1, memory_order_release);
        while (!atomic_load_explicit(&a->go, memory_order_acquire))
            sched_yield();
        return NULL;
    }
    a->t = g_tcache;

    void *p = wmm_alloc(16384);
    if (p == NULL)
        err |= 2u;
    else
        wmm_free(p);                    /* 4 页类留一块存量 */

    void *b8[WMM_TCACHE_LARGE_NSLOTS];
    bool  b8_ok = true;
    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++) {
        b8[k] = wmm_alloc(8 * WMM_PAGE);
        if (b8[k] == NULL)
            b8_ok = false;
    }
    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++)
        if (b8[k] != NULL)
            wmm_free(b8[k]);
    if (!b8_ok)
        err |= 2u;

    a->hold = wmm_alloc(5 * WMM_PAGE);  /* 预分配,放行后释放 */
    if (a->hold == NULL)
        err |= 4u;
    if (g_tcache->large_count[4] != 1 ||
        g_tcache->large_count[8] != WMM_TCACHE_LARGE_NSLOTS)
        err |= 8u;

    a->err = err;
    atomic_store_explicit(&a->ready, 1, memory_order_release);
    while (!atomic_load_explicit(&a->go, memory_order_acquire))
        sched_yield();

    if (a->hold != NULL)
        wmm_free(a->hold);              /* 大对象 put 入口消费 gc_req */

    a->cnt4_after   = g_tcache->large_count[4];
    a->cnt5_after   = g_tcache->large_count[5];
    a->cnt8_after   = g_tcache->large_count[8];
    a->nlarge_after = atomic_load_explicit(&g_tcache->nlarge,
                                           memory_order_relaxed);
    if (atomic_load_explicit(&g_tcache->gc_req, memory_order_acquire) != 0)
        err |= 16u;
    a->err = err;
    atomic_store_explicit(&a->done, 1, memory_order_release);
    return NULL;
}

static void check_large_cross_gc(uint64_t base) {
    int nodes0 = tc_nodes();
    large_gc_arg_t a;

    a.t           = NULL;
    a.hold        = NULL;
    a.err         = 0;
    a.cnt4_after  = 0;
    a.cnt5_after  = 0;
    a.cnt8_after  = 0;
    a.nlarge_after = 0;
    atomic_init(&a.ready, 0);
    atomic_init(&a.go, 0);
    atomic_init(&a.done, 0);

    pthread_t th;
    if (pthread_create(&th, NULL, large_gc_worker, &a) != 0) {
        printf("  FAIL @%d: pthread_create\n", __LINE__);
        g_fail++;
        return;
    }
    while (!atomic_load_explicit(&a.ready, memory_order_acquire))
        sched_yield();

    CHECK(a.err == 0);
    CHECK(a.t != NULL && a.t != g_tcache);
    if (a.t != NULL) {
        CHECK(a.t->large_count[4] == 1);
        CHECK(a.t->large_count[8] == WMM_TCACHE_LARGE_NSLOTS);
        CHECK(wmm_tcache_cached_total() ==
              1 + WMM_TCACHE_LARGE_NSLOTS);

        /* 控制线程:自己同步收缩,工作线程只置位;它停在 go 上,
           不会消费请求,所以置位后可以稳定观察 1 */
        CHECK(wmm_ctl("gc") == 0);
        CHECK(atomic_load_explicit(&a.t->gc_req,
                                   memory_order_acquire) == 1);
    }

    atomic_store_explicit(&a.go, 1, memory_order_release);
    while (!atomic_load_explicit(&a.done, memory_order_acquire))
        sched_yield();
    CHECK(pthread_join(th, NULL) == 0);

    CHECK(a.err == 0);
    CHECK(a.cnt4_after == 0);       /* 请求先收缩旧类,再放入本块 */
    CHECK(a.cnt8_after == 0);       /* 跨线程请求是完整收缩,不受页限额 */
    CHECK(a.cnt5_after == 1);
    CHECK(a.nlarge_after == 1);
    CHECK(tc_nodes() == nodes0);
    CHECK(wmm_tcache_cached_total() == 0);
    CHECK(total_live() == base);
}

/* 滚动 GC:每 256 次大对象 put 自动收缩一次 */
static void check_large_rolling_gc(uint64_t base) {
    wmm_tcache_t *t = g_tcache;
    void *h[WMM_TCACHE_LARGE_NSLOTS];

    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++) {
        h[k] = wmm_alloc(16384);
        CHECK(h[k] != NULL);
    }
    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++)
        wmm_free(h[k]);
    CHECK(t->large_count[4] == WMM_TCACHE_LARGE_NSLOTS);
    CHECK(t->large_used[4] == 0);

    t->large_gc_evt = 0;   /* 白盒:让滚动周期从本次循环起算 */
    t->large_gc_cursor = WMM_TCACHE_LARGE_MIN_PAGES;

    for (unsigned k = 0; k < WMM_TCACHE_LARGE_GC_EVERY; k++) {
        void *p = wmm_alloc(5 * WMM_PAGE);
        CHECK(p != NULL);
        wmm_free(p);
    }
    CHECK(t->large_count[4] == 0);   /* 到点,未取用的类整类归还 */
    CHECK(t->large_gc_evt == 0);
    CHECK(t->large_gc_cursor == WMM_TCACHE_LARGE_MIN_PAGES);
    CHECK(t->large_count[5] == 1);
    CHECK(t->large_pages == 5);
    CHECK(large_slots(t) == 1);
    large_check_counts(t);
    large_reset(base);
}

/* 滚动 GC 按页限额并轮转页数类:默认 128 页的确定性回放 */
static void check_large_rolling_bounded(uint64_t base) {
    wmm_tcache_t *t = g_tcache;
    void *b4[WMM_TCACHE_LARGE_NSLOTS];
    void *b5[WMM_TCACHE_LARGE_NSLOTS];
    void *b6[WMM_TCACHE_LARGE_NSLOTS];
    void *b8[WMM_TCACHE_LARGE_NSLOTS];

    CHECK(WMM_TCACHE_LARGE_GC_PAGES == 128u);
    CHECK(t->large_max_pages >= 8);
    large_reset(base);

    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++) {
        b4[k] = wmm_alloc(4 * WMM_PAGE);
        b5[k] = wmm_alloc(5 * WMM_PAGE);
        b6[k] = wmm_alloc(6 * WMM_PAGE);
        b8[k] = wmm_alloc(8 * WMM_PAGE);
        CHECK(b4[k] != NULL && b5[k] != NULL &&
              b6[k] != NULL && b8[k] != NULL);
    }
    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++) {
        wmm_free(b4[k]);
        wmm_free(b5[k]);
        wmm_free(b6[k]);
        wmm_free(b8[k]);
    }
    CHECK(t->large_count[4] == WMM_TCACHE_LARGE_NSLOTS);
    CHECK(t->large_count[5] == WMM_TCACHE_LARGE_NSLOTS);
    CHECK(t->large_count[6] == WMM_TCACHE_LARGE_NSLOTS);
    CHECK(t->large_count[8] == WMM_TCACHE_LARGE_NSLOTS);
    CHECK(t->large_pages == 460);
    CHECK(t->large_used[4] == 0 && t->large_used[5] == 0 &&
          t->large_used[6] == 0 && t->large_used[8] == 0);
    large_check_counts(t);

    t->large_gc_cursor = 4;
    large_trigger_rolling(9);
    CHECK(460 - t->large_pages == 125);
    CHECK(t->large_pages == 335);
    CHECK(t->large_count[4] == 0);
    CHECK(t->large_count[5] == 11);
    CHECK(t->large_count[6] == WMM_TCACHE_LARGE_NSLOTS);
    CHECK(t->large_count[8] == WMM_TCACHE_LARGE_NSLOTS);
    CHECK(t->large_gc_cursor == 6);
    CHECK(total_live() == base + large_slots(t));
    large_check_counts(t);

    large_trigger_rolling(9);
    CHECK(335 - t->large_pages == 128);
    CHECK(t->large_pages == 207);
    CHECK(t->large_count[6] == 0);
    CHECK(t->large_count[8] == 19);
    CHECK(t->large_gc_cursor == 4);
    large_check_counts(t);

    large_trigger_rolling(9);
    CHECK(207 - t->large_pages == 127);
    CHECK(t->large_pages == 80);
    CHECK(t->large_count[5] == 0);
    CHECK(t->large_count[8] == 10);
    CHECK(t->large_gc_cursor == 4);
    large_check_counts(t);

    large_trigger_rolling(9);
    CHECK(80 - t->large_pages == 80);
    CHECK(t->large_pages == 0);
    CHECK(t->large_count[8] == 0);
    CHECK(t->large_gc_cursor == 4);
    CHECK(large_slots(t) == 0);
    CHECK(total_live() == base);
    large_check_counts(t);

    /* 游标持续轮转:同一类反复填满,事件最终能把它排空 */
    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++) {
        b8[k] = wmm_alloc(8 * WMM_PAGE);
        CHECK(b8[k] != NULL);
    }
    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++)
        wmm_free(b8[k]);
    CHECK(t->large_count[8] == WMM_TCACHE_LARGE_NSLOTS);

    unsigned events = 0;
    while (t->large_count[8] != 0 && events < 8) {
        large_trigger_rolling(9);
        events++;
    }
    CHECK(events == 2);
    CHECK(t->large_pages == 0);
    CHECK(large_slots(t) == 0);
    CHECK(total_live() == base);
    large_check_counts(t);
    large_reset(base);
}

/* 部分归还的类保留 used,下一圈继续朝同一目标收缩 */
static void check_large_rolling_partial(uint64_t base) {
    wmm_tcache_t *t = g_tcache;
    void *b[WMM_TCACHE_LARGE_NSLOTS];

    large_reset(base);
    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++) {
        b[k] = wmm_alloc(8 * WMM_PAGE);
        CHECK(b[k] != NULL);
    }
    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++)
        wmm_free(b[k]);
    CHECK(t->large_count[8] == WMM_TCACHE_LARGE_NSLOTS);
    CHECK(t->large_used[8] == 0);

    void *x = wmm_alloc(8 * WMM_PAGE);
    void *y = wmm_alloc(8 * WMM_PAGE);
    CHECK(x != NULL && y != NULL);
    CHECK(t->large_used[8] == 2);
    wmm_free(x);
    wmm_free(y);
    CHECK(t->large_count[8] == WMM_TCACHE_LARGE_NSLOTS);
    CHECK(t->large_used[8] == 2);

    t->large_gc_cursor = 8;
    large_trigger_rolling(9);
    CHECK(t->large_count[8] == 4);
    CHECK(t->large_used[8] == 2);          /* 未到 keep,保留取用信号 */
    CHECK(t->large_pages == 4 * 8);
    CHECK(t->large_gc_cursor == 4);
    large_check_counts(t);

    large_trigger_rolling(9);
    CHECK(t->large_count[8] == 2);
    CHECK(t->large_used[8] == 0);          /* 到达 keep,清信号 */
    CHECK(t->large_pages == 2 * 8);
    CHECK(t->large_gc_cursor == 4);
    CHECK(total_live() == base + large_slots(t));
    large_check_counts(t);
    large_reset(base);
}

/* 未轮到的类保留其取用计数,游标停在预算耗尽处 */
static void check_large_rolling_pending_used(uint64_t base) {
    wmm_tcache_t *t = g_tcache;
    void *b4[WMM_TCACHE_LARGE_NSLOTS];
    void *b6[WMM_TCACHE_LARGE_NSLOTS];
    void *b8[WMM_TCACHE_LARGE_NSLOTS];

    large_reset(base);
    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++) {
        b4[k] = wmm_alloc(4 * WMM_PAGE);
        b6[k] = wmm_alloc(6 * WMM_PAGE);
        b8[k] = wmm_alloc(8 * WMM_PAGE);
        CHECK(b4[k] != NULL && b6[k] != NULL && b8[k] != NULL);
    }
    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++) {
        wmm_free(b4[k]);
        wmm_free(b6[k]);
        wmm_free(b8[k]);
    }
    CHECK(t->large_pages == 360);

    void *x = wmm_alloc(8 * WMM_PAGE);
    CHECK(x != NULL);
    wmm_free(x);
    CHECK(t->large_count[8] == WMM_TCACHE_LARGE_NSLOTS);
    CHECK(t->large_used[8] == 1);

    t->large_gc_cursor = 4;
    large_trigger_rolling(9);
    CHECK(t->large_count[4] == 0);
    CHECK(t->large_count[6] == WMM_TCACHE_LARGE_NSLOTS - 8);
    CHECK(t->large_count[8] == WMM_TCACHE_LARGE_NSLOTS);
    CHECK(t->large_pages == 12 * 6 + 20 * 8);
    CHECK(t->large_used[8] == 1);          /* 类 8 未被访问 */
    CHECK(t->large_gc_cursor == 7);
    CHECK(total_live() == base + large_slots(t));
    large_check_counts(t);
    large_reset(base);
}

/* 超限类不可命中:滚动路径整类归还且不占页预算 */
static void check_large_rolling_above_limit(uint64_t base) {
    wmm_tcache_t *t = g_tcache;
    uint32_t max0 = wmm_tcache_max_pages_now();
    void *b[WMM_TCACHE_LARGE_NSLOTS];

    CHECK(max0 >= 8);
    large_reset(base);

    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++) {
        b[k] = wmm_alloc(8 * WMM_PAGE);
        CHECK(b[k] != NULL);
    }
    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++)
        wmm_free(b[k]);
    CHECK(t->large_count[8] == WMM_TCACHE_LARGE_NSLOTS);
    CHECK(t->large_pages == 160);

    /* 160 页超过滚动预算,但类 8 已超上限,仍一次整类归还 */
    CHECK(wmm_ctl("config.tcache_max", (int)(4 * WMM_PAGE)) == 0);
    t->large_gc_cursor = 4;
    large_trigger_rolling(5);
    CHECK(t->large_count[8] == 0);
    CHECK(t->large_pages == 0);
    CHECK(large_slots(t) == 0);
    CHECK(t->large_gc_cursor == WMM_TCACHE_LARGE_MIN_PAGES);
    CHECK(total_live() == base);
    large_check_counts(t);

    /* tcache_max=0:下一次滚动事件排空全部存量 */
    CHECK(wmm_ctl("config.tcache_max", (int)(max0 * WMM_PAGE)) == 0);
    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++) {
        b[k] = wmm_alloc(4 * WMM_PAGE);
        CHECK(b[k] != NULL);
    }
    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++)
        wmm_free(b[k]);
    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++) {
        b[k] = wmm_alloc(8 * WMM_PAGE);
        CHECK(b[k] != NULL);
    }
    for (unsigned k = 0; k < WMM_TCACHE_LARGE_NSLOTS; k++)
        wmm_free(b[k]);
    CHECK(t->large_count[4] == WMM_TCACHE_LARGE_NSLOTS);
    CHECK(t->large_count[8] == WMM_TCACHE_LARGE_NSLOTS);
    CHECK(t->large_pages == 240);

    void *q = wmm_alloc(5 * WMM_PAGE);
    CHECK(q != NULL);
    CHECK(wmm_ctl("config.tcache_max", 0) == 0);
    t->large_gc_cursor = WMM_TCACHE_LARGE_MIN_PAGES;
    t->large_gc_evt = WMM_TCACHE_LARGE_GC_EVERY - 1;
    wmm_free(q);
    CHECK(t->large_gc_evt == 0);
    CHECK(t->large_count[4] == 0);
    CHECK(t->large_count[8] == 0);
    CHECK(t->large_pages == 0);
    CHECK(large_slots(t) == 0);
    CHECK(t->large_gc_cursor == WMM_TCACHE_LARGE_MIN_PAGES);
    CHECK(total_live() == base);
    large_check_counts(t);

    CHECK(wmm_ctl("config.tcache_max", (int)(max0 * WMM_PAGE)) == 0);
    CHECK(wmm_tcache_max_pages_now() == max0);
    large_reset(base);
}

/* 创建时上限为 0 的线程:滚动事件直接复位,不碰大缓存数组 */
typedef struct {
    atomic_int ready;
    unsigned   err;
    uint32_t   maxp;
    uint16_t   evt;
    uint16_t   cursor;
    uint32_t   nlarge;
} zero_max_arg_t;

static void *zero_max_worker(void *arg) {
    zero_max_arg_t *a = (zero_max_arg_t *)arg;
    unsigned err = 0;

    wmm_thread_init();
    if (g_tcache == NULL) {
        a->err = 1u;
        atomic_store_explicit(&a->ready, 1, memory_order_release);
        return NULL;
    }

    a->maxp = g_tcache->large_max_pages;
    void *p = wmm_alloc(4 * WMM_PAGE);
    if (p == NULL) {
        err |= 2u;
    } else {
        g_tcache->large_gc_evt = WMM_TCACHE_LARGE_GC_EVERY - 1;
        wmm_free(p);
    }

    a->evt    = g_tcache->large_gc_evt;
    a->cursor = g_tcache->large_gc_cursor;
    a->nlarge = atomic_load_explicit(&g_tcache->nlarge, memory_order_relaxed);
    if (a->maxp != 0 || a->evt != 0 || a->nlarge != 0 ||
        a->cursor != WMM_TCACHE_LARGE_MIN_PAGES)
        err |= 4u;

    a->err = err;
    atomic_store_explicit(&a->ready, 1, memory_order_release);
    return NULL;
}

static void check_large_zero_max_thread(uint64_t base) {
    uint32_t max0 = wmm_tcache_max_pages_now();
    zero_max_arg_t a;

    large_reset(base);
    a.err    = 0;
    a.maxp   = 0;
    a.evt    = 0;
    a.cursor = 0;
    a.nlarge = 0;
    atomic_init(&a.ready, 0);

    CHECK(wmm_ctl("config.tcache_max", 0) == 0);
    CHECK(wmm_tcache_max_pages_now() == 0);

    pthread_t th;
    if (pthread_create(&th, NULL, zero_max_worker, &a) != 0) {
        printf("  FAIL @%d: pthread_create\n", __LINE__);
        g_fail++;
    } else {
        while (!atomic_load_explicit(&a.ready, memory_order_acquire))
            sched_yield();
        CHECK(a.err == 0);
        CHECK(a.maxp == 0);
        CHECK(a.evt == 0);
        CHECK(a.cursor == WMM_TCACHE_LARGE_MIN_PAGES);
        CHECK(a.nlarge == 0);
        CHECK(pthread_join(th, NULL) == 0);
        CHECK(wmm_tcache_cached_total() == 0);
        CHECK(total_live() == base);
    }

    CHECK(wmm_ctl("config.tcache_max", (int)(max0 * WMM_PAGE)) == 0);
    CHECK(wmm_tcache_max_pages_now() == max0);
    large_reset(base);
}

/* enabled=false 一并旁路大缓存,但 gc 仍能排空既有存量 */
static void check_large_disabled(uint64_t base) {
    wmm_tcache_t *t = g_tcache;

    void *p = wmm_alloc(16384);
    CHECK(p != NULL);
    wmm_free(p);
    CHECK(t->large_count[4] == 1);

    bool saved = t->enabled;
    t->enabled = false;

    CHECK(wmm_ctl("gc") == 0);
    CHECK(t->large_count[4] == 0);
    CHECK(large_slots(t) == 0);
    CHECK(total_live() == base);

    void *x = wmm_alloc(16384);
    CHECK(x != NULL);
    wmm_free(x);
    CHECK(t->large_count[4] == 0);
    CHECK(large_slots(t) == 0);
    CHECK(total_live() == base);

    t->enabled = true;
    void *y = wmm_alloc(16384);
    CHECK(y != NULL);
    wmm_free(y);
    CHECK(t->large_count[4] == 1);

    t->enabled = false;
    void *z = wmm_alloc(16384);
    CHECK(z != NULL && z != y);       /* 没有命中 y 的缓存 */
    wmm_free(z);                      /* 也旁路,不进缓存 */
    CHECK(t->large_count[4] == 1);

    t->enabled = saved;
    void *w = wmm_alloc(16384);
    CHECK(w == y);                    /* 恢复后照常命中 */
    wmm_free(w);
    large_reset(base);
}

static void check_large_extent_cache(void) {
    uint64_t base = flush_baseline();

    CHECK(g_tcache != NULL);
    if (g_tcache == NULL)
        return;

    check_large_basic(base);
    check_large_limits(base);
    check_large_raise(base);
    check_large_flush_gc(base);
    check_large_cross_gc(base);
    check_large_rolling_gc(base);
    check_large_rolling_bounded(base);
    check_large_rolling_partial(base);
    check_large_rolling_pending_used(base);
    check_large_rolling_above_limit(base);
    check_large_zero_max_thread(base);
    check_large_disabled(base);

    CHECK(flush_baseline() == base);
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
static void    *g_fork_large;

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

    /* 大缓存也带一块进子进程 */
    void *lp = wmm_alloc(16384);
    CHECK(lp != NULL);
    wmm_free(lp);
    CHECK(g_tcache->large_count[4] == 1);

    g_fork_bin   = bin;
    g_fork_head  = cb->head;
    g_fork_live  = total_live();
    g_fork_large = lp;

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
        if (wmm_tcache_cached_total() != g_fork_head + 1)
            bad |= 4;                /* 小对象栈与大块原样过继 */
        if (total_live() != g_fork_live)
            bad |= 8;                /* 没有对象被凭空归还或吞掉 */
        if (g_tcache->large_count[4] != 1 ||
            g_tcache->large_head[4] == NULL ||
            g_tcache->large_head[4]->addr != g_fork_large)
            bad |= 64;

        void *p = wmm_alloc(64);     /* 必须命中:水位恰降一格 */
        if (p == NULL)
            bad |= 16;
        if ((unsigned)g_tcache->bins[g_fork_bin].head + 1 != h0)
            bad |= 32;
        wmm_free(p);

        void *lq = wmm_alloc(16384);
        if (lq != g_fork_large)
            bad |= 128;              /* 子进程也能原样 take 到大块 */
        wmm_free(lq);

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
    unsigned   large_at_ready;
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

    if (g_tcache != NULL) {
        void *big = wmm_alloc(16384);
        if (big == NULL)
            err |= 4u;
        else
            wmm_free(big);       /* 死线程的大缓存块也要被子进程清掉 */
    }

    a->head_at_ready  = (g_tcache != NULL)
                          ? g_tcache->bins[bin].head : 0;
    a->large_at_ready = (g_tcache != NULL)
                          ? g_tcache->large_count[4] : 0;
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

    a.err            = 0;
    a.head_at_ready  = 0;
    a.large_at_ready = 0;
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
    CHECK(a.large_at_ready == 1);

    g_ghost_pre_live   = total_live();
    g_ghost_pre_cached = wmm_tcache_cached_total();
    /* 主线程缓存已空:缓存总量就是死线程的小对象加大块 */
    CHECK(g_ghost_pre_cached == a.head_at_ready + a.large_at_ready);

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
    check_ctl_gc_self();
    check_ctl_gc_cross_thread();
    check_thread_lifecycle();
    check_thread_churn();
    check_disabled_path();
    check_large_extent_cache();
    check_cross_thread_free();
    check_fork_single_thread();
    check_fork_ghost();

    if (g_fail == 0)
        printf("  all tcache checks passed\n");
    return g_fail;
}
