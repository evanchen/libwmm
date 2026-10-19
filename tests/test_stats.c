/* test_stats.c —— M11 统计与事件用例(白盒:直接对账内部计数)
 *
 * 覆盖(tasks.md M11 自测清单):
 *   OBS1 恒等式:Σ cls[i].live*size + 大对象字节 == live_bytes。用
 *        独立核算(直接读全局计数)与快照对照——拿快照自己的求和
 *        重放恒等式是恒真的,测不出任何东西;
 *   快照增量:已知工作量的分配/释放字节数与对象数逐项对账,
 *        epoch 严格 +1,high_water 只升不降;
 *   active/frag:purge 前后 active 恰好减少"同一锁内读到的脏页
 *        字节数",frag == active − live 恒成立;
 *   线程表:工作线程存活时其行的计数与手工计算一致,退出后行消失;
 *   事件:OOM(1TB 请求越窗口上限)、野指针释放、大对象双释放各产
 *        出一条对应类型的事件,字段(时间戳/线程号/调用点/指针)
 *        非空;drain 取出即消费。
 *
 * 确定性策略:本组开始时重建主线程缓存并关掉 enabled(同 test_phys),
 * 释放即回 bin,计数不经缓存中转;进程内其余线程都已 join,白盒
 * 直读全局计数没有并发写者。快照与直读的差值因此可以写死。
 * 本组放在用例表末尾:不依赖后续组,也不给后续组留半开状态
 * (组尾恢复 enabled)。
 */

#define _GNU_SOURCE        /* gettid(2) */

#include "wmm_internal.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static int g_fail;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("  FAIL @%d: %s\n", __LINE__, #cond);               \
            g_fail++;                                                  \
        }                                                              \
    } while (0)

/* ---- 独立核算:直接读全局计数(单线程,无并发写者) ---- */
static uint64_t live_bytes_direct(void) {
    uint64_t sum = 0;
    for (szind_t i = 0; i < WMM_NBINS; i++)
        sum += g_bins[i].live * (uint64_t)wmm_bin_info[i].reg_size;
    return sum + g_large.live_bytes;
}

static uint64_t live_objects_direct(void) {
    uint64_t sum = 0;
    for (szind_t i = 0; i < WMM_NBINS; i++)
        sum += g_bins[i].live;
    return sum + g_large.live;
}

static size_t dirty_pages_locked(void) {
    pthread_mutex_lock(&g_ext_lock);
    size_t n = g_dirty_pages;
    pthread_mutex_unlock(&g_ext_lock);
    return n;
}

/* ---- 快照增量 + OBS1 独立核算 ---- */
static void check_snapshot_accounting(void) {
    enum { NS = 40, NL = 4 };
    static void *ps[NS], *pl[NL];

    wmm_stats_t s0;
    CHECK(wmm_stats_snapshot(&s0) == 0);

    uint64_t expect_alloc = 0, expect_free = 0, expect_live = 0;

    for (int k = 0; k < NS; k++) {              /* 小对象:跨多个尺寸类 */
        ps[k] = wmm_alloc(96 + (size_t)k * 8);
        CHECK(ps[k] != NULL);
        expect_alloc += wmm_usable_size(ps[k]);
    }
    for (int k = 0; k < NL; k++) {              /* 大对象:走物理层 */
        pl[k] = wmm_alloc(100000 + (size_t)k * 5000);
        CHECK(pl[k] != NULL);
        expect_alloc += wmm_usable_size(pl[k]);
    }

    /* 释放一半:live 增量应只剩另一半 */
    for (int k = 0; k < NS; k += 2) {
        expect_free += wmm_usable_size(ps[k]);
        wmm_free(ps[k]);
        ps[k] = NULL;
    }
    for (int k = 0; k < NL; k += 2) {
        expect_free += wmm_usable_size(pl[k]);
        wmm_free(pl[k]);
        pl[k] = NULL;
    }
    for (int k = 0; k < NS; k++)
        if (ps[k] != NULL)
            expect_live += wmm_usable_size(ps[k]);
    for (int k = 0; k < NL; k++)
        if (pl[k] != NULL)
            expect_live += wmm_usable_size(pl[k]);

    wmm_stats_t s1;
    CHECK(wmm_stats_snapshot(&s1) == 0);

    CHECK(s1.epoch == s0.epoch + 1);                        /* 严格递增 */
    CHECK(s1.live_bytes - s0.live_bytes == expect_live);
    CHECK(s1.allocated_bytes - s0.allocated_bytes == expect_alloc);
    CHECK(s1.deallocated_bytes - s0.deallocated_bytes == expect_free);

    /* OBS1:快照与独立核算逐字节一致 */
    CHECK(s1.live_bytes == live_bytes_direct());
    CHECK(s1.live_objects == live_objects_direct());

    /* 口径:live ≤ active,frag 恰是差值 */
    CHECK(s1.active_bytes >= s1.live_bytes);
    CHECK(s1.frag_bytes == s1.active_bytes - s1.live_bytes);

    for (int k = 0; k < NS; k++)
        if (ps[k] != NULL)
            wmm_free(ps[k]);
    for (int k = 0; k < NL; k++)
        if (pl[k] != NULL)
            wmm_free(pl[k]);

    wmm_stats_t s2;
    CHECK(wmm_stats_snapshot(&s2) == 0);
    CHECK(s2.live_bytes == s0.live_bytes);   /* 全部归还:回到起点 */
    CHECK(s2.live_bytes == live_bytes_direct());
}

/* ---- high_water:只升不降,记录峰值 ---- */
static void check_high_water(void) {
    wmm_stats_t a0;
    CHECK(wmm_stats_snapshot(&a0) == 0);

    enum { NB = 32 };
    static void *blk[NB];
    const size_t BSZ = 4u << 20;

    for (int k = 0; k < NB; k++) {
        blk[k] = wmm_alloc(BSZ);
        CHECK(blk[k] != NULL);
    }

    wmm_stats_t peak;
    CHECK(wmm_stats_snapshot(&peak) == 0);
    CHECK(peak.live_bytes == a0.live_bytes + (uint64_t)NB * BSZ);
    CHECK(peak.high_water_live >= peak.live_bytes);

    for (int k = 0; k < NB; k++)
        wmm_free(blk[k]);

    wmm_stats_t after;
    CHECK(wmm_stats_snapshot(&after) == 0);
    CHECK(after.live_bytes == a0.live_bytes);
    CHECK(after.high_water_live >= peak.live_bytes);   /* 峰值被记住 */
}

/* ---- active 记账 + purge:减少量恰等于脏页字节数 ---- */
static void check_active_purge(void) {
    /* 清掉前序用例的事件,好让 0xDC 的计数干净 */
    wmm_event_t buf[16];
    size_t      got;
    do {
        CHECK(wmm_event_drain(buf, 16, &got) == 0);
    } while (got > 0);

    enum { NB = 32 };
    static void *blk[NB];
    const size_t BSZ = 4u << 20;

    for (int k = 0; k < NB; k++) {
        blk[k] = wmm_alloc(BSZ);
        CHECK(blk[k] != NULL);
    }
    for (int k = 0; k < NB; k++)
        wmm_free(blk[k]);

    wmm_stats_t a1;
    CHECK(wmm_stats_snapshot(&a1) == 0);

    size_t dirty = dirty_pages_locked();
    CHECK(dirty > 0);

    CHECK(wmm_ctl("purge") == 0);

    wmm_stats_t a2;
    CHECK(wmm_stats_snapshot(&a2) == 0);

    /* purge 把"读到的那些脏页"整批还给内核:active 的减少量恰好
       是同一把锁里读到的脏页字节数(单线程,中间无分配) */
    CHECK(a2.active_bytes == a1.active_bytes - dirty * WMM_PAGE);
    CHECK(a2.live_bytes == a1.live_bytes);          /* 回收不动存活 */
    CHECK(a2.active_bytes >= a2.live_bytes);        /* 口径不变式 */

    /* purge 写了一条 0xDC 事件 */
    CHECK(wmm_event_drain(buf, 16, &got) == 0);
    unsigned n_dc = 0;
    for (size_t k = 0; k < got; k++)
        if (buf[k].type == 0xDC)
            n_dc++;
    CHECK(n_dc == 1);
}

/* ---- 事件:OOM / 野指针 / 双释放 ---- */
static void check_events(void) {
    wmm_event_t buf[16];
    size_t      got;

    /* 清空环,保证下面数到的都是本次触发 */
    do {
        CHECK(wmm_event_drain(buf, 16, &got) == 0);
    } while (got > 0);

    /* 0xDB:请求 1TB 越过窗口上限,物理层在入口挡下 */
    void *huge = wmm_alloc((size_t)1 << 40);
    CHECK(huge == NULL);

    /* 0xBE:窗口外指针(野指针) */
    wmm_free((void *)(uintptr_t)0x1234);

    /* 0xBE:大对象双释放。第一次 free 后页表整块清零,第二次
       反查 raw==0,与"从未分配"统一走同一条诊断路径 */
    void *q = wmm_alloc(4 * WMM_PAGE);
    CHECK(q != NULL);
    wmm_free(q);
    wmm_free(q);

    CHECK(wmm_event_drain(buf, 16, &got) == 0);
    CHECK(got == 3);

    unsigned n_db = 0, n_be = 0;
    for (size_t k = 0; k < got; k++) {
        CHECK(buf[k].ts != 0);            /* 时间戳 */
        CHECK(buf[k].tid != 0);           /* 线程号 */
        CHECK(buf[k].pc0 != 0);           /* 调用点第 1 帧 */
        CHECK(buf[k].pc1 != 0);           /* 调用点第 2 帧 */
        if (buf[k].type == 0xDB)
            n_db++;
        else if (buf[k].type == 0xBE)
            n_be++;
        else
            CHECK(0);                     /* 不该有别的类型 */
        if (buf[k].ptr == (uintptr_t)0x1234)
            CHECK(buf[k].type == 0xBE);   /* 野指针事件带原指针 */
    }
    CHECK(n_db == 1 && n_be == 2);

    /* 取出即消费:再 drain 应为空 */
    CHECK(wmm_event_drain(buf, 16, &got) == 0);
    CHECK(got == 0);

    /* 参数防御 */
    CHECK(wmm_event_drain(NULL, 1, &got) == -1);
    CHECK(errno == EINVAL);
    CHECK(wmm_event_drain(buf, 0, &got) == -1);
    CHECK(errno == EINVAL);
}

/* ---- 线程表:存活线程的行计数精确,退出后行消失 ---- */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  cv;
    bool            ready;     /* 分配+释放完毕,等主线程放行 */
    bool            release;   /* 主线程放行 */
    uint32_t        tid;
} stats_worker_t;

static void *stats_worker(void *arg) {
    stats_worker_t *w = (stats_worker_t *)arg;
    void           *objs[54];

    for (int k = 0; k < 50; k++) {              /* 50 × 4096B 小对象 */
        objs[k] = wmm_alloc(4096);
        CHECK(objs[k] != NULL);
    }
    for (int k = 0; k < 4; k++) {               /* 4 × 100KB 大对象 */
        objs[50 + k] = wmm_alloc(100000);
        CHECK(objs[50 + k] != NULL);
    }
    for (int k = 0; k < 54; k++)
        wmm_free(objs[k]);

    w->tid = (uint32_t)gettid();

    /* 计数已被本线程写进自己的 tcache 块;持锁停在活着的状态,
       让主线程快照时能读到这一行 */
    pthread_mutex_lock(&w->lock);
    w->ready = true;
    pthread_cond_broadcast(&w->cv);
    while (!w->release)
        pthread_cond_wait(&w->cv, &w->lock);
    pthread_mutex_unlock(&w->lock);
    return NULL;
}

static void check_thread_row(void) {
    stats_worker_t w;
    memset(&w, 0, sizeof(w));
    pthread_mutex_init(&w.lock, NULL);
    pthread_cond_init(&w.cv, NULL);

    pthread_t th;
    CHECK(pthread_create(&th, NULL, stats_worker, &w) == 0);

    pthread_mutex_lock(&w.lock);
    while (!w.ready)
        pthread_cond_wait(&w.cv, &w.lock);
    pthread_mutex_unlock(&w.lock);

    wmm_stats_t st;
    CHECK(wmm_stats_snapshot(&st) == 0);

    /* 该线程的行:分配/释放字节与次数都精确(计数在分配/释放
       出口,与快慢路径无关)。大对象按页对齐计价:100000B 向上
       取整到 25 页 = 102400B */
    const uint64_t large_usable = ((100000 + WMM_PAGE - 1) / WMM_PAGE) * WMM_PAGE;
    const uint64_t expect = 50ull * 4096 + 4ull * large_usable;
    bool     found = false, main_found = false;
    unsigned rows  = 0;
    for (unsigned k = 0; k < 128; k++) {
        if (st.threads[k].tid == 0 && st.threads[k].alloc_bytes == 0)
            continue;                       /* 空槽(未用行) */
        rows++;
        if (st.threads[k].tid == w.tid) {
            found = true;
            CHECK(st.threads[k].alloc_bytes == expect);
            CHECK(st.threads[k].free_bytes == expect);
            CHECK(st.threads[k].alloc_cnt == 54);
        }
        if (st.threads[k].tid == g_tcache->tid)
            main_found = true;
    }
    CHECK(found);
    CHECK(main_found);
    CHECK(rows >= 2);

    pthread_mutex_lock(&w.lock);
    w.release = true;
    pthread_cond_broadcast(&w.cv);
    pthread_mutex_unlock(&w.lock);
    CHECK(pthread_join(th, NULL) == 0);

    /* 线程退出即注销:其行不再出现 */
    CHECK(wmm_stats_snapshot(&st) == 0);
    for (unsigned k = 0; k < 128; k++)
        CHECK(st.threads[k].tid != w.tid);
}

/* ---- ctl 接线:stats.snapshot / stats.per_class ---- */
static void check_ctl_stats(void) {
    wmm_stats_t st;
    CHECK(wmm_ctl("stats.snapshot", &st) == 0);
    CHECK(st.epoch != 0);

    CHECK(wmm_ctl("stats.snapshot", (wmm_stats_t *)NULL) == -1);
    CHECK(errno == EINVAL);

    /* per_class 只填直方图:epoch 与线程表原样不动 */
    wmm_stats_t pc;
    memset(&pc, 0, sizeof(pc));
    pc.epoch = 0xDEAD;
    CHECK(wmm_ctl("stats.per_class", &pc) == 0);
    CHECK(pc.epoch == 0xDEAD);
    for (szind_t i = 0; i < WMM_NBINS; i++) {
        CHECK(pc.cls[i].live == g_bins[i].live);
        CHECK(pc.cls[i].size == wmm_bin_info[i].reg_size);
    }
    CHECK(pc.cls[WMM_NBINS].size == 0);
    CHECK(pc.cls[WMM_NBINS].live == g_large.live);
}

int test_stats(void) {
    g_fail = 0;
    wmm_boot();

    /* 关缓存:计数全部走直通路径,直读全局与快照可比对(理由
       见文件头)。先 exit 丢掉前序组留在缓存里的对象,再建一个
       本组的空缓存并禁用 */
    wmm_thread_exit();
    wmm_thread_init();
    g_tcache->enabled = false;

    check_snapshot_accounting();
    check_high_water();
    check_active_purge();
    check_events();
    check_thread_row();
    check_ctl_stats();

    g_tcache->enabled = true;   /* 恢复默认状态 */

    if (g_fail == 0)
        printf("  all stats checks passed\n");
    return g_fail;
}
