/* test_guard.c —— guard/quarantine 与泄漏三档用例
 *
 * 覆盖:
 *   基本布局:guard 对象独占 slab、数据区按 data_off 对齐垫层摆放、
 *        前后 redzone 全为 0xBD、usable_size 等于尺寸类规格;
 *   越界写:写尾部 redzone、写头部前一字节,free 时各产生 0xBD,
 *        事件的指针精确落在被踩的那个字节上,两帧调用点非零;
 *   双重释放:隔离中的对象再次 free 产生 0xBE,且不改变 slab 状态;
 *   野指针:数据区中间的指针 free 产生 0xBE,事件带上分配点;
 *   UAF 写:free 后在隔离期写数据区,tick 时产生 0xBF;tick 后
 *        (已进回收池)再被写,同类下一次分配产生 0xBC;
 *   泄漏 A 档:leak.dump 全量聚合存活 guard 对象的分配调用点,
 *        count/bytes 与分配次数/尺寸类规格吻合;
 *   泄漏 C 档:后台线程观察 live_bytes 连续上涨,数秒内产生 0xDA
 *        并自动打开采样;
 *   泄漏 B 档:leak.check 首次建基线,随后 20 万次同调用点分配
 *        在增量表里留下 bytes == count × 3072 的行;
 *   关闭语义:guard.disable 排空隔离区/回收池,已在应用的 guard
 *        对象仍能正确释放(直接归还物理层),此后新分配回常规 slab。
 *
 * 确定性:本组开头停掉后台线程并清空事件环与泄漏窗口状态,各场景
 * 之间先 drain 事件,断言按"存在性 + 精确指针"做——被踩过的 slab
 * 在隔离检查、池复用、关闭排空等多处都会被再次校验,同一个 0xBD
 * 可能出现多次,数条数会把正确行为断成失败。
 *
 * 坏内存写是刻意为之:测试写的是 wmm 自己 mmap 的 slab 页,ASan
 * 看不到,也不会影响分配器元数据(base 区与 slab 头都不在数据区
 * 内),这正是要验证的检测能力。
 */

#include "wmm_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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

/* 事件类型(线上格式见 wmm.h) */
#define EV_UAF_WRITE   0xBCu   /* 释放后内存被改 */
#define EV_REDZONE     0xBDu   /* 越界写踩哨兵 */
#define EV_BAD_FREE    0xBEu   /* 双重释放/野指针 */
#define EV_QUARANTINE  0xBFu   /* 隔离期被写 */
#define EV_LEAK        0xDAu   /* 泄漏告警 */

static bool all_bytes(const void *p, size_t n, uint8_t v) {
    const uint8_t *q = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++)
        if (q[i] != v)
            return false;
    return true;
}

static wmm_slab_t *slab_of(void *p) {
    wmm_slab_t *sl = NULL;
    return wmm_pte_is_slab(p, &sl) ? sl : NULL;
}

/* 抽干事件环,至多保留 cap 条。返回值可能小于实际条数——断言只
   关心"出现过什么",不关心历史条数。 */
static int drain_events(wmm_event_t *buf, size_t cap) {
    wmm_event_t tmp[32];
    size_t total = 0;

    for (;;) {
        size_t got = 0;
        if (wmm_event_drain(tmp, 32, &got) != 0)
            break;
        for (size_t i = 0; i < got; i++)
            if (total < cap)
                buf[total++] = tmp[i];
        if (got < 32)
            break;
    }
    return (int)total;
}

static const wmm_event_t *find_event(const wmm_event_t *ev, int n,
                                     uint32_t type, uintptr_t ptr,
                                     bool match_ptr) {
    for (int i = 0; i < n; i++)
        if (ev[i].type == type && (!match_ptr || ev[i].ptr == ptr))
            return &ev[i];
    return NULL;
}

/* 泄漏 B 档的"吃内存函数":noinline 保证 20 万次分配共享同一个
   返回地址;3072 是专属尺寸类,行满足 bytes == count × 3072 即可
   唯一识别。分配后立刻释放——prof 统计的是累计分配量,不是存活量。 */
#define LEAK_B_N 200000u
#define LEAK_B_SIZE 3072u

static __attribute__((noinline)) void leak_b_work(void) {
    for (unsigned i = 0; i < LEAK_B_N; i++) {
        void *p = wmm_alloc(LEAK_B_SIZE);
        if (p == NULL)
            return;
        wmm_free(p);
    }
}

int test_guard(void) {
    g_fail = 0;
    wmm_boot();

    /* 前面的组可能留下事件(尤其 shm 组注入过合成 0xDA)和泄漏
       窗口状态;本组要按事件类型精确认定,先归零 */
    CHECK(wmm_ctl("bg.disable") == 0);
    wmm_leak_reset();
    CHECK(wmm_ctl("sampling.disable") == 0);

    wmm_event_t ev[64];
    (void)drain_events(ev, 64);

    /* ---- 基本布局:单对象 slab + 双向哨兵 ---- */
    CHECK(wmm_ctl("guard.enable") == 0);
    CHECK(g_opt.guard);

    void *p = wmm_alloc(100);
    CHECK(p != NULL);
    wmm_slab_t *ps = p != NULL ? slab_of(p) : NULL;
    CHECK(ps != NULL);
    if (p != NULL && ps != NULL) {
        CHECK(ps->guarded);
        CHECK(ps->nregs == 1);
        CHECK(ps->reg_size >= 100);
        CHECK(ps->data_off >= WMM_ALIGN);
        CHECK((uintptr_t)p == (uintptr_t)ps->addr + ps->data_off);
        CHECK((ps->data_off % WMM_ALIGN) == 0);
        CHECK(((uintptr_t)p & 15u) == 0);
        CHECK(wmm_usable_size(p) == ps->reg_size);
        CHECK(all_bytes(ps->addr, ps->data_off, 0xBD));
        CHECK(all_bytes((char *)p + ps->reg_size, WMM_GUARD_RZ, 0xBD));

        wmm_free(p);
        CHECK(ps->nfree == 1);                      /* 隔离中:整块空闲 */
        CHECK(all_bytes(p, ps->reg_size, 0xBC));    /* 数据区已 poison */
    }

    /* ---- 越界写:尾部 redzone,free 时抓现行 ---- */
    (void)drain_events(ev, 64);
    void *q = wmm_alloc(64);
    wmm_slab_t *qs = q != NULL ? slab_of(q) : NULL;
    CHECK(q != NULL && qs != NULL);
    if (q != NULL && qs != NULL) {
        ((uint8_t *)q)[qs->reg_size] = 0x55;
        wmm_free(q);

        int n = drain_events(ev, 64);
        const wmm_event_t *e = find_event(ev, n, EV_REDZONE,
                                          (uintptr_t)q + qs->reg_size, true);
        CHECK(e != NULL);
        if (e != NULL) {
            CHECK(e->pc0 != 0);          /* 释放点 */
            CHECK(e->pc1 != 0);          /* 分配点 */
            CHECK(e->cls == qs->szind);
        }
    }

    /* ---- 越界写:头部前一字节 ---- */
    (void)drain_events(ev, 64);
    q = wmm_alloc(64);
    qs = q != NULL ? slab_of(q) : NULL;
    CHECK(q != NULL && qs != NULL);
    if (q != NULL && qs != NULL) {
        ((uint8_t *)q)[-1] = 0x66;
        wmm_free(q);

        int n = drain_events(ev, 64);
        const wmm_event_t *e = find_event(ev, n, EV_REDZONE,
                                          (uintptr_t)q - 1, true);
        CHECK(e != NULL);
        if (e != NULL)
            CHECK(e->pc0 != 0 && e->pc1 != 0);
    }

    /* ---- 双重释放:隔离中的对象再次 free ---- */
    (void)drain_events(ev, 64);
    void *d = wmm_alloc(2048);
    wmm_slab_t *ds = d != NULL ? slab_of(d) : NULL;
    CHECK(d != NULL && ds != NULL);
    if (d != NULL && ds != NULL) {
        uintptr_t alloc_pc = ds->site_pc;
        wmm_free(d);
        wmm_free(d);                     /* 第二次:0xBE,不改变状态 */

        int n = drain_events(ev, 64);
        const wmm_event_t *e = find_event(ev, n, EV_BAD_FREE,
                                          (uintptr_t)d, true);
        CHECK(e != NULL);
        if (e != NULL) {
            CHECK(e->pc0 != 0);
            CHECK(e->pc1 == alloc_pc);
        }
        CHECK(ds->nfree == 1);           /* 仍在隔离区,未被二次污染 */
    }

    /* ---- 野指针:数据区中间的地址 ---- */
    (void)drain_events(ev, 64);
    void *w = wmm_alloc(512);
    wmm_slab_t *ws = w != NULL ? slab_of(w) : NULL;
    CHECK(w != NULL && ws != NULL);
    if (w != NULL && ws != NULL) {
        uintptr_t alloc_pc = ws->site_pc;
        wmm_free((char *)w + 8);

        int n = drain_events(ev, 64);
        const wmm_event_t *e = find_event(ev, n, EV_BAD_FREE,
                                          (uintptr_t)w + 8, true);
        CHECK(e != NULL);
        if (e != NULL) {
            CHECK(e->pc0 != 0);
            CHECK(e->pc1 == alloc_pc);
        }
        wmm_free(w);                     /* 正确地址仍可正常释放 */
    }

    /* ---- UAF:隔离期写 0xBF,池中写 0xBC ---- */
    (void)drain_events(ev, 64);
    void *u = wmm_alloc(2560);
    wmm_slab_t *us = u != NULL ? slab_of(u) : NULL;
    CHECK(u != NULL && us != NULL);
    if (u != NULL && us != NULL) {
        uintptr_t alloc_pc = us->site_pc;
        CHECK(alloc_pc != 0);

        wmm_free(u);
        ((uint8_t *)u)[0] = 0xAA;        /* 隔离期写入,freed memory UAF */
        wmm_guard_tick();

        int n = drain_events(ev, 64);
        const wmm_event_t *e = find_event(ev, n, EV_QUARANTINE,
                                          (uintptr_t)u, true);
        CHECK(e != NULL);
        if (e != NULL) {
            CHECK(e->pc0 == alloc_pc);   /* 分配点可归因 */
            CHECK(e->pc1 == 0);
        }

        /* tick 后 slab 已进回收池;同类唯一 slab,LIFO 必被复用。
           复用前的 poison 检查抓到 0xAA,记 0xBC。 */
        void *u2 = wmm_alloc(2560);
        CHECK(u2 == u);
        n = drain_events(ev, 64);
        e = find_event(ev, n, EV_UAF_WRITE, (uintptr_t)u, true);
        CHECK(e != NULL);
        if (e != NULL) {
            CHECK(e->pc0 == alloc_pc);
            CHECK(e->pc1 == 0);
        }
        if (u2 != NULL)
            wmm_free(u2);
    }

    /* ---- 泄漏 A 档:全量 dump 按分配点聚合 ---- */
    (void)drain_events(ev, 64);
    enum { DUMP_N = 3 };
    void *dp[DUMP_N];
    size_t dsize = wmm_bin_info[wmm_size2index(3000)].reg_size;
    for (int i = 0; i < DUMP_N; i++) {
        dp[i] = wmm_alloc(3000);         /* 同一调用点:class 3072 */
        CHECK(dp[i] != NULL);
    }

    wmm_prof_entry_t rows[8];
    unsigned nr = wmm_guard_leak_dump(rows, 8);
    CHECK(nr == 1);
    if (nr == 1) {
        CHECK(rows[0].pc != 0);
        CHECK(rows[0].count == DUMP_N);
        CHECK(rows[0].bytes == (uint64_t)DUMP_N * dsize);
        CHECK(rows[0].reserved == 0);
    }
    CHECK(wmm_ctl("leak.dump") == 0);    /* 命令接线与打印路径 */

    for (int i = 0; i < DUMP_N; i++)
        if (dp[i] != NULL)
            wmm_free(dp[i]);

    /* ---- 泄漏 C 档:live_bytes 连续上涨 → 0xDA + 自动开采样 ---- */
    (void)drain_events(ev, 64);
    CHECK(wmm_ctl("bg.enable") == 0);

    enum { LEAK_C_MAX = 4096 };
    static void *leakp[LEAK_C_MAX];
    size_t nl = 0;
    bool alerted = false;
    uint64_t t0 = wmm_mono_ns();

    while (!alerted && wmm_mono_ns() - t0 < 15000000000ull) {
        for (int k = 0; k < 16 && nl < LEAK_C_MAX; k++) {
            void *x = wmm_alloc(1024);
            CHECK(x != NULL);
            if (x != NULL)
                leakp[nl++] = x;
        }
        usleep(100000);                  /* 每窗口有可观测的增长 */

        int n = drain_events(ev, 64);
        if (find_event(ev, n, EV_LEAK, 0, false) != NULL)
            alerted = true;
    }

    CHECK(alerted);
    CHECK(g_opt.sample_n != 0);          /* 告警自动打开采样 */
    printf("  leak alert after %llu ms, %zu objects live\n",
           (unsigned long long)((wmm_mono_ns() - t0) / 1000000ull), nl);

    for (size_t i = 0; i < nl; i++)
        wmm_free(leakp[i]);
    CHECK(wmm_ctl("bg.disable") == 0);

    /* ---- 泄漏 B 档:TOP N 增量对比 ---- */
    CHECK(wmm_ctl("sampling.enable") == 0);
    (void)drain_events(ev, 64);

    wmm_prof_entry_t lbase[8];
    CHECK(wmm_ctl("leak.check", 8, lbase) == 0);   /* 首次:建立基线 */

    leak_b_work();

    wmm_prof_entry_t lrows[16];
    int ln = wmm_ctl("leak.check", 16, lrows);
    CHECK(ln >= 1);

    bool found = false;
    for (int i = 0; i < ln; i++) {
        if (lrows[i].pc != 0 && lrows[i].count == LEAK_B_N &&
            lrows[i].bytes == (uint64_t)LEAK_B_N * LEAK_B_SIZE) {
            found = true;
            break;
        }
    }
    CHECK(found);
    printf("  leak.check: %d delta rows, leak function %s\n",
           ln, found ? "located" : "NOT found");

    (void)drain_events(ev, 64);
    CHECK(wmm_ctl("sampling.disable") == 0);

    /* ---- 关闭语义:排空池,已有 guard 对象仍可释放 ---- */
    void *edge = wmm_alloc(128);
    wmm_slab_t *es = edge != NULL ? slab_of(edge) : NULL;
    CHECK(edge != NULL && es != NULL && es->guarded);

    CHECK(wmm_ctl("guard.disable") == 0);
    CHECK(!g_opt.guard);

    if (edge != NULL) {
        wmm_free(edge);                  /* 检测已关:命中即归还物理层 */
        wmm_slab_t *gone = NULL;
        CHECK(!wmm_pte_is_slab(edge, &gone));
    }

    uint64_t guard_live = 0;
    for (szind_t i = 0; i < WMM_NBINS; i++)
        guard_live += g_guard_bins[i].live;
    CHECK(guard_live == 0);              /* 所有 guard 对象已结清 */

    /* 关闭后新分配回常规布局 */
    void *np = wmm_alloc(64);
    CHECK(np != NULL);
    wmm_slab_t *ns = np != NULL ? slab_of(np) : NULL;
    CHECK(ns != NULL && !ns->guarded);
    if (np != NULL)
        wmm_free(np);

    /* 收尾:泄漏窗口与事件环恢复干净,不把状态带给下一组 */
    wmm_leak_reset();
    (void)drain_events(ev, 64);

    return g_fail;
}
