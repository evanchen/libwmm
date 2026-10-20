/* test_prof.c —— M13 调用点采样用例
 *
 * 覆盖:
 *   开关语义:开启/关闭;关闭时采样节拍不推进(环形缓冲 head 不动),
 *        也不产生新的调用点行;
 *   采样节拍:每 1000 次成功分配一个样本、每个样本权重 1000,因此
 *        a 次分配产生的估算 count 恰好是 a(测试里没有别的分配源);
 *   调用点折叠:同一函数采多次只产生一行、count 累加;不同调用点
 *        分成不同行且 pc 互不相同;
 *   TOP 排序:估算字节大的行排在前面;
 *   环绕圈:同一调用点连采 300 次(30 万次分配),环容量 256,合并
 *        后 count 恰好是 256 × 1000——最旧的 44 个样本按设计丢弃;
 *   线程退出:工作线程分配 12000 次(12 个样本),析构先归账再
 *        munmap,join 之后 TOP 表里能看到完整的 12000;
 *   ctl 契约:prof.top 的 k/out 参数校验、输出尾部清零。
 *
 * 识别策略:测试里每个"吃内存函数"只用一个尺寸类规格(128/256/
 * 512/1024/2048/1MB,互不重叠),所以聚合行满足
 *     bytes == count × size
 * 时就能唯一认出它。为什么不用函数地址断言:采样记录的是 wmm_alloc
 * 返回后的那条指令地址,位于函数体中间,用 &func 无法精确框定
 * (ASan 插桩还会改变函数布局),按地址框定比契约更严。契约只承诺
 * pc 是用户调用点,因此这里断言 pc 非零、不同调用点的 pc 不同,
 * 并把表打印出来供 addr2line 人工对名。
 *
 * 确定性:本组开头停掉后台线程,合并只由 wmm_prof_top 同步触发,
 * 否则后台每秒一次的合并会让"环绕圈丢多少样本"不可预期。
 */

#include "wmm_internal.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
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

/* 各调用点的工作量(分配次数,最终等于估算 count) */
#define PROF_N_A 4000u      /* 1MB 大对象:4 个样本 */
#define PROF_N_B 3000u      /* 256B:3 个样本 */
#define PROF_N_C 12000u     /* 512B,工作线程:12 个样本 */
#define PROF_N_D 2000u      /* 1024B:2 个样本 */
#define PROF_N_E 5000u      /* 2048B,采样关闭:0 个样本 */
#define PROF_N_W 300000u    /* 128B:300 个样本,超出环容量 */

/* ---- 吃内存函数:每个函数一个专属尺寸,noinline 保证调用点稳定 ---- */

static __attribute__((noinline)) void eat_a(void) {
    for (unsigned i = 0; i < PROF_N_A; i++) {
        void *p = wmm_alloc(1u << 20);
        if (p == NULL)
            return;
        wmm_free(p);
    }
}

static __attribute__((noinline)) void eat_b(void) {
    for (unsigned i = 0; i < PROF_N_B; i++) {
        void *p = wmm_alloc(256);
        if (p == NULL)
            return;
        wmm_free(p);
    }
}

static __attribute__((noinline)) void eat_d(void) {
    for (unsigned i = 0; i < PROF_N_D; i++) {
        void *p = wmm_alloc(1024);
        if (p == NULL)
            return;
        wmm_free(p);
    }
}

static __attribute__((noinline)) void eat_e(void) {
    for (unsigned i = 0; i < PROF_N_E; i++) {
        void *p = wmm_alloc(2048);
        if (p == NULL)
            return;
        wmm_free(p);
    }
}

static __attribute__((noinline)) void eat_w(void) {
    for (unsigned i = 0; i < PROF_N_W; i++) {
        void *p = wmm_alloc(128);
        if (p == NULL)
            return;
        wmm_free(p);
    }
}

static void *worker_thread(void *arg) {
    int *ok = arg;
    *ok = 1;
    for (unsigned i = 0; i < PROF_N_C; i++) {
        void *p = wmm_alloc(512);
        if (p == NULL) {
            *ok = 0;
            break;
        }
        wmm_free(p);
    }
    return NULL;
}

/* 在当前 TOP 表里找"只分配 size 的那个调用点",返回其下标并拷出;
   找不到返回 -1。TOP 表最多 256 行,本测试的调用点远少于此。 */
static int find_entry(wmm_prof_entry_t *out, uint64_t size) {
    static wmm_prof_entry_t tab[WMM_PROF_TOP_MAX];
    int n = wmm_ctl("prof.top", (int)WMM_PROF_TOP_MAX, tab);
    if (n < 0)
        return -1;

    for (int i = 0; i < n; i++) {
        if (tab[i].count != 0 && tab[i].bytes == tab[i].count * size) {
            if (out != NULL)
                *out = tab[i];
            return i;
        }
    }
    return -1;
}

static uint64_t entry_count(uint64_t size) {
    wmm_prof_entry_t e;
    memset(&e, 0, sizeof e);
    return find_entry(&e, size) >= 0 ? e.count : 0;
}

int test_prof(void) {
    g_fail = 0;
    wmm_boot();

    /* 本组要精确控制"环里积了多少样本",不能让后台线程插进来合并 */
    CHECK(wmm_ctl("bg.disable") == 0);

    wmm_thread_init();
    CHECK(g_tcache != NULL);
    if (g_tcache == NULL) {
        printf("  thread cache unavailable, cannot test sampling\n");
        return g_fail;
    }

    /* ---- ctl 参数契约 ---- */
    static wmm_prof_entry_t tab[WMM_PROF_TOP_MAX];
    CHECK(wmm_ctl("prof.top", 0, tab) == -1);
    CHECK(errno == EINVAL);
    CHECK(wmm_ctl("prof.top", 1, NULL) == -1);
    CHECK(errno == EINVAL);
    CHECK(wmm_ctl("prof.top", (int)WMM_PROF_TOP_MAX + 1, tab) == -1);
    CHECK(errno == EINVAL);

    /* ---- 关闭状态:节拍不推进,不产生调用点 ---- */
    CHECK(wmm_ctl("sampling.disable") == 0);
    CHECK(g_opt.sample_n == 0);

    /* 先取一次 TOP:把此前任何残留样本合并掉,再读 head 作基线 */
    uint64_t e_before = entry_count(2048);
    uint64_t head0 = atomic_load_explicit(&g_tcache->prof.head,
                                          memory_order_relaxed);
    eat_e();
    uint64_t head1 = atomic_load_explicit(&g_tcache->prof.head,
                                          memory_order_relaxed);
    CHECK(head1 == head0);                  /* 关闭时根本没走采样入口 */
    CHECK(entry_count(2048) == e_before);   /* 也没有新增估算计数 */

    /* ---- 开启:A(大对象)与 B(小对象) ---- */
    CHECK(wmm_ctl("sampling.enable") == 0);
    CHECK(g_opt.sample_n == WMM_PROF_SAMPLE_N);

    uint64_t a0 = entry_count(1u << 20);
    eat_a();
    wmm_prof_entry_t ea;
    memset(&ea, 0, sizeof ea);
    int ia = find_entry(&ea, 1u << 20);
    CHECK(ia >= 0);
    if (ia >= 0) {
        CHECK(ea.count - a0 == PROF_N_A);       /* 4 样本 × 权重 1000 */
        CHECK(ea.bytes == ea.count * (1u << 20));
        CHECK(ea.pc != 0);
        CHECK(ea.reserved == 0);
    }

    uint64_t b0 = entry_count(256);
    eat_b();
    wmm_prof_entry_t eb;
    memset(&eb, 0, sizeof eb);
    int ib = find_entry(&eb, 256);
    CHECK(ib >= 0);
    if (ib >= 0) {
        CHECK(eb.count - b0 == PROF_N_B);
        CHECK(eb.bytes == eb.count * 256);
        CHECK(eb.pc != 0);
        CHECK(eb.reserved == 0);
    }
    if (ia >= 0 && ib >= 0)
        CHECK(ea.pc != eb.pc);   /* 不同调用点必须可区分 */

    /* TOP 排序:1MB × 4000 远大于 256B × 3000,A 必须排在 B 前面。
       用同一次快照里的下标比较,避免两次取数之间表变化 */
    int n = wmm_ctl("prof.top", (int)WMM_PROF_TOP_MAX, tab);
    CHECK(n >= 2);
    if (n >= 2) {
        int ra = -1, rb = -1;
        for (int i = 0; i < n; i++) {
            if (tab[i].count != 0 && tab[i].bytes == tab[i].count * (1u << 20))
                ra = i;
            if (tab[i].count != 0 && tab[i].bytes == tab[i].count * 256)
                rb = i;
        }
        CHECK(ra >= 0 && rb >= 0 && ra < rb);
    }

    /* ---- D:再加一个可区分的调用点 ---- */
    uint64_t d0 = entry_count(1024);
    eat_d();
    CHECK(entry_count(1024) - d0 == PROF_N_D);

    /* ---- 环绕圈:300 个样本只留最新 256 个 ---- */
    uint64_t w0 = entry_count(128);
    eat_w();
    wmm_prof_entry_t ew;
    memset(&ew, 0, sizeof ew);
    int iw = find_entry(&ew, 128);
    CHECK(iw >= 0);
    if (iw >= 0) {
        CHECK(ew.count - w0 == 256u * WMM_PROF_SAMPLE_N);
        CHECK(ew.bytes == ew.count * 128);
        CHECK(ew.pc != 0);
    }

    /* ---- 工作线程:退出时析构把残余样本归账 ---- */
    uint64_t c0 = entry_count(512);
    int ok = 0;
    pthread_t th;
    if (pthread_create(&th, NULL, worker_thread, &ok) == 0) {
        CHECK(pthread_join(th, NULL) == 0);
        CHECK(ok == 1);
        CHECK(entry_count(512) - c0 == PROF_N_C);
    } else {
        CHECK(0);   /* 线程起不来:本组核心场景无法覆盖 */
    }

    /* ---- 再关闭:后续分配不再采样 ---- */
    CHECK(wmm_ctl("sampling.disable") == 0);
    CHECK(g_opt.sample_n == 0);
    (void)entry_count(2048);   /* 先合并干净 */
    head0 = atomic_load_explicit(&g_tcache->prof.head, memory_order_relaxed);
    eat_e();
    head1 = atomic_load_explicit(&g_tcache->prof.head, memory_order_relaxed);
    CHECK(head1 == head0);

    /* ---- 输出契约:尾部清零 + 打印供 addr2line 对名 ---- */
    int ns = wmm_ctl("prof.top", 64, tab);
    CHECK(ns >= 0);
    if (ns >= 0) {
        if (ns < 64)
            for (int i = ns; i < 64; i++)
                CHECK(tab[i].pc == 0 && tab[i].bytes == 0 &&
                      tab[i].count == 0 && tab[i].reserved == 0);

        printf("  prof top (%d entries, estimates with weight %u):\n",
               ns, WMM_PROF_SAMPLE_N);
        for (int i = 0; i < ns && i < 8; i++)
            printf("    pc=%p bytes=%llu count=%llu\n", (void *)tab[i].pc,
                   (unsigned long long)tab[i].bytes,
                   (unsigned long long)tab[i].count);
    }

    return g_fail;
}
