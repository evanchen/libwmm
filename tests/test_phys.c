/* test_phys.c —— M6 物理层用例(白盒:直接检查地址序链与计数)
 *
 * 覆盖(tasks.md M6 自测清单):
 *   PHY1 合并:奇偶交替释放 → 独立空闲块;全释放 → 融成单块;
 *         复用取最低地址且 g_heap_cur 不前进(零新映射);
 *   分裂:大块复用时从低段切,余段按地址留在链上,计数守恒;
 *   跨路径吸收:slab 退休的页被大对象分配复用(碎片不丢);
 *   PHY3:大对象每页页表登记同一 extent,释放后全页 raw==0;
 *   结构不变量:地址序链严格递增、不重叠、双向指针互指、
 *         g_dirty_pages == Σ FREE 页数(每个检查点跑一遍);
 *   重放:混合压力序列跑两遍(同种子同序列),第二遍 g_heap_cur
 *         一页不前进 = 全部命中复用,碎片被合并吸收;
 *   purge:1GB 脏页同步回收 → dirty 归零、页数守恒地转
 *         RETAINED、RSS 实测回落、RETAINED 复用零新映射;
 *   跨状态合并:RETAINED 邻居并进 DIRTY 块,整块重新按 DIRTY 记账;
 *   后台线程:未启动不动、启用后 2 秒内按阈值回收、停后不再动,
 *         重复启停幂等。
 *
 * 确定性策略:api/slab 用例留下的空闲 extent 会让 first-fit 落点
 * 不可预测,所以每个子测试先 drain_free_pool——每次按"当前最大
 * 空闲块的精确大小"分配,直到链上无空闲。此后全部 extent 都在
 * INUSE,新分配必是窗口 bump(地址连续可预知),断言才能写死。
 * drain 块从不写入:mmap 匿名页不碰就驻留在共享零页上,抽干不耗
 * 物理内存。
 *
 * tcache 开关:本组的断言全部依赖"释放即回 slab、全空即退休"的
 * 即时性(curslabs 计数、dirty 页数、重放的零新映射)。线程缓存会
 * 把释放先压在栈上,对象仍计存活、slab 不退休,确定性世界就没了。
 * 因此本组开始时建一个空缓存并整体关掉 enabled,组尾恢复——本组
 * 测的是物理层与 bin 状态机,缓存行为归 oracle/stress/tcache 三组。
 */
#include "wmm_internal.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

static int g_fail;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("  FAIL @%d: %s\n", __LINE__, #cond);               \
            g_fail++;                                                  \
        }                                                              \
    } while (0)

/* 固定种子(黄金分割的十六进制前缀),失败可复现 */
static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;

static uint32_t rng(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 32);
}

/* ---- 结构不变量:地址序链 + dirty 计数 ----
 * 单线程测试直接读全局(无并发写者)。 */
static size_t ar_check(void) {
    size_t        dirty_sum = 0;
    wmm_extent_t *prev      = NULL;

    for (wmm_extent_t *e = g_ar_head; e != NULL; e = e->ar_next) {
        CHECK(e->state >= WMM_EXT_INUSE && e->state <= WMM_EXT_FREE_RETAINED);
        CHECK(e->npages >= 1);
        if (prev != NULL) {
            CHECK((uintptr_t)prev->addr < (uintptr_t)e->addr);   /* 严格递增 */
            CHECK((char *)prev->addr + prev->npages * WMM_PAGE
                      <= (char *)e->addr);                       /* 不重叠 */
            CHECK(prev->ar_next == e && e->ar_prev == prev);     /* 双向互指 */
        }
        if (e->state == WMM_EXT_FREE_DIRTY)
            dirty_sum += e->npages;
        prev = e;
    }
    CHECK(g_dirty_pages == dirty_sum);
    return dirty_sum;
}

/* ---- 抽干空闲池:全 INUSE 的确定性世界 ---- */
static void *drain_blocks[4096];
static size_t ndrain;

static void drain_free_pool(void) {
    while (ndrain < 4096) {
        /* 找当前最大空闲块的页数;精确按该大小分配 → 整块消费、
           无余段,循环必然终止(每轮消灭一个空闲块) */
        size_t max_p = 0;
        for (wmm_extent_t *e = g_ar_head; e != NULL; e = e->ar_next)
            if (e->state != WMM_EXT_INUSE && e->npages > max_p)
                max_p = e->npages;
        if (max_p == 0)
            break;

        drain_blocks[ndrain++] = wmm_alloc(max_p * WMM_PAGE);
        CHECK(drain_blocks[ndrain - 1] != NULL);
    }
    CHECK(ndrain < 4096);        /* 退出必须因池干,而非触上限 */
    CHECK(g_dirty_pages == 0);   /* 全 INUSE:脏页必然归零 */
    ar_check();
}

/* ---- PHY3:大对象页表登记与清零 ---- */
static void check_large_pte(void) {
    drain_free_pool();

    size_t npages = 25;                       /* 100000B → 25 页 */
    unsigned char *L = wmm_alloc(100000);
    CHECK(L != NULL);
    CHECK(wmm_usable_size(L) == npages * WMM_PAGE);

    void *ext;
    CHECK(wmm_pte_is_extent(L, &ext));
    wmm_extent_t *e = (wmm_extent_t *)ext;
    CHECK(e->npages == npages && e->state == WMM_EXT_INUSE && e->slab == NULL);

    /* 每一页登记同一 extent;注意中间页与首页都可反查 */
    for (size_t pg = 0; pg < npages; pg++) {
        void *x;
        CHECK(wmm_pte_is_extent(L + pg * WMM_PAGE, &x));
        CHECK(x == ext);
    }

    wmm_free(L);
    for (size_t pg = 0; pg < npages; pg++)
        CHECK(wmm_pte_get(L + pg * WMM_PAGE).raw == 0);
}

/* ---- PHY1:交替释放的合并与最低地址复用 ---- */
static void check_merge(void) {
    drain_free_pool();

    /* 8 个相邻 64KB(16 页)大对象:drain 后全是新 bump,地址连续 */
    unsigned char *blk[8];
    for (int k = 0; k < 8; k++) {
        blk[k] = wmm_alloc(64 << 10);
        CHECK(blk[k] != NULL);
        if (k > 0)
            CHECK(blk[k] == blk[k - 1] + (64 << 10));   /* 相邻性 */
    }

    /* 奇数位释放:互不相邻,应得 4 个独立空闲块(合并不发生) */
    for (int k = 1; k < 8; k += 2)
        wmm_free(blk[k]);
    size_t dirty = ar_check();
    CHECK(dirty == 4 * 16);
    size_t nfree = 0;
    for (wmm_extent_t *e = g_ar_head; e != NULL; e = e->ar_next)
        if (e->state == WMM_EXT_FREE_DIRTY)
            nfree++;
    CHECK(nfree == 4);

    /* 偶数位再释放:全部相接 → 融成单个 128 页块 */
    for (int k = 0; k < 8; k += 2)
        wmm_free(blk[k]);
    dirty = ar_check();
    CHECK(dirty == 8 * 16);
    nfree = 0;
    wmm_extent_t *only = NULL;
    for (wmm_extent_t *e = g_ar_head; e != NULL; e = e->ar_next)
        if (e->state == WMM_EXT_FREE_DIRTY) {
            nfree++;
            only = e;
        }
    CHECK(nfree == 1);
    CHECK(only != NULL && only->addr == blk[0] && only->npages == 8 * 16);

    /* 复用取最低地址,且零新映射:记录复用分配前的 bump 指针,
       分配后不变 = 没找内核要过页 */
    void *cur_pre = g_heap_cur;
    void *p = wmm_alloc(64 << 10);
    CHECK(p == blk[0]);
    CHECK(g_heap_cur == cur_pre);
    wmm_free(p);
}

/* ---- 分裂:复用块从低段切,余段留在链上 ---- */
static void check_split(void) {
    drain_free_pool();

    unsigned char *A = wmm_alloc(5 * WMM_PAGE);   /* 20480B:大对象 5 页 */
    unsigned char *B = wmm_alloc(4 * WMM_PAGE);   /* 16384B:相邻护栏 */
    CHECK(A != NULL && B != NULL && B == A + 5 * WMM_PAGE);

    wmm_free(A);                    /* 5 页空闲,是链上唯一空闲块 */
    CHECK(g_dirty_pages == 5);

    /* 4 页请求:first-fit 命中它,低段切出,余 1 页留在原位 */
    void *cur_pre = g_heap_cur;
    unsigned char *C = wmm_alloc(4 * WMM_PAGE);
    CHECK(C == A);                  /* 低段交付 */
    CHECK(g_dirty_pages == 1);
    CHECK(g_heap_cur == cur_pre);   /* 零新映射 */

    wmm_extent_t *rem = NULL;
    for (wmm_extent_t *e = g_ar_head; e != NULL; e = e->ar_next)
        if (e->state == WMM_EXT_FREE_DIRTY) {
            CHECK(rem == NULL);     /* 恰一个空闲块 */
            rem = e;
        }
    CHECK(rem != NULL);
    CHECK(rem->addr == A + 4 * WMM_PAGE && rem->npages == 1);

    /* 全放:C(4页)+余段(1页)+B(4页)相邻 → 融成 9 页单块 */
    wmm_free(C);
    wmm_free(B);
    CHECK(g_dirty_pages == 9);
    size_t nfree = 0;
    for (wmm_extent_t *e = g_ar_head; e != NULL; e = e->ar_next)
        if (e->state == WMM_EXT_FREE_DIRTY)
            nfree++;
    CHECK(nfree == 1);
    ar_check();
}

/* ---- 跨路径吸收:slab 退休的页被大对象复用 ---- */
static void check_slab_absorb(void) {
    drain_free_pool();

    /* 600 个 48B(类 3,每 slab 3 页 256 对象)→ 恰 3 个新 slab。
       相邻性依据是"drain 后链上无空闲 → extent_get 只能窗口
       bump → 三块页连续",与 bin3 的历史无关。bin 计数带着 api
       组的历史(那些 slab 早已退休),断言一律用增量,不要求
       全局史为零 */
    uint64_t curslabs0 = g_bins[3].curslabs;
    unsigned char *objs[600];
    for (int k = 0; k < 600; k++) {
        objs[k] = wmm_alloc(48);
        CHECK(objs[k] != NULL);
    }
    CHECK(g_bins[3].curslabs - curslabs0 == 3);   /* 恰 3 个新 slab 在管 */

    /* 第一个 slab 的基址:页表反查,不靠地址推算 */
    wmm_slab_t *sl = NULL;
    CHECK(wmm_pte_is_slab(objs[0], &sl));
    void *slab0 = sl->addr;

    /* 全部释放:3 个 slab 依次退休,页融合成一个 9 页空闲块 */
    for (int k = 0; k < 600; k++)
        wmm_free(objs[k]);
    CHECK(g_bins[3].curslabs == curslabs0);   /* 全部退休,回到起点 */
    CHECK(g_dirty_pages == 9);   /* drain 清零后本节恰退休 3 slab × 3 页;
                                    释放之间无任何分配,计数无他源 */

    /* 大对象恰好要 9 页:必须复用 slab 腾出的页(零新映射) */
    void *cur_pre = g_heap_cur;
    void *big = wmm_alloc(9 * WMM_PAGE);
    CHECK(big == slab0);
    CHECK(g_heap_cur == cur_pre);
    CHECK(wmm_usable_size(big) == 9 * WMM_PAGE);
    wmm_free(big);
}

/* ---- 混合压力 ×2 重放:第二遍零新映射 = 碎片被吸收 ---- */
static void mixed_replay_pass(void) {
    enum { SLOTS = 256, OPS = 3000 };
    static void *slot[SLOTS];

    memset(slot, 0, sizeof(slot));
    for (int op = 0; op < OPS; op++) {
        uint32_t s = rng() % SLOTS;
        if (slot[s] != NULL && (rng() & 1) == 0) {
            wmm_free(slot[s]);
            slot[s] = NULL;
        } else if (slot[s] == NULL) {
            size_t sz = (rng() % 10) < 7
                          ? 1 + rng() % 14336                 /* 小对象 70% */
                          : 16384 + rng() % (512u << 10);     /* 大对象 30% */
            slot[s] = wmm_alloc(sz);
            CHECK(slot[s] != NULL);
        }
    }
    for (int s = 0; s < SLOTS; s++)
        if (slot[s] != NULL)
            wmm_free(slot[s]);
}

static void check_replay(void) {
    /* 释放 drain 块:整个已映射区域变空闲并融合,两遍起点一致 */
    for (size_t k = 0; k < ndrain; k++)
        wmm_free(drain_blocks[k]);
    ndrain = 0;
    ar_check();

    g_rng = 0x9E3779B97F4A7C15ULL;      /* 同种子 = 同序列 */
    mixed_replay_pass();
    void *cur1 = g_heap_cur;
    ar_check();                          /* 全释放后:无 INUSE 残留 */
    for (wmm_extent_t *e = g_ar_head; e != NULL; e = e->ar_next)
        CHECK(e->state != WMM_EXT_INUSE);

    g_rng = 0x9E3779B97F4A7C15ULL;      /* 第二遍:同一条序列重放 */
    mixed_replay_pass();
    CHECK(g_heap_cur == cur1);           /* 一页不前进:全部命中复用 */
    ar_check();
}

/* ==================== purge 与后台线程 ==================== */

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

/* RSS(KB):statm 的 resident 字段以页计。返回 0 = 读失败,调用点的
 * 断言会显式失败,而不是让 RSS 检查静默失去意义。 */
static size_t rss_kb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (f == NULL)
        return 0;
    long total = 0, resident = 0;
    int  n     = fscanf(f, "%ld %ld", &total, &resident);
    fclose(f);
    if (n != 2 || resident < 0)
        return 0;
    return (size_t)resident * (WMM_PAGE / 1024);
}

/* 启用后台线程后 g_dirty_pages 有第二个写者,白盒读数要拿锁 */
static size_t dirty_pages_locked(void) {
    pthread_mutex_lock(&g_ext_lock);
    size_t n = g_dirty_pages;
    pthread_mutex_unlock(&g_ext_lock);
    return n;
}

/* ---- PHY2:同步 purge 把脏页还给内核 ----
 * 三件事分别证明:账本(dirty 归零、页数守恒转 RETAINED)、物理
 * 事实(RSS 实测回落,不是只改状态),以及回收后的页仍可零系统
 * 调用复用。NB×4MB 这个量级取的是"远大于 dirty_max",断言只依赖
 * 这一点;测试机内存紧张时改 NB 即可。 */
static void check_purge(void) {
    drain_free_pool();

    enum { NB = 256 };
    static unsigned char *blk[NB];
    const size_t BSZ = 4u << 20;              /* 256 × 4MB = 1GB */

    for (int k = 0; k < NB; k++) {
        blk[k] = wmm_alloc(BSZ);
        CHECK(blk[k] != NULL);
        if (k > 0)
            CHECK(blk[k] == blk[k - 1] + BSZ);   /* drain 后只能窗口 bump */
    }
    /* 触碰每一页:没写过的匿名页驻留在共享零页上,没有 RSS 可回收,
       RSS 断言就失去意义 */
    for (int k = 0; k < NB; k++)
        memset(blk[k], 0xA5, BSZ);

    for (int k = 0; k < NB; k++)
        wmm_free(blk[k]);

    size_t dirty0 = g_dirty_pages;
    CHECK(dirty0 * WMM_PAGE > g_opt.dirty_max);

    size_t rss0 = rss_kb();
    CHECK(rss0 != 0);

    CHECK(wmm_ctl("purge") == 0);

    CHECK(g_dirty_pages == 0);
    size_t retained = 0;
    for (wmm_extent_t *e = g_ar_head; e != NULL; e = e->ar_next) {
        CHECK(e->state != WMM_EXT_FREE_DIRTY);   /* 全部离开 DIRTY */
        if (e->state == WMM_EXT_FREE_RETAINED)
            retained += e->npages;
    }
    CHECK(retained == dirty0);    /* 页数守恒:一张不少地转 RETAINED */
    ar_check();

    /* RSS 至少回落一半(512MB):进程其余活动会带来噪声,留足余量 */
    size_t rss1 = rss_kb();
    CHECK(rss1 != 0);
    CHECK(rss1 + (512u << 10) <= rss0);

    /* RETAINED 复用:整块 1GB 恰好命中最低地址,零新映射;RETAINED
       本来就不在 dirty 账上,复用这一步也不得改变计数 */
    void *cur_pre = g_heap_cur;
    void *again = wmm_alloc((size_t)NB * BSZ);
    CHECK(again == blk[0]);
    CHECK(g_heap_cur == cur_pre);
    CHECK(g_dirty_pages == 0);
    wmm_free(again);
    CHECK(g_dirty_pages == dirty0);   /* 重新释放 = 重新计入脏页 */
    ar_check();

    /* madvise 方式开关:非法值 EINVAL,合法值只切模式。MADV_FREE 的
       物理效果(立即回落与否)由内核版本决定,不在测试里钉;切完
       恢复默认,后续用例的 RSS 断言依赖 DONTNEED */
    CHECK(wmm_ctl("config.madvise_mode", -1) == -1);
    CHECK(errno == EINVAL);
    CHECK(wmm_ctl("config.madvise_mode", MADV_FREE) == 0);
    CHECK(g_opt.madvise_mode == MADV_FREE);
    CHECK(wmm_ctl("config.madvise_mode", MADV_DONTNEED) == 0);
    CHECK(g_opt.madvise_mode == MADV_DONTNEED);
}

/* ---- 跨状态合并:RETAINED 邻居并进 DIRTY 块必须重新记账 ----
 * 状态标签与 dirty 计数必须同步;漏记的页既让页数守恒断言失效,
 * 也会永远逃过后续 purge。 */
static void check_merge_across_states(void) {
    drain_free_pool();

    unsigned char *A = wmm_alloc(4 * WMM_PAGE);
    unsigned char *B = wmm_alloc(4 * WMM_PAGE);
    unsigned char *C = wmm_alloc(4 * WMM_PAGE);
    CHECK(A != NULL && B != NULL && C != NULL);
    CHECK(B == A + 4 * WMM_PAGE && C == B + 4 * WMM_PAGE);

    wmm_free(A);
    wmm_free(C);
    CHECK(g_dirty_pages == 8);
    CHECK(wmm_ctl("purge") == 0);
    CHECK(g_dirty_pages == 0);        /* A、C 转 RETAINED,B 仍 INUSE */

    /* B 释放:两侧都是 RETAINED,合并后整块必须改标 DIRTY 且
       12 页全部回到 dirty 计数 */
    wmm_free(B);
    CHECK(g_dirty_pages == 12);

    size_t        nfree = 0;
    wmm_extent_t *only  = NULL;
    for (wmm_extent_t *e = g_ar_head; e != NULL; e = e->ar_next)
        if (e->state != WMM_EXT_INUSE) {
            nfree++;
            only = e;
        }
    CHECK(nfree == 1);
    CHECK(only != NULL && only->addr == A && only->npages == 12);
    CHECK(only != NULL && only->state == WMM_EXT_FREE_DIRTY);
    ar_check();

    /* 曾 RETAINED 的 8 页被重复 madvise 一次(空操作),整块转 RETAINED */
    CHECK(wmm_ctl("purge") == 0);
    CHECK(g_dirty_pages == 0);
    ar_check();
}

/* ---- 后台线程:启停幂等 + 阈值触发 + 停后不再动 ----
 * 时间预算:两段"等一个巡检周期"各 1.3s,回收轮询上限 2s。
 * 128MB 足以越过默认 8MB 阈值,不必像 PHY2 上 1GB。 */
static void check_bg_lifecycle(void) {
    drain_free_pool();

    enum { NB = 32 };
    static unsigned char *blk[NB];
    const size_t BSZ = 4u << 20;              /* 32 × 4MB = 128MB */

    /* ---- 停用状态:脏页攒着不动 ---- */
    for (int k = 0; k < NB; k++) {
        blk[k] = wmm_alloc(BSZ);
        CHECK(blk[k] != NULL);
        memset(blk[k], 0x5A, BSZ);
    }
    for (int k = 0; k < NB; k++)
        wmm_free(blk[k]);

    size_t dirty0 = g_dirty_pages;
    CHECK(dirty0 * WMM_PAGE > g_opt.dirty_max);

    sleep_ms(1300);                        /* > 一个巡检周期 */
    CHECK(dirty_pages_locked() == dirty0); /* 没启动就不许自动回收 */

    CHECK(wmm_ctl("bg.disable") == 0);     /* 未运行:停 = 无操作 */

    /* ---- 启用:2 秒内按阈值回收 ---- */
    size_t rss0 = rss_kb();
    CHECK(rss0 != 0);
    CHECK(wmm_ctl("bg.enable") == 0);
    CHECK(wmm_ctl("bg.enable") == 0);      /* 重复启动幂等 */

    int polls = 0;
    while (dirty_pages_locked() != 0 && polls < 40) {
        sleep_ms(50);
        polls++;
    }
    CHECK(polls < 40);                     /* 2 秒内必须回收完 */
    CHECK(dirty_pages_locked() == 0);

    size_t rss1 = rss_kb();
    CHECK(rss1 != 0);
    CHECK(rss1 + (32u << 10) <= rss0);     /* 128MB 脏页至少回落 32MB */

    /* ---- 停止:返回即 join 完成;再停幂等 ---- */
    CHECK(wmm_ctl("bg.disable") == 0);
    CHECK(wmm_ctl("bg.disable") == 0);

    /* 停后不再自动回收:再攒 128MB,等一个周期应原样 */
    for (int k = 0; k < NB; k++) {
        blk[k] = wmm_alloc(BSZ);
        CHECK(blk[k] != NULL);
    }
    for (int k = 0; k < NB; k++)
        wmm_free(blk[k]);

    size_t dirty1 = dirty_pages_locked();
    CHECK(dirty1 * WMM_PAGE > g_opt.dirty_max);
    sleep_ms(1300);
    CHECK(dirty_pages_locked() == dirty1);

    /* 收尾:再启一次清掉脏页并停线程,后续组看到默认(未运行)状态 */
    CHECK(wmm_ctl("bg.enable") == 0);
    polls = 0;
    while (dirty_pages_locked() != 0 && polls < 40) {
        sleep_ms(50);
        polls++;
    }
    CHECK(dirty_pages_locked() == 0);
    CHECK(wmm_ctl("bg.disable") == 0);

    ar_check();
}

int test_phys(void) {
    g_fail = 0;
    wmm_boot();

    /* 关缓存,理由见文件头。先 exit 丢掉前序组可能留在缓存里的
       对象,再建一个本组的空缓存并禁用——wmm_alloc/free 回到
       M7 的直通路径 */
    wmm_thread_exit();
    wmm_thread_init();
    g_tcache->enabled = false;

    ar_check();            /* api 用例遗留:结构应已完好 */
    check_large_pte();
    check_merge();
    check_split();
    check_slab_absorb();
    check_replay();
    check_purge();
    check_merge_across_states();
    check_bg_lifecycle();

    g_tcache->enabled = true;   /* 后续组(oracle/stress)要用缓存 */

    if (g_fail == 0)
        printf("  all phys checks passed\n");
    return g_fail;
}
