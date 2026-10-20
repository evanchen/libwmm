/* test_wrap.c —— --wrap 接管冒烟测试(独立程序,自带 main)
 *
 * 覆盖(冒烟三连):
 *   1) 跑通:malloc/calloc/realloc/reallocarray/posix_memalign/
 *      aligned_alloc/free 七个入口全部经 __wrap_* 落到 libwmm,
 *      混合负载不崩;
 *   2) 统计非零且合理:增量分配字节 ≥ 请求总和、不超过请求总和的
 *      8 倍(尺寸类取整的理论最坏比),累计分配与释放严格对平;
 *   3) 退出零泄漏:全部释放并刷空本线程缓存后,live 字节与对象数
 *      回到工作负载之前的基线。
 *
 * 接管证据是白盒的:每次分配都检查指针落在 [g_heap_base, g_heap_end)
 * 窗口内。指针从窗口外来,就说明该入口没被链接期改写,直接失败。
 *
 * 为什么单独成程序:主测试二进制 build/test/run 故意不 wrap——
 * test_oracle 的 glibc 对照模式要在同一进程里调用真的 malloc 家族。
 * 本文件由 Makefile 用 -Wl,--wrap=... 单独链接成 build/test/wrap,
 * 运行前另做一次 nm 审计(见 test 目标)。
 *
 * 口径说明:本程序里只有显式调用走 libwmm;glibc 内部(stdio 缓冲
 * 等)的 malloc 仍归 glibc,不进入下面的统计。因此基线取在首条
 * printf 之后,让 stdio 的初始化发生在测量窗口之外。
 */
#include "wmm_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int    g_fail;
static size_t g_requested;   /* 成功分配请求字节总和,统计下界样本 */

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("  FAIL @%d: %s\n", __LINE__, #cond);               \
            g_fail++;                                                  \
        }                                                              \
    } while (0)

/* 固定种子,失败可复现 */
static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;

static uint32_t rng(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 32);
}

/* 指针是否来自 libwmm 的堆窗口:被 --wrap 接管的硬证据 */
static bool in_heap(const void *p) {
    return (uintptr_t)p >= (uintptr_t)g_heap_base &&
           (uintptr_t)p <  (uintptr_t)g_heap_end;
}

/* ---- malloc 基础契约:0 字节、空指针释放、对齐、首尾可写 ---- */
static void check_malloc(void) {
    free(NULL);   /* 空指针释放是合法空操作,不崩、不记账 */

    void *z = malloc(0);

    CHECK(z != NULL);
    CHECK(in_heap(z));
    CHECK(wmm_usable_size(z) >= 1);
    CHECK(((uintptr_t)z & 7u) == 0);
    free(z);

    /* 对齐契约:16B;唯一例外是 8B 最小类(请求 ≤ 8 只保证 8B,
       wmm.h 明文允许)。用 usable 判断豁免,不用请求值——判据与
       分配器内部选类一致 */
    for (size_t s = 1; s <= 300; s++) {
        unsigned char *p = malloc(s);

        CHECK(p != NULL);
        if (p == NULL)
            continue;
        CHECK(in_heap(p));
        size_t u = wmm_usable_size(p);
        CHECK(u >= s);
        CHECK(((uintptr_t)p & 15u) == 0 || u == 8);
        p[0] = 0xA5;
        p[s - 1] = 0x5A;
        g_requested += s;
        free(p);
    }
}

/* ---- calloc:溢出拒绝 + 复用对象必须重新清零 ---- */
static void check_calloc(void) {
    /* 两个调用故意要乘积溢出。实参经 volatile 读入,编译器就不能在
       编译期断定"必定溢出",否则测试自己会触发
       -Walloc-size-larger-than;运行期仍是同样的大数,照常走
       calloc 内部的溢出拒绝分支 */
    volatile size_t n = SIZE_MAX / 2;
    volatile size_t sz = 3;
    CHECK(calloc(n, sz) == NULL);
    n  = (size_t)1 << 40;
    sz = (size_t)1 << 30;
    CHECK(calloc(n, sz) == NULL);

    /* 零元请求按 glibc 语义仍返回可释放指针(size→1)。同样用 volatile
       挡住 -Walloc-size:常量 0 会让编译器判定"分配不足以放下 1 字节" */
    volatile size_t zero = 0;
    unsigned char *cz = calloc(zero, 16);
    CHECK(cz != NULL);
    if (cz != NULL) {
        CHECK(in_heap(cz));
        free(cz);
    }

    /* 先造一个脏块再释放;同类下一次分配按 LIFO 会弹出它。calloc
       若跳过清零就把脏数据泄漏给应用了 */
    unsigned char *dirty = malloc(64);
    CHECK(dirty != NULL);
    if (dirty != NULL) {
        memset(dirty, 0xAB, 64);
        g_requested += 64;
        free(dirty);
    }

    unsigned char *c = calloc(4, 16);
    CHECK(c != NULL);
    if (c != NULL) {
        CHECK(in_heap(c));
        CHECK(wmm_usable_size(c) >= 64);
        bool clean = true;
        for (int k = 0; k < 64; k++)
            clean = clean && (c[k] == 0);
        CHECK(clean);
        g_requested += 64;
        free(c);
    }
}

/* ---- realloc:NULL/收缩/搬移/零长四特例 ---- */
static void check_realloc(void) {
    unsigned char *p = realloc(NULL, 100);

    CHECK(p != NULL);
    if (p == NULL)
        return;
    CHECK(in_heap(p));
    g_requested += 100;
    for (int k = 0; k < 100; k++)
        p[k] = (unsigned char)k;

    /* 收缩原地:指针不变 */
    void *q = realloc(p, 50);
    CHECK(q == p);

    /* 扩容搬移:前 100 字节原样到达 */
    q = realloc(p, 5000);
    CHECK(q != NULL);
    CHECK(q != p);   /* 112B 类 → 5120B 类,必搬移 */
    if (q != NULL) {
        CHECK(in_heap(q));
        CHECK(wmm_usable_size(q) >= 5000);
        bool same = true;
        for (int k = 0; k < 100; k++)
            same = same && (((unsigned char *)q)[k] == (unsigned char)k);
        CHECK(same);
        g_requested += 5000;
        free(q);
    }

    /* n==0 等价 free 并返回 NULL */
    p = malloc(100);
    CHECK(p != NULL);
    if (p != NULL) {
        g_requested += 100;
        CHECK(realloc(p, 0) == NULL);
    }

    /* reallocarray:溢出拒绝且旧块有效;正常乘积与 realloc 等价。
       huge 经 volatile 读入,避免编译器在编译期判定必然溢出 */
    volatile size_t huge = SIZE_MAX / 2;
    unsigned char *ra = malloc(32);
    CHECK(ra != NULL);
    if (ra != NULL) {
        CHECK(in_heap(ra));
        g_requested += 32;

        errno = 0;

        /* 溢出失败后旧块必须仍可读、可再次交给 reallocarray——这正是
           glibc 的契约,也是本段要验证的东西。GCC 12 的
           -Wuse-after-free 只看"指针是否又出现在 reallocarray 之后",
           不区分返回 NULL 的失败路径,因此在这里显式关掉;判定本身
           仍由下面的断言完成,没有放宽任何检查。 */
#if defined(__GNUC__) && __GNUC__ >= 12
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wuse-after-free"
#endif
        CHECK(reallocarray(ra, huge, 3) == NULL);
        CHECK(errno == ENOMEM);
        CHECK(in_heap(ra));   /* 失败后旧块必须仍可用 */

        ra = reallocarray(ra, 4, 100);
#if defined(__GNUC__) && __GNUC__ >= 12
# pragma GCC diagnostic pop
#endif
        CHECK(ra != NULL);
        if (ra != NULL) {
            CHECK(in_heap(ra));
            CHECK(wmm_usable_size(ra) >= 400);
            g_requested += 400;
            free(ra);
        }
    }
}

/* ---- 对齐分配:错误码 + 对齐 + 可用量 ---- */
static void check_aligned(void) {
    void *m = (void *)0x1;

    CHECK(posix_memalign(&m, 0, 100) == EINVAL && m == NULL);
    CHECK(posix_memalign(&m, 3, 100) == EINVAL && m == NULL);
    CHECK(posix_memalign(&m, 4, 100) == EINVAL && m == NULL);
    CHECK(posix_memalign(&m, 24, 100) == EINVAL && m == NULL);

    errno = 0;
    CHECK(aligned_alloc(0, 100) == NULL && errno == EINVAL);
    errno = 0;
    CHECK(aligned_alloc(12, 100) == NULL && errno == EINVAL);

    unsigned char *a = aligned_alloc(64, 128);
    CHECK(a != NULL);
    if (a != NULL) {
        CHECK(in_heap(a));
        CHECK(((uintptr_t)a & 63u) == 0);
        CHECK(wmm_usable_size(a) >= 128);
        a[0] = 0xA5;
        a[127] = 0x5A;
        g_requested += 128;
        free(a);
    }

    CHECK(posix_memalign(&m, 4096, 5000) == 0);
    CHECK(m != NULL);
    if (m != NULL) {
        CHECK(in_heap(m));
        CHECK(((uintptr_t)m & 4095u) == 0);
        CHECK(wmm_usable_size(m) >= 5000);
        g_requested += 5000;
        free(m);
    }
}

/* ---- 混合风暴:随机槽位 × 随机入口,覆盖缓存的弹压与搬移 ---- */
static void check_mixed_storm(void) {
    enum { SLOTS = 64, OPS = 4000 };
    static void *slot[SLOTS];
    static const size_t sizes[] = {
        8, 16, 24, 64, 100, 300, 1000, 4096, 5000, 16384, 20000, 100000
    };
    const size_t nsizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int op = 0; op < OPS; op++) {
        unsigned s = rng() % SLOTS;

        if (slot[s] == NULL) {
            /* 打散到随机槽位,而不是扫描空位:槽位占用率才会在
               中间均衡,缓存水位也才有涨有落 */
            size_t   sz  = sizes[rng() % nsizes];
            size_t   req = sz;
            void    *p;
            unsigned kind = rng() & 7u;

            if (kind == 0) {
                p = calloc(1, sz);
            } else if (kind == 1) {
                req = (sz + 63u) & ~(size_t)63u;
                p = aligned_alloc(64, req);
            } else {
                p = malloc(sz);
            }

            CHECK(p != NULL);
            if (p == NULL)
                continue;
            CHECK(in_heap(p));
            size_t u = wmm_usable_size(p);
            CHECK(u >= req);
            CHECK(((uintptr_t)p & 15u) == 0 || u == 8);
            ((unsigned char *)p)[0] = 0xA5;
            ((unsigned char *)p)[req - 1] = 0x5A;
            g_requested += req;
            slot[s] = p;
        } else if ((rng() & 3u) != 0) {
            unsigned char *old   = slot[s];
            unsigned char  first = old[0];
            size_t         old_usable = wmm_usable_size(old);
            size_t         ns    = sizes[rng() % nsizes];
            unsigned char *p     = realloc(old, ns);

            CHECK(p != NULL);
            if (p == NULL)
                continue;
            CHECK(in_heap(p));
            CHECK(wmm_usable_size(p) >= ns);
            CHECK(p[0] == first);   /* 搬移后内容必须原样到达 */
            /* 原地满足(指针与可用量都没变)不会产生新分配,不能
               计入请求下界;搬移或换类才是真的又分配了一次 */
            if (p != old || wmm_usable_size(p) != old_usable)
                g_requested += ns;
            slot[s] = p;
        } else {
            free(slot[s]);
            slot[s] = NULL;
        }
    }

    for (unsigned s = 0; s < SLOTS; s++)
        if (slot[s] != NULL) {
            free(slot[s]);
            slot[s] = NULL;
        }
}

/* ---- 冒烟三连:统计非零合理 + 零泄漏 ---- */
static void check_stats_and_leaks(const wmm_stats_t *st0,
                                  const wmm_stats_t *st1) {
    uint64_t alloc_d = st1->allocated_bytes - st0->allocated_bytes;
    uint64_t free_d  = st1->deallocated_bytes - st0->deallocated_bytes;

    CHECK(alloc_d > 0);
    CHECK(free_d > 0);
    CHECK(alloc_d >= (uint64_t)g_requested);

    /* 上界只做量级检查:1 字节请求落进 8 字节类是最坏取整(8 倍),
       再加 4KB 零头。超过这个量级说明统计把页或缓存存量重复计了 */
    CHECK(alloc_d <= (uint64_t)g_requested * 8u + 4096u);

    /* 对象全部归还后,累计分配与累计释放必须严格对平 */
    CHECK(alloc_d == free_d);

    /* live 回到基线:应用持有与线程缓存都空了 */
    CHECK(st1->live_bytes == st0->live_bytes);
    CHECK(st1->live_objects == st0->live_objects);
}

int main(void) {
    wmm_boot();
    printf("wrap smoke: malloc family through ld --wrap\n");

    wmm_stats_t st0;
    if (wmm_stats_snapshot(&st0) != 0) {
        printf("  FAIL: initial stats snapshot\n");
        return 1;
    }

    check_malloc();
    check_calloc();
    check_realloc();
    check_aligned();
    check_mixed_storm();

    /* 应用持有的对象已全部释放;缓存里的存量刷回 bin,live 才归零 */
    wmm_tcache_flush_all();

    wmm_stats_t st1;
    if (wmm_stats_snapshot(&st1) != 0) {
        printf("  FAIL: final stats snapshot\n");
        return 1;
    }

    check_stats_and_leaks(&st0, &st1);

    printf("  alloc delta = %llu bytes, free delta = %llu bytes, "
           "live = %llu bytes\n",
           (unsigned long long)(st1.allocated_bytes - st0.allocated_bytes),
           (unsigned long long)(st1.deallocated_bytes - st0.deallocated_bytes),
           (unsigned long long)st1.live_bytes);
    printf("  requests = %zu bytes, all pointers inside heap window\n",
           g_requested);

    if (g_fail == 0) {
        printf("  all wrap smoke checks passed\n");
        return 0;
    }
    printf("  WRAP SMOKE FAIL(%d 项断言失败)\n", g_fail);
    return 1;
}
