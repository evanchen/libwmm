/* test_stress.c —— 多线程压力:独立随机序列 + 屏障对账 + RSS 观察
 *
 * 与 oracle 的分工:oracle 单线程把每一步的不变量钉死;本文件验证
 * 同样的性质在并发下依然成立——36 把 bin 锁 + 一把物理层锁的世界里,
 * 任何被遗漏的加锁点、任何跨线程的计数破坏,在这里现形。
 *
 * 结构:4/8/16 线程三个阶段。每阶段总步数(20 万)与槽位总配额
 * (512)都固定、按线程均分,分 20 轮,每轮结束屏障汇合对一次账:
 *   Σ 各线程模型活指针数 == Σ bins live + 大对象 live(相对进场基线)
 * 对账必须靠屏障而不是"偶尔看一眼":活着的线程随时在改计数,只有
 * 全体静止的瞬间,全局 Σ 与模型 Σ 的等式才有意义。
 *
 * 逐指针检查(对齐/usable/指纹/保内容)与 oracle 同款,线程只碰
 * 自己的槽位行,不引入共享可写状态;断言失败计入线程本地计数,
 * join 之后汇总——共享失败计数本身就是一个需要锁的状态,不添乱。
 *
 * RSS 观察:每阶段结束读 /proc/self/statm 打印。总步数与槽位总配额
 * (512)三阶段恒定,峰值工作集相同;空闲页按设计留在映射里等复用
 * (真正还内核是 purge 阶段的事),所以期望形态是"涨到峰值后持平",
 * 不是回落。阶段间显著上台阶 = 释放页没有回到复用池,合并或复用失效。
 * RSS 混着 ASan 影子页/线程栈等大量非分配器杂音,复用还有一道更尖
 * 的白盒断言直接盯窗口 bump 指针:阶段 2/3 的容量与配比和阶段 1
 * 全同,峰值需求必须落在阶段 1 已建立的足迹(加复用池)之内。
 */
#include "wmm_internal.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>     /* sched_yield:屏障自旋时让出 CPU——16 线程
                          干转圈会抢走还在干活的线程的时间片 */
#include <unistd.h>    /* sysconf:RSS 换算页大小 */

static int g_fail;      /* 仅主线程写;工作线程各自记账,join 后汇总 */

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("  FAIL @%d: %s\n", __LINE__, #cond);               \
            g_fail++;                                                  \
        }                                                              \
    } while (0)

/* 线程内断言:写线程本地计数 *pf(printf 自带内部锁,并发打无碍) */
#define SCHECK(pf, cond)                                               \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("  FAIL @%d: %s\n", __LINE__, #cond);               \
            (*(pf))++;                                                 \
        }                                                              \
    } while (0)

enum { WMM_STRESS_ROW = 128,          /* 每线程槽位数组的行容量(最大配额) */
       WMM_STRESS_TOTAL_SLOTS = 512,  /* 全体线程的槽位总配额:三阶段恒定,
                                         峰值工作集才有跨阶段可比性 */
       WMM_STRESS_ROUNDS = 20,
       WMM_STRESS_PHASE_STEPS = 200000, WMM_STRESS_MAX_THREADS = 16 };

static const int g_stress_nthreads[] = { 4, 8, 16 };

/* xorshift,状态由调用方持有:每线程一条独立流(种子掺线程号),
   不能再用文件级全局——多线程写它本身就是数据竞争 */
static uint32_t rng_next(uint64_t *s) {
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return (uint32_t)(*s >> 32);
}

/* 尺寸生成:与 test_oracle.c 同款带宽加权(小对象为主,覆盖到 1MB),
   随机数取自调用方的流 */
static size_t gen_size(uint64_t *st) {
    uint32_t r = rng_next(st) % 100;
    if (r < 35) return 1 + rng_next(st) % 64;
    if (r < 65) return 65 + rng_next(st) % (4096 - 64);
    if (r < 80) return 4097 + rng_next(st) % (14336 - 4096);
    if (r < 95) return 14337 + rng_next(st) % (262144 - 14336);
    return 262145 + rng_next(st) % (1048576 - 262144);
}

static uint64_t total_live(void) {
    uint64_t sum = 0;
    for (int i = 0; i < WMM_NBINS; i++)
        sum += g_bins[i].live;
    return sum + g_large.live;
}

/* /proc/self/statm 第二项 = 常驻页数;读不到返回 0(对照断言兜底) */
static size_t rss_kb(void) {
    FILE  *f = fopen("/proc/self/statm", "r");
    size_t total = 0, res = 0;

    if (f == NULL)
        return 0;
    if (fscanf(f, "%zu %zu", &total, &res) != 2)
        res = 0;
    fclose(f);
    return res * (size_t)sysconf(_SC_PAGESIZE) / 1024;
}

/* ---- 自旋屏障 ----
 * 最后到达者清计数、拨轮次号;先到者自旋等轮次号变化。
 * 为什么不用 pthread_barrier_t:不依赖平台可选项,内存序自己写明白
 * ——到达计数是原子读改写链,最后到达者对 gen 的写与等待者的
 * 获取读配对,各线程在屏障前发布的 live_pub 由此对检查者可见。
 * 快速线程提前进入下一轮也不会误撞:下一轮的到达都发生在 gen
 * 变化之后,而 count 已被最后一位置零。 */
typedef struct {
    atomic_uint count;
    atomic_uint gen;
    int         n;
} wmm_barrier_t;

static void barrier_init(wmm_barrier_t *b, int n) {
    atomic_init(&b->count, 0);
    atomic_init(&b->gen, 0);
    b->n = n;
}

static void barrier_wait(wmm_barrier_t *b) {
    unsigned g = atomic_load(&b->gen);
    if (atomic_fetch_add(&b->count, 1u) == (unsigned)b->n - 1u) {
        atomic_store(&b->count, 0u);
        atomic_store(&b->gen, g + 1u);
    } else {
        while (atomic_load_explicit(&b->gen, memory_order_acquire) == g)
            sched_yield();
    }
}

/* ---- 每线程模型与共享只读状态 ---- */

typedef struct {
    unsigned char *p;
    size_t         req;
    uint64_t       tag;
} wmm_oref_t;
static wmm_oref_t g_row[WMM_STRESS_MAX_THREADS][WMM_STRESS_ROW];

#define WMM_TAG_MAGIC 0x53545245535377ULL   /* "STRESSw" 化身常数 */

static uint8_t pat_byte(uint64_t tag, size_t off) {
    return (uint8_t)((tag >> ((off & 7u) * 8)) ^ off ^ 0xA5u);
}

typedef struct {
    int      tid;
    int      nthreads;
    size_t   slots;         /* 本线程槽位配额 = 512/线程数:每线程槽数
                               随线程数缩、总量恒定,峰值工作集跨阶段
                               可比(见文件头) */
    size_t   round_steps;
    long     fails;      /* join 后主线程读;写者只有本线程 */
    uint64_t live_pub;   /* 屏障间发布的模型活指针数:两道屏障之间
                            全体静止,主读者无须原子访问 */
} wmm_sarg_t;
static wmm_sarg_t    g_sarg[WMM_STRESS_MAX_THREADS];
static wmm_barrier_t g_bar;
static uint64_t      g_live0;   /* 进场基线:前序组的计数史不归本测试管 */

static int s_find(wmm_oref_t *row, size_t nslots, bool want_live, size_t start) {
    for (size_t k = 0; k < nslots; k++) {
        size_t s = (start + k) % nslots;
        if ((row[s].p != NULL) == want_live)
            return (int)s;
    }
    return -1;
}

/* 指纹掺线程号:并发下唯一可疑的坏法是"把 A 线程的存储发给 B",
   线程号入指纹让这种交叉交付也现形 */
static int s_alloc(wmm_oref_t *row, int tid, size_t s, size_t req, long *pf) {
    unsigned char *p = (unsigned char *)wmm_alloc(req);
    SCHECK(pf, p != NULL);
    if (p == NULL)
        return 0;

    size_t u = wmm_usable_size(p);
    SCHECK(pf, u >= req);
    SCHECK(pf, ((uintptr_t)p & 15u) == 0 || u == 8u);

    uint64_t tag = WMM_TAG_MAGIC ^ ((uint64_t)tid << 40) ^ ((uint64_t)s << 24);
    memcpy(p, &tag, sizeof tag);
    row[s].p   = p;
    row[s].req = req;
    row[s].tag = tag;
    return 1;
}

static int s_free(wmm_oref_t *row, size_t s, long *pf) {
    SCHECK(pf, row[s].p != NULL);
    if (row[s].p == NULL)
        return 0;
    uint64_t tag;
    memcpy(&tag, row[s].p, sizeof tag);
    SCHECK(pf, tag == row[s].tag);   /* 指纹被毁 = 交叉交付/踩踏 */
    wmm_free(row[s].p);
    row[s].p = NULL;
    return -1;
}

static int s_realloc(wmm_oref_t *row, size_t s, size_t nreq, long *pf) {
    unsigned char *old = row[s].p;
    size_t         oreq = row[s].req;
    size_t         old_u = wmm_usable_size(old);   /* 旧块信息先取 */

    size_t fill = oreq < 128 ? oreq : 128;
    for (size_t k = 0; k < fill; k++)
        old[k] = pat_byte(row[s].tag, k);

    unsigned char *np = (unsigned char *)wmm_realloc(old, nreq);
    SCHECK(pf, np != NULL);
    if (np == NULL)
        return 0;

    size_t u = wmm_usable_size(np);
    SCHECK(pf, u >= nreq);
    SCHECK(pf, ((uintptr_t)np & 15u) == 0 || u == 8u);
    if (nreq <= old_u)
        SCHECK(pf, np == old);      /* 契约:不超旧 usable 原地返回 */

    size_t keep = oreq < nreq ? oreq : nreq;
    if (keep > 128)
        keep = 128;
    for (size_t k = 0; k < keep; k++)
        SCHECK(pf, np[k] == pat_byte(row[s].tag, k));
    memcpy(np, &row[s].tag, sizeof row[s].tag);

    row[s].p   = np;
    row[s].req = nreq;
    return 0;
}

/* 屏障静止时刻的全局对账:Σ 各线程模型数 == Σ 分配器计数(相对
   进场基线)。tid==0 专属执行,其余线程停在第二道屏障上——
   计数没有可变窗口,读数即精确值 */
static void barrier_check(int nthreads, long *pf) {
    uint64_t model = 0;
    for (int t = 0; t < nthreads; t++)
        model += g_sarg[t].live_pub;
    SCHECK(pf, total_live() - g_live0 == model);
}

static void *stress_thread(void *arg) {
    wmm_sarg_t *me = (wmm_sarg_t *)arg;
    wmm_oref_t *row = g_row[me->tid];
    size_t      nslots = me->slots;
    long        fails = 0;
    long        n_live = 0;
    uint64_t    st = 0x9E3779B97F4A7C15ULL
                   + (uint64_t)(me->tid + 1) * 0x9E3779B97F4A7C15ULL;

    for (int r = 0; r < WMM_STRESS_ROUNDS; r++) {
        for (size_t k = 0; k < me->round_steps; k++) {
            uint32_t rr = rng_next(&st) & 3u;
            size_t   start = rng_next(&st) % nslots;

            if (rr == 0) {              /* 分配:优先空槽;表满先腾位 */
                int s = s_find(row, nslots, false, start);
                if (s < 0) {
                    int v = s_find(row, nslots, true, 0);
                    if (v >= 0)
                        n_live += s_free(row, (size_t)v, &fails);
                    s = v;
                }
                if (s >= 0)
                    n_live += s_alloc(row, me->tid, (size_t)s,
                                      gen_size(&st), &fails);
            } else if (rr <= 2) {       /* 释放:打中活槽才释放,空槽
                                           回退分配。打点而非扫描——
                                           扫描版成功率与占用率无关,
                                           占用率会被钉死在 0 附近
                                           (推导见 test_oracle.c);
                                           打点版均衡在 75% */
                if (row[st].p != NULL)
                    n_live += s_free(row, st, &fails);
                else
                    n_live += s_alloc(row, me->tid, st,
                                      gen_size(&st), &fails);
            } else {                    /* realloc:优先活槽;表空改分配 */
                int s = s_find(row, nslots, true, start);
                if (s < 0)
                    n_live += s_alloc(row, me->tid, start,
                                      gen_size(&st), &fails);
                else
                    n_live += s_realloc(row, (size_t)s,
                                        gen_size(&st), &fails);
            }
        }
        me->live_pub = (uint64_t)n_live;
        barrier_wait(&g_bar);
        if (me->tid == 0)
            barrier_check(me->nthreads, &fails);
        barrier_wait(&g_bar);
    }

    /* 阶段收尾:全量释放,清零后再全局对一次账(并发下的无泄漏) */
    for (size_t s = 0; s < nslots; s++)
        if (row[s].p != NULL)
            n_live += s_free(row, s, &fails);
    me->live_pub = 0;
    barrier_wait(&g_bar);
    if (me->tid == 0)
        barrier_check(me->nthreads, &fails);
    barrier_wait(&g_bar);

    me->fails = fails;
    return NULL;
}

int test_stress(void) {
    g_fail = 0;
    wmm_boot();
    g_live0 = total_live();

    size_t rss_first = 0, rss_last = 0;
    void  *win_entry = g_heap_cur;   /* 进场 bump 指针:oracle 组全量
                                        释放后复用池很大,阶段 1 的峰值
                                        需求应基本落在池内;允许小量
                                        补充,增量打印出来观察 */
    void  *win_pin   = NULL;         /* 阶段 1 足迹:阶段 2/3 与它容量
                                        全同,bump 不得再越此界 */

    for (size_t ph = 0;
         ph < sizeof g_stress_nthreads / sizeof g_stress_nthreads[0]; ph++) {
        int n = g_stress_nthreads[ph];

        memset(g_row, 0, sizeof g_row);   /* 阶段从空世界起步 */
        for (int t = 0; t < n; t++) {
            g_sarg[t].tid = t;
            g_sarg[t].nthreads = n;
            /* 跨阶段可比性的两个前提同时钉死:总步数固定按线程均分;
               槽位总配额固定 512 按线程均分(4 线程 128、8 线程 64、
               16 线程 32)。只钉步数不钉槽数的话,16 线程阶段的峰值
               活指针天然是 4 线程阶段的 4 倍——峰值工作集由并发容量
               决定,不由总步数决定 */
            g_sarg[t].slots = WMM_STRESS_TOTAL_SLOTS / (size_t)n;
            g_sarg[t].round_steps = WMM_STRESS_PHASE_STEPS
                                  / ((size_t)n * WMM_STRESS_ROUNDS);
            g_sarg[t].fails = 0;
            g_sarg[t].live_pub = 0;
        }
        barrier_init(&g_bar, n);

        size_t rss_before = rss_kb();

        pthread_t th[WMM_STRESS_MAX_THREADS];
        int created = 0;
        for (; created < n; created++)
            if (pthread_create(&th[created], NULL,
                               stress_thread, &g_sarg[created]) != 0)
                break;
        CHECK(created == n);
        if (created < n) {
            /* 线程建不全 = 环境已不可测;已建线程会停在屏障上,
               挂起远比快速失败糟,直接终止整个测试进程 */
            printf("  cannot create %d threads (got %d), aborting test\n",
                   n, created);
            exit(1);
        }
        for (int t = 0; t < n; t++)
            pthread_join(th[t], NULL);

        size_t rss_after = rss_kb();

        long fails = 0;
        for (int t = 0; t < n; t++)
            fails += g_sarg[t].fails;
        g_fail += (int)fails;

        printf("  phase %2d threads x %zu steps: %s, RSS %zu -> %zu KB, "
               "window +%ld KB\n",
               n, (size_t)WMM_STRESS_PHASE_STEPS,
               fails ? "FAIL" : "PASS", rss_before, rss_after,
               (long)(((char *)g_heap_cur - (char *)win_entry) >> 10));

        if (ph == 0) {
            rss_first = rss_after;
            win_pin   = g_heap_cur;
        } else {
            /* 复用尖断言(纯分配器量,无 RSS 杂音):三阶段槽数/步数/
               配比全同,阶段 2/3 的峰值需求必须落在阶段 1 足迹内;
               1MB 预算只吸收并发调度的碎片抖动(部分填充 slab 的
               数量随调度略有浮动)。复用失效的特征量级完全不同:每
               阶段 GB 级分配流量,1% 漏复用就新增 20MB 以上足迹 */
            CHECK((char *)g_heap_cur - (char *)win_pin
                      <= (ptrdiff_t)(WMM_PAGE * 256));
        }
        rss_last = rss_after;
    }

    /* RSS 平稳(粗粒度总检;复用的尖断言在上面盯窗口 bump):三阶段
       总步数与槽位总配额都固定,峰值工作集相同;空闲页按设计留在
       映射里等复用(真正还内核是 purge 的事),期望形态是"涨到峰值
       后持平"。容差 20MB 的构成(首轮实测分解):每阶段新起 4/8/16
       个线程,栈页与 ASan 线程登记/影子页在 join 后仍留驻 RSS,
       跨阶段合计 ~8MB;再加占用率随机演化的峰值波动。复用失效的
       信号量级完全不同——每阶段 GB 级分配流量,1% 漏复用就新增
       20MB+ 触碰——容差挡得住噪声,拦得住真错 */
    CHECK(rss_first > 0);
    CHECK(rss_last <= rss_first + 20480);
    printf("  RSS plateau: first %zu KB, last %zu KB (tolerance 20480 KB)\n",
           rss_first, rss_last);

    if (g_fail == 0)
        printf("  all stress checks passed\n");
    return g_fail;
}
