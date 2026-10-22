/* validate_common.h —— 生产验证工具共用的小助手
 *
 * 只放 header-only 的 static inline:解析参数、读 RSS、随机流、
 * 快照不变式与事件分类。助手本身只用 libc 与 mmap,不经过被测
 * 分配器;否则验证工具会改变它正在测量的状态。
 */
#ifndef WMM_VALIDATE_COMMON_H_INCLUDED
#define WMM_VALIDATE_COMMON_H_INCLUDED

#include "wmm_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static inline uint64_t v_now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void v_sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

/* 进程 RSS,单位 KB;读不到返回 0 */
static inline uint64_t v_rss_kb(void) {
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

static inline int v_parse_u64(const char *s, uint64_t lo, uint64_t hi,
                              uint64_t *out) {
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

typedef struct {
    uint64_t state;
} v_rng_t;

static inline uint64_t v_rng_next(v_rng_t *r) {
    uint64_t x = r->state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    r->state = x;
    return x;
}

static inline uint64_t v_seed(uint64_t base, unsigned id) {
    uint64_t s = base ^ ((uint64_t)(id + 1) * 0x9E3779B97F4A7C15ull);
    return s != 0 ? s : 0xD1B54A32D192ED03ull;
}

/* 现行契约:16B 对齐;唯一例外是 8B 最小类,只保证 8B */
static inline bool v_align_ok(const void *p, size_t usable) {
    return ((uintptr_t)p & 15u) == 0 || usable == 8;
}

/* 每线程缓存对象上限:小对象按 clamp(2*nregs, 20, 200) 求和,
   大缓存按每个页数类 NSLOTS 折算对象数 */
static inline uint64_t v_tcache_max_per_thread(void) {
    uint64_t sum = 0;
    uint32_t maxp = g_tcache != NULL ? g_tcache->large_max_pages
                                     : wmm_tcache_max_pages_now();
    uint64_t ncls = maxp >= WMM_TCACHE_LARGE_MIN_PAGES
                      ? (uint64_t)(maxp - WMM_TCACHE_LARGE_MIN_PAGES + 1u)
                      : 0;

    for (szind_t i = 0; i < WMM_NBINS; i++) {
        unsigned v = 2u * wmm_bin_info[i].nregs;
        if (v < WMM_TCACHE_NCACHED_MIN)
            v = WMM_TCACHE_NCACHED_MIN;
        if (v > WMM_TCACHE_NCACHED_MAX)
            v = WMM_TCACHE_NCACHED_MAX;
        sum += v;
    }
    return sum + (uint64_t)WMM_TCACHE_LARGE_NSLOTS * ncls;
}

/* 快照的基本恒等式。prev_valid 时额外检查 epoch 与 high_water 的
   历史关系;返回失败条数并逐条打印,便于定位是哪个字段坏了。 */
static inline unsigned v_stats_check(const wmm_stats_t *st,
                                     const wmm_stats_t *prev,
                                     bool prev_valid) {
    unsigned fail = 0;
    uint64_t obj_sum = 0;

    for (szind_t i = 0; i <= WMM_NBINS; i++)
        obj_sum += st->cls[i].live;

    if (obj_sum != st->live_objects) {
        fprintf(stderr, "stats: live_objects=%" PRIu64
                " but sum(cls.live)=%" PRIu64 "\n",
                st->live_objects, obj_sum);
        fail++;
    }

    if (st->active_bytes < st->live_bytes) {
        fprintf(stderr, "stats: active=%" PRIu64 " < live=%" PRIu64 "\n",
                st->active_bytes, st->live_bytes);
        fail++;
    } else if (st->frag_bytes != st->active_bytes - st->live_bytes) {
        fprintf(stderr, "stats: frag=%" PRIu64 " != active-live=%" PRIu64
                "\n", st->frag_bytes, st->active_bytes - st->live_bytes);
        fail++;
    }

    if (st->high_water_live < st->live_bytes) {
        fprintf(stderr, "stats: high_water=%" PRIu64 " < live=%" PRIu64
                "\n", st->high_water_live, st->live_bytes);
        fail++;
    }

    for (szind_t i = 0; i < WMM_NBINS; i++) {
        if (st->cls[i].size != wmm_bin_info[i].reg_size) {
            fprintf(stderr, "stats: cls[%u].size=%" PRIu64
                    " expected %u\n", (unsigned)i, st->cls[i].size,
                    wmm_bin_info[i].reg_size);
            fail++;
        }
    }
    if (st->cls[WMM_NBINS].size != 0) {
        fprintf(stderr, "stats: large cls size=%" PRIu64 " expected 0\n",
                st->cls[WMM_NBINS].size);
        fail++;
    }

    if (prev_valid) {
        if (st->epoch <= prev->epoch) {
            fprintf(stderr, "stats: epoch did not advance (%" PRIu64
                    " <= %" PRIu64 ")\n", st->epoch, prev->epoch);
            fail++;
        }
        if (st->high_water_live < prev->high_water_live) {
            fprintf(stderr, "stats: high_water decreased (%" PRIu64
                    " < %" PRIu64 ")\n",
                    st->high_water_live, prev->high_water_live);
            fail++;
        }
    }

    return fail;
}

/* 小对象字节可由 cls 精确重算;剩余部分必须是大对象,按页对齐且
   每个大对象至少占一页。并发快照下这条恒等式也成立。 */
static inline unsigned v_stats_large_check(const wmm_stats_t *st) {
    unsigned fail = 0;
    uint64_t small = 0;

    for (szind_t i = 0; i < WMM_NBINS; i++)
        small += st->cls[i].live * st->cls[i].size;

    if (small > st->live_bytes) {
        fprintf(stderr, "stats: small bytes=%" PRIu64
                " > live=%" PRIu64 "\n", small, st->live_bytes);
        return fail + 1;
    }

    uint64_t large = st->live_bytes - small;
    if (large % (uint64_t)WMM_PAGE != 0) {
        fprintf(stderr, "stats: large bytes=%" PRIu64
                " not page aligned\n", large);
        fail++;
    }
    if (large < st->cls[WMM_NBINS].live * (uint64_t)WMM_PAGE) {
        fprintf(stderr, "stats: large bytes=%" PRIu64
                " < large objects=%" PRIu64 " pages\n",
                large, st->cls[WMM_NBINS].live);
        fail++;
    }
    return fail;
}

typedef struct {
    uint64_t total;
    uint64_t guard;   /* 0xBC 0xBD 0xBE 0xBF */
    uint64_t leak;    /* 0xDA */
    uint64_t oom;     /* 0xDB */
    uint64_t purge;   /* 0xDC */
    uint64_t other;
} v_event_counts_t;

/* 排空事件环并按类型计数。返回 0 成功,-1 表示 event_drain 失败 */
static inline int v_events_drain(v_event_counts_t *c) {
    wmm_event_t buf[64];

    memset(c, 0, sizeof(*c));
    for (;;) {
        size_t got = 0;
        if (wmm_event_drain(buf, 64, &got) != 0)
            return -1;
        if (got == 0)
            return 0;

        for (size_t i = 0; i < got; i++) {
            c->total++;
            switch (buf[i].type) {
            case 0xBC:
            case 0xBD:
            case 0xBE:
            case 0xBF:
                c->guard++;
                break;
            case 0xDA:
                c->leak++;
                break;
            case 0xDB:
                c->oom++;
                break;
            case 0xDC:
                c->purge++;
                break;
            default:
                c->other++;
                break;
            }
        }
    }
}

#endif
