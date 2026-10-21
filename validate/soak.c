/* soak.c —— 长时浸泡:并发分配、跨线程释放、线程 churn 与不变量巡检
 *
 * 独立进程链接 release 库。工作线程只做分配/释放类操作;控制线程
 * 每秒取一次快照、排空事件环、读 RSS。停止后要求 live/active/
 * cached 全部回到基线,否则就是泄漏或回收没走完。
 *
 * 注意:分配器统计里的 allocated/deallocated 只汇总当前仍注册的
 * 线程行,线程退出会带走它那一行。因此默认开启 churn 时这两项不做
 * 单调断言;只有 --no-churn 才检查。
 */

#define _GNU_SOURCE

#include "validate_common.h"

#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/mman.h>

#define SOAK_MAGIC      0x574D4D31u
#define SOAK_SLOTS      65536u
#define SOAK_XFER       256u
#define SOAK_MAX_THREADS 64u

static const size_t soak_large_sizes[] = {
    16384, 24576, 32768, 49152, 65536, 131072, 262144, 1048576
};

typedef enum {
    SOAK_PHASE_RAMP,
    SOAK_PHASE_STEADY,
    SOAK_PHASE_BURST,
    SOAK_PHASE_COOL,
} soak_phase_t;

static const char *soak_phase_name(soak_phase_t ph) {
    switch (ph) {
    case SOAK_PHASE_RAMP:   return "ramp";
    case SOAK_PHASE_STEADY: return "steady";
    case SOAK_PHASE_BURST:  return "burst";
    case SOAK_PHASE_COOL:   return "cool";
    }
    return "unknown";
}

typedef struct {
    void    *p;
    uint32_t req;
    uint32_t tag;
    size_t   usable;
} soak_obj_t;

typedef struct {
    void    *p;
    uint32_t req;
    uint32_t tag;
} soak_xfer_t;

typedef struct {
    unsigned      id;
    pthread_t     tid;
    v_rng_t       rng;

    soak_obj_t   *slots;
    unsigned      nocc;
    size_t        live_bytes;
    size_t        cap_bytes;
    size_t        base_cap;
    soak_phase_t  phase;
    uint64_t      op_seq;
    uint64_t      tag_seq;

    pthread_mutex_t xfer_lock;
    soak_xfer_t     xfer[SOAK_XFER];
    unsigned        xhead;
    unsigned        xtail;

    _Atomic uint64_t ops;
    uint64_t         sent;
    uint64_t         send_drop;
    uint64_t         received;
} soak_worker_t;

typedef struct {
    unsigned     seconds;
    unsigned     threads;
    unsigned     live_mb;
    unsigned     rss_growth_mb;
    uint64_t     purge_every;
    uint64_t     seed;
    const char  *csv_path;
    bool         bg;
    bool         churn;
    bool         cross_free;
} soak_cfg_t;

static soak_cfg_t      g_cfg;
static soak_worker_t  *g_workers;
static uint64_t        g_start_ns;
static uint64_t        g_duration_ns;
static size_t          g_base_cap;

static volatile sig_atomic_t g_stop_sig;
static _Atomic bool          g_stop;
static _Atomic unsigned      g_fail;

#define SOAK_FAIL(...)                                                   \
    do {                                                                 \
        (void)atomic_fetch_add_explicit(&g_fail, 1,                      \
                                        memory_order_relaxed);           \
        atomic_store_explicit(&g_stop, true, memory_order_relaxed);      \
        fprintf(stderr, "soak: " __VA_ARGS__);                           \
    } while (0)

static void soak_add_fail(unsigned n) {
    if (n != 0) {
        (void)atomic_fetch_add_explicit(&g_fail, n, memory_order_relaxed);
        atomic_store_explicit(&g_stop, true, memory_order_relaxed);
    }
}

static bool soak_stopping(void) {
    return g_stop_sig != 0 ||
           atomic_load_explicit(&g_stop, memory_order_relaxed);
}

static void soak_on_term(int sig) {
    (void)sig;
    g_stop_sig = 1;
}

static void soak_on_alarm(int sig) {
    (void)sig;
    _exit(124);
}

static void soak_install_signals(void) {
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = soak_on_term;
    if (sigaction(SIGINT, &sa, NULL) != 0 ||
        sigaction(SIGTERM, &sa, NULL) != 0) {
        perror("soak: sigaction");
        exit(2);
    }

    sa.sa_handler = soak_on_alarm;
    if (sigaction(SIGALRM, &sa, NULL) != 0) {
        perror("soak: sigaction SIGALRM");
        exit(2);
    }
}

static void *soak_map(size_t bytes) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (p == MAP_FAILED) {
        perror("soak: mmap");
        exit(2);
    }
    return p;
}

static soak_phase_t soak_phase_at(uint64_t elapsed_ns, unsigned *pct) {
    uint64_t ramp_end = g_duration_ns / 5u;
    uint64_t steady_end = g_duration_ns * 7u / 10u;
    uint64_t burst_end = g_duration_ns * 9u / 10u;

    if (elapsed_ns < ramp_end) {
        unsigned span = (unsigned)(elapsed_ns * 75u / (ramp_end != 0
                                                       ? ramp_end : 1u));
        *pct = 25u + span;
        return SOAK_PHASE_RAMP;
    }
    if (elapsed_ns < steady_end) {
        *pct = 75u;
        return SOAK_PHASE_STEADY;
    }
    if (elapsed_ns < burst_end) {
        *pct = 100u;
        return SOAK_PHASE_BURST;
    }

    *pct = 25u;
    return SOAK_PHASE_COOL;
}

static void soak_update_phase(soak_worker_t *w) {
    uint64_t elapsed = v_now_ns() - g_start_ns;
    unsigned pct = 0;

    w->phase = soak_phase_at(elapsed, &pct);
    w->cap_bytes = g_base_cap * (size_t)pct / 100u;
}

static uint32_t soak_new_tag(soak_worker_t *w, uint32_t req) {
    uint32_t t = (uint32_t)((w->id + 1u) * 0x9E3779B1u)
               ^ (uint32_t)(w->tag_seq++ * 0x85EBCA6Bu)
               ^ (req * 0x27D4EB2Fu);

    return t != 0 ? t : 1u;
}

static void soak_stamp(void *p, size_t usable, uint32_t tag, bool large) {
    uint32_t *h = (uint32_t *)p;

    h[0] = SOAK_MAGIC;
    h[1] = tag;
    if (large)
        ((unsigned char *)p)[usable - 1] = (unsigned char)tag;
}

static bool soak_verify_obj(soak_worker_t *w, const soak_obj_t *o,
                            const char *what) {
    const uint32_t *h = (const uint32_t *)o->p;
    bool ok = h[0] == SOAK_MAGIC && h[1] == o->tag;

    if (ok && o->usable > WMM_SMALL_MAXCLASS) {
        unsigned char end = ((const unsigned char *)o->p)[o->usable - 1];
        if (end != (unsigned char)o->tag)
            ok = false;
    }

    if (!ok) {
        SOAK_FAIL("content check failed thread=%u op=%s ptr=%p req=%u "
                  "tag=%08x magic=%08x\n",
                  w->id, what, o->p, o->req, o->tag, h[0]);
    }
    return ok;
}

static size_t soak_pick_size(soak_phase_t ph, uint64_t r) {
    unsigned large_pct;

    switch (ph) {
    case SOAK_PHASE_RAMP:   large_pct = 4;  break;
    case SOAK_PHASE_STEADY: large_pct = 5;  break;
    case SOAK_PHASE_BURST:  large_pct = 15; break;
    default:                large_pct = 3;  break;
    }

    if ((unsigned)(r % 100u) < large_pct) {
        size_t n = sizeof soak_large_sizes / sizeof soak_large_sizes[0];
        return soak_large_sizes[(r >> 8) % n];
    }

    unsigned idx = (unsigned)((r >> 16) % WMM_NBINS);
    uint32_t lo = idx == 0 ? 0 : wmm_bin_info[idx - 1].reg_size;
    uint32_t hi = wmm_bin_info[idx].reg_size;
    uint32_t span = hi - lo;

    return (size_t)lo + 1u + (size_t)((r >> 32) % span);
}

static bool soak_check_common(void *p, size_t req, size_t usable) {
    if (p == NULL) {
        SOAK_FAIL("allocation of %zu bytes returned NULL\n", req);
        return false;
    }
    if (usable < req) {
        SOAK_FAIL("usable_size %zu < request %zu (ptr=%p)\n",
                  usable, req, p);
        return false;
    }
    if (!v_align_ok(p, usable)) {
        SOAK_FAIL("pointer %p not 16-byte aligned (usable=%zu)\n",
                  p, usable);
        return false;
    }
    return true;
}

static void soak_store(soak_worker_t *w, void *p, uint32_t req,
                       uint32_t tag, size_t usable) {
    if (w->nocc >= SOAK_SLOTS) {
        wmm_free(p);
        SOAK_FAIL("slot table exhausted after capacity check\n");
        return;
    }

    soak_obj_t *o = &w->slots[w->nocc++];
    o->p      = p;
    o->req    = req;
    o->tag    = tag;
    o->usable = usable;
    w->live_bytes += usable;
}

static bool soak_room(soak_worker_t *w, uint64_t r) {
    if (w->nocc < SOAK_SLOTS)
        return true;
    if (w->nocc != 0) {
        unsigned j = (unsigned)((r >> 8) % w->nocc);
        soak_obj_t *o = &w->slots[j];

        (void)soak_verify_obj(w, o, "slot-full free");
        wmm_free(o->p);
        w->live_bytes -= o->usable;
        w->slots[j] = w->slots[--w->nocc];
    }
    return false;
}

static void soak_free_random(soak_worker_t *w, uint64_t r) {
    if (w->nocc == 0)
        return;

    unsigned j = (unsigned)((r >> 8) % w->nocc);
    soak_obj_t *o = &w->slots[j];

    (void)soak_verify_obj(w, o, "free");
    wmm_free(o->p);
    w->live_bytes -= o->usable;
    w->slots[j] = w->slots[--w->nocc];
}

static bool soak_ring_push(soak_worker_t *dst, const soak_xfer_t *x) {
    pthread_mutex_lock(&dst->xfer_lock);

    unsigned next = (dst->xhead + 1u) % SOAK_XFER;
    bool ok = next != dst->xtail;
    if (ok) {
        dst->xfer[dst->xhead] = *x;
        dst->xhead = next;
    }

    pthread_mutex_unlock(&dst->xfer_lock);
    return ok;
}

static unsigned soak_ring_pop(soak_worker_t *w, soak_xfer_t *out,
                              unsigned max) {
    unsigned n = 0;

    pthread_mutex_lock(&w->xfer_lock);
    while (n < max && w->xtail != w->xhead) {
        out[n++] = w->xfer[w->xtail];
        w->xtail = (w->xtail + 1u) % SOAK_XFER;
    }
    pthread_mutex_unlock(&w->xfer_lock);
    return n;
}

static void soak_xfer_free(soak_worker_t *w, const soak_xfer_t *x) {
    const uint32_t *h = (const uint32_t *)x->p;

    if (h[0] != SOAK_MAGIC || h[1] != x->tag) {
        SOAK_FAIL("transfer content check failed thread=%u ptr=%p req=%u "
                  "tag=%08x magic=%08x\n",
                  w->id, x->p, x->req, x->tag, h[0]);
    }
    wmm_free(x->p);
    w->received++;
}

static unsigned soak_drain_ring(soak_worker_t *w, unsigned max) {
    soak_xfer_t batch[32];
    unsigned total = 0;

    while (total < max) {
        unsigned want = max - total;
        if (want > (unsigned)(sizeof batch / sizeof batch[0]))
            want = (unsigned)(sizeof batch / sizeof batch[0]);

        unsigned n = soak_ring_pop(w, batch, want);
        if (n == 0)
            break;
        for (unsigned i = 0; i < n; i++)
            soak_xfer_free(w, &batch[i]);
        total += n;
    }
    return total;
}

static void soak_op_alloc(soak_worker_t *w, uint64_t r) {
    if (!soak_room(w, r))
        return;

    size_t req = soak_pick_size(w->phase, r);
    void  *p = wmm_alloc(req);
    size_t usable = wmm_usable_size(p);

    if (!soak_check_common(p, req, usable)) {
        wmm_free(p);
        return;
    }

    bool     large = usable > WMM_SMALL_MAXCLASS;
    uint32_t tag = soak_new_tag(w, (uint32_t)req);
    soak_stamp(p, usable, tag, large);

    if (g_cfg.cross_free && g_cfg.threads > 1 &&
        req <= WMM_SMALL_MAXCLASS && ((r >> 17) % 100u) < 12u) {
        unsigned target = (w->id + 1u +
                           (unsigned)((r >> 24) % (g_cfg.threads - 1u)))
                          % g_cfg.threads;
        soak_xfer_t x = { p, (uint32_t)req, tag };

        if (soak_ring_push(&g_workers[target], &x)) {
            w->sent++;
            return;
        }
        w->send_drop++;
    }

    soak_store(w, p, (uint32_t)req, tag, usable);
}

static void soak_op_calloc(soak_worker_t *w, uint64_t r) {
    if (!soak_room(w, r))
        return;

    size_t req = soak_pick_size(w->phase, r);
    unsigned char *p = (unsigned char *)wmm_calloc(1, req);

    if (p == NULL) {
        SOAK_FAIL("calloc(%zu) returned NULL\n", req);
        return;
    }
    if (p[0] != 0 || p[req - 1] != 0) {
        SOAK_FAIL("calloc memory not zeroed (req=%zu ptr=%p)\n", req, p);
        wmm_free(p);
        return;
    }

    size_t usable = wmm_usable_size(p);
    if (!soak_check_common(p, req, usable)) {
        wmm_free(p);
        return;
    }

    uint32_t tag = soak_new_tag(w, (uint32_t)req);
    soak_stamp(p, usable, tag, usable > WMM_SMALL_MAXCLASS);
    soak_store(w, p, (uint32_t)req, tag, usable);
}

static void soak_op_aligned(soak_worker_t *w, uint64_t r) {
    if (!soak_room(w, r))
        return;

    size_t req = soak_pick_size(w->phase, r);
    size_t align = (size_t)16u << (unsigned)(r % 9u);
    void  *p = wmm_aligned_alloc(align, req);
    size_t usable = wmm_usable_size(p);

    if (!soak_check_common(p, req, usable) ||
        ((uintptr_t)p & (align - 1u)) != 0) {
        if (p != NULL && ((uintptr_t)p & (align - 1u)) != 0) {
            SOAK_FAIL("aligned_alloc(%zu, %zu) returned misaligned %p\n",
                      align, req, p);
        }
        wmm_free(p);
        return;
    }

    uint32_t tag = soak_new_tag(w, (uint32_t)req);
    soak_stamp(p, usable, tag, usable > WMM_SMALL_MAXCLASS);
    soak_store(w, p, (uint32_t)req, tag, usable);
}

static void soak_op_posix(soak_worker_t *w, uint64_t r) {
    if (!soak_room(w, r))
        return;

    size_t req = soak_pick_size(w->phase, r);
    size_t align = (size_t)16u << (unsigned)(r % 9u);
    void  *p = NULL;
    int    rc = wmm_posix_memalign(&p, align, req);

    if (rc != 0) {
        SOAK_FAIL("posix_memalign(%zu, %zu) returned %d\n", align, req, rc);
        return;
    }

    size_t usable = wmm_usable_size(p);
    if (!soak_check_common(p, req, usable) ||
        ((uintptr_t)p & (align - 1u)) != 0) {
        if (p != NULL && ((uintptr_t)p & (align - 1u)) != 0) {
            SOAK_FAIL("posix_memalign(%zu, %zu) returned misaligned %p\n",
                      align, req, p);
        }
        wmm_free(p);
        return;
    }

    uint32_t tag = soak_new_tag(w, (uint32_t)req);
    soak_stamp(p, usable, tag, usable > WMM_SMALL_MAXCLASS);
    soak_store(w, p, (uint32_t)req, tag, usable);
}

static void soak_op_realloc(soak_worker_t *w, uint64_t r) {
    if (!soak_room(w, r))
        return;
    if (w->nocc == 0) {
        soak_op_alloc(w, r);
        return;
    }

    unsigned j = (unsigned)((r >> 8) % w->nocc);
    soak_obj_t *o = &w->slots[j];

    if (!soak_verify_obj(w, o, "realloc"))
        return;

    size_t   old_usable = o->usable;
    uint32_t old_req    = o->req;
    uint32_t tag        = o->tag;
    size_t   nr;

    if ((r & 1u) != 0) {
        nr = 1u + (size_t)((r >> 20) % 4096u);
    } else {
        size_t add = 1u + (size_t)((r >> 20) % ((size_t)old_req * 2u + 1u));
        nr = (size_t)old_req + add;
        if (nr > ((size_t)1 << 20))
            nr = (size_t)1 << 20;
    }

    void *q = wmm_realloc(o->p, nr);
    if (q == NULL) {
        SOAK_FAIL("realloc(%zu) failed (ptr=%p)\n", nr, o->p);
        return;
    }

    size_t usable = wmm_usable_size(q);
    if (usable < nr) {
        SOAK_FAIL("realloc usable %zu < request %zu\n", usable, nr);
    }

    uint32_t *h = (uint32_t *)q;
    if (h[0] != SOAK_MAGIC || h[1] != tag) {
        SOAK_FAIL("realloc lost content tag ptr=%p old=%p req=%zu\n",
                  q, o->p, nr);
    }

    soak_stamp(q, usable, tag, usable > WMM_SMALL_MAXCLASS);

    if (usable >= old_usable)
        w->live_bytes += usable - old_usable;
    else
        w->live_bytes -= old_usable - usable;

    o->p      = q;
    o->req    = (uint32_t)nr;
    o->usable = usable;
}

static void soak_op_usable(soak_worker_t *w, uint64_t r) {
    if (w->nocc == 0) {
        soak_op_alloc(w, r);
        return;
    }

    unsigned j = (unsigned)((r >> 8) % w->nocc);
    soak_obj_t *o = &w->slots[j];

    if (!soak_verify_obj(w, o, "usable_size"))
        return;

    size_t usable = wmm_usable_size(o->p);
    if (usable != o->usable || usable < o->req) {
        SOAK_FAIL("usable_size changed: got %zu stored %zu req %u\n",
                  usable, o->usable, o->req);
    }
}

static void soak_one(soak_worker_t *w) {
    (void)soak_drain_ring(w, 32);

    uint64_t r = v_rng_next(&w->rng);
    (void)atomic_fetch_add_explicit(&w->ops, 1, memory_order_relaxed);
    w->op_seq++;

    if (w->live_bytes > w->cap_bytes) {
        soak_free_random(w, r);
        return;
    }

    unsigned op = (unsigned)(r % 100u);

    if (op < 40) {
        soak_op_alloc(w, r);
    } else if (op < 75) {
        if (w->nocc == 0)
            soak_op_alloc(w, r);
        else
            soak_free_random(w, r);
    } else if (op < 83) {
        soak_op_realloc(w, r);
    } else if (op < 88) {
        soak_op_calloc(w, r);
    } else if (op < 93) {
        soak_op_aligned(w, r);
    } else if (op < 97) {
        soak_op_posix(w, r);
    } else {
        soak_op_usable(w, r);
    }
}

static void *soak_worker_main(void *arg) {
    soak_worker_t *w = (soak_worker_t *)arg;

    soak_update_phase(w);

    while (!soak_stopping() &&
           atomic_load_explicit(&g_fail, memory_order_relaxed) == 0) {
        soak_one(w);

        if ((w->op_seq & 1023u) == 0)
            soak_update_phase(w);

        if (g_cfg.purge_every != 0 && w->op_seq != 0 &&
            (w->op_seq % g_cfg.purge_every) == 0) {
            if (wmm_ctl("purge") != 0)
                SOAK_FAIL("wmm_ctl(purge) failed\n");
        }
    }

    (void)soak_drain_ring(w, UINT_MAX);
    while (w->nocc != 0) {
        soak_obj_t *o = &w->slots[w->nocc - 1u];

        (void)soak_verify_obj(w, o, "worker-exit free");
        wmm_free(o->p);
        w->live_bytes -= o->usable;
        w->nocc--;
    }
    return NULL;
}

typedef struct {
    uint64_t seed;
    unsigned id;
} soak_churn_ctx_t;

static void *soak_churn_thread(void *arg) {
    soak_churn_ctx_t *ctx = (soak_churn_ctx_t *)arg;
    v_rng_t rng = { v_seed(ctx->seed, ctx->id) };
    void   *obj[64];
    unsigned n = 0;

    for (unsigned k = 0; k < 256 && !soak_stopping(); k++) {
        uint64_t r = v_rng_next(&rng);

        if (n == 0 || (n < 64 && (r & 3u) != 0)) {
            size_t req = 1u + (size_t)((r >> 8) % 14336u);
            void  *p = wmm_alloc(req);

            if (p == NULL) {
                SOAK_FAIL("churn alloc(%zu) returned NULL\n", req);
                break;
            }
            ((unsigned char *)p)[0] = (unsigned char)r;
            ((unsigned char *)p)[req - 1] = (unsigned char)(r >> 8);
            obj[n++] = p;
        } else {
            wmm_free(obj[--n]);
        }

        if ((r & 127u) == 0) {
            void *big = wmm_alloc(65536);

            if (big != NULL) {
                ((unsigned char *)big)[0] = 1;
                wmm_free(big);
            }
        }
    }

    while (n != 0)
        wmm_free(obj[--n]);
    return NULL;
}

static void *soak_churner(void *arg) {
    uint64_t seq = 0;

    (void)arg;
    while (!soak_stopping() &&
           atomic_load_explicit(&g_fail, memory_order_relaxed) == 0) {
        soak_churn_ctx_t ctx = { g_cfg.seed, (unsigned)seq };
        pthread_t t;
        int rc = pthread_create(&t, NULL, soak_churn_thread, &ctx);

        if (rc != 0) {
            SOAK_FAIL("churn pthread_create failed: %d\n", rc);
            break;
        }
        (void)pthread_join(t, NULL);
        seq++;
        v_sleep_ms(10);
    }
    return NULL;
}

static uint64_t soak_total_ops(void) {
    uint64_t sum = 0;

    for (unsigned i = 0; i < g_cfg.threads; i++)
        sum += atomic_load_explicit(&g_workers[i].ops, memory_order_relaxed);
    return sum;
}

static void soak_drain_all_rings(void) {
    for (unsigned i = 0; i < g_cfg.threads; i++)
        (void)soak_drain_ring(&g_workers[i], UINT_MAX);
}

static void soak_usage(const char *arg0) {
    fprintf(stderr,
            "usage: %s [--seconds N] [--threads N] [--live-mb N] "
            "[--seed N] [--csv PATH]\n"
            "          [--rss-growth-mb N] [--no-bg] [--cross-free 0|1] "
            "[--no-churn] [--purge-every N]\n",
            arg0);
}

static int soak_parse(int argc, char **argv, soak_cfg_t *cfg) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        uint64_t v = 0;

        if (strcmp(a, "--help") == 0) {
            soak_usage(argv[0]);
            exit(0);
        }
        if (strcmp(a, "--no-bg") == 0) {
            cfg->bg = false;
            continue;
        }
        if (strcmp(a, "--no-churn") == 0) {
            cfg->churn = false;
            continue;
        }

        if (i + 1 >= argc) {
            fprintf(stderr, "soak: %s needs a value\n", a);
            return -1;
        }
        const char *val = argv[++i];

        if (strcmp(a, "--seconds") == 0) {
            if (v_parse_u64(val, 1, 86400, &v) != 0)
                return -1;
            cfg->seconds = (unsigned)v;
        } else if (strcmp(a, "--threads") == 0) {
            if (v_parse_u64(val, 1, SOAK_MAX_THREADS, &v) != 0)
                return -1;
            cfg->threads = (unsigned)v;
        } else if (strcmp(a, "--live-mb") == 0) {
            if (v_parse_u64(val, 1, 65536, &v) != 0)
                return -1;
            cfg->live_mb = (unsigned)v;
        } else if (strcmp(a, "--rss-growth-mb") == 0) {
            if (v_parse_u64(val, 1, 65536, &v) != 0)
                return -1;
            cfg->rss_growth_mb = (unsigned)v;
        } else if (strcmp(a, "--purge-every") == 0) {
            if (v_parse_u64(val, 0, (uint64_t)1 << 40, &v) != 0)
                return -1;
            cfg->purge_every = v;
        } else if (strcmp(a, "--seed") == 0) {
            if (v_parse_u64(val, 0, UINT64_MAX, &v) != 0)
                return -1;
            cfg->seed = v;
        } else if (strcmp(a, "--csv") == 0) {
            cfg->csv_path = val;
        } else if (strcmp(a, "--cross-free") == 0) {
            if (v_parse_u64(val, 0, 1, &v) != 0)
                return -1;
            cfg->cross_free = v != 0;
        } else {
            fprintf(stderr, "soak: unknown option %s\n", a);
            return -1;
        }
    }
    return 0;
}

static void soak_worker_init(soak_worker_t *w, unsigned id, uint64_t seed) {
    memset(w, 0, sizeof *w);

    w->id       = id;
    w->rng.state = v_seed(seed, id);
    w->slots    = (soak_obj_t *)soak_map((size_t)SOAK_SLOTS * sizeof *w->slots);
    w->base_cap = g_base_cap;
    w->cap_bytes = g_base_cap / 4u;
    w->phase    = SOAK_PHASE_RAMP;
    atomic_init(&w->ops, 0);

    if (pthread_mutex_init(&w->xfer_lock, NULL) != 0) {
        fprintf(stderr, "soak: pthread_mutex_init failed\n");
        exit(2);
    }
}

int main(int argc, char **argv) {
    soak_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.seconds       = 60;
    cfg.threads       = 4;
    cfg.live_mb       = 64;
    cfg.rss_growth_mb = 64;
    cfg.seed          = 0x5EA5ED0000000001ull;
    cfg.bg            = true;
    cfg.churn         = true;
    cfg.cross_free    = true;

    if (soak_parse(argc, argv, &cfg) != 0) {
        soak_usage(argv[0]);
        return 2;
    }

    g_cfg = cfg;
    g_duration_ns = (uint64_t)cfg.seconds * 1000000000ull;
    g_base_cap = ((size_t)cfg.live_mb << 20) / cfg.threads;

    wmm_boot();
    soak_install_signals();
    alarm(cfg.seconds + 120u);

    if (cfg.bg && wmm_bg_start() != 0) {
        perror("soak: wmm_bg_start");
        return 2;
    }

    wmm_stats_t base;
    if (wmm_stats_snapshot(&base) != 0) {
        fprintf(stderr, "soak: baseline snapshot failed\n");
        return 2;
    }

    g_workers = (soak_worker_t *)soak_map((size_t)cfg.threads *
                                          sizeof *g_workers);
    for (unsigned i = 0; i < cfg.threads; i++)
        soak_worker_init(&g_workers[i], i, cfg.seed);

    FILE *csv = NULL;
    if (cfg.csv_path != NULL) {
        csv = fopen(cfg.csv_path, "w");
        if (csv == NULL) {
            perror(cfg.csv_path);
            return 2;
        }
        fprintf(csv, "elapsed_s,phase,live_bytes,active_bytes,frag_bytes,"
                "high_water_live,live_objects,rss_kb,cached_objs,ops,"
                "guard_events,leak_events,oom_events,purge_events,"
                "other_events\n");
    }

    pthread_t churn_tid;
    bool churn_started = false;

    if (cfg.churn) {
        if (pthread_create(&churn_tid, NULL, soak_churner, NULL) != 0) {
            fprintf(stderr, "soak: cannot start churn thread\n");
            return 2;
        }
        churn_started = true;
    }

    g_start_ns = v_now_ns();

    unsigned started = 0;
    for (unsigned i = 0; i < cfg.threads; i++) {
        if (pthread_create(&g_workers[i].tid, NULL, soak_worker_main,
                           &g_workers[i]) != 0) {
            fprintf(stderr, "soak: cannot start worker %u\n", i);
            atomic_store_explicit(&g_stop, true, memory_order_relaxed);
            break;
        }
        started++;
    }

    if (started != cfg.threads) {
        for (unsigned i = 0; i < started; i++)
            (void)pthread_join(g_workers[i].tid, NULL);
        if (churn_started)
            (void)pthread_join(churn_tid, NULL);
        return 2;
    }

    printf("soak: seconds=%u threads=%u live_mb=%u bg=%d churn=%d "
           "cross_free=%d purge_every=%" PRIu64 "\n",
           cfg.seconds, cfg.threads, cfg.live_mb, cfg.bg ? 1 : 0,
           cfg.churn ? 1 : 0, cfg.cross_free ? 1 : 0, cfg.purge_every);
    fflush(stdout);

    wmm_stats_t prev;
    bool        prev_valid = false;
    uint64_t    next_tick = g_start_ns + 1000000000ull;

    uint64_t peak_live = 0;
    uint64_t peak_rss = 0;
    uint64_t peak_cached = 0;
    uint64_t ramp_rss = 0;
    bool     ramp_rss_set = false;
    bool     rss_gate_noted = false;
    bool     leak_noted = false;

    uint64_t ev_guard = 0;
    uint64_t ev_leak = 0;
    uint64_t ev_oom = 0;
    uint64_t ev_purge = 0;
    uint64_t ev_other = 0;

    while (!soak_stopping()) {
        uint64_t now = v_now_ns();
        if (now - g_start_ns >= g_duration_ns)
            break;
        if (now < next_tick) {
            v_sleep_ms(50);
            continue;
        }
        while (next_tick <= now)
            next_tick += 1000000000ull;

        uint64_t elapsed_ns = now - g_start_ns;
        unsigned pct = 0;
        soak_phase_t ph = soak_phase_at(elapsed_ns, &pct);
        (void)pct;

        wmm_stats_t st;
        bool st_ok = wmm_stats_snapshot(&st) == 0;
        if (!st_ok) {
            SOAK_FAIL("stats snapshot failed at %" PRIu64 "s\n",
                      elapsed_ns / 1000000000ull);
            memset(&st, 0, sizeof st);
        } else {
            unsigned n = v_stats_check(&st, &prev, prev_valid);
            n += v_stats_large_check(&st);

            if (!cfg.churn && prev_valid) {
                if (st.allocated_bytes < prev.allocated_bytes ||
                    st.deallocated_bytes < prev.deallocated_bytes) {
                    fprintf(stderr, "soak: cumulative counters decreased "
                            "without thread churn\n");
                    n++;
                }
            }
            soak_add_fail(n);

            if (st.live_bytes > peak_live)
                peak_live = st.live_bytes;

            prev = st;
            prev_valid = true;
        }

        v_event_counts_t ev;
        if (v_events_drain(&ev) != 0) {
            SOAK_FAIL("event drain failed\n");
        } else {
            ev_guard += ev.guard;
            ev_leak  += ev.leak;
            ev_oom   += ev.oom;
            ev_purge += ev.purge;
            ev_other += ev.other;

            if (ev.guard != 0)
                SOAK_FAIL("%" PRIu64 " guard event(s) with guard disabled\n",
                          ev.guard);
            if (ev.oom != 0)
                SOAK_FAIL("%" PRIu64 " OOM event(s) during soak\n", ev.oom);
            if (ev.other != 0)
                SOAK_FAIL("%" PRIu64 " unknown event type(s)\n", ev.other);
            if (ev.leak != 0 && !leak_noted) {
                fprintf(stderr, "soak: note: leak alerts seen during "
                        "growth; final quiesce decides\n");
                leak_noted = true;
            }
        }

        uint64_t rss = v_rss_kb();
        if (rss > peak_rss)
            peak_rss = rss;

        uint64_t cached = wmm_tcache_cached_total();
        if (cached > peak_cached)
            peak_cached = cached;

        uint64_t cache_bound = ((uint64_t)cfg.threads + 8u) *
                               v_tcache_max_per_thread();
        if (cached > cache_bound) {
            SOAK_FAIL("tcache objects %" PRIu64 " exceed bound %" PRIu64
                      "\n", cached, cache_bound);
        }

        if (!ramp_rss_set && ph != SOAK_PHASE_RAMP) {
            ramp_rss = rss;
            ramp_rss_set = true;
        }
        if (ramp_rss_set && !rss_gate_noted) {
            uint64_t growth = (uint64_t)cfg.rss_growth_mb << 10;
            if (rss > ramp_rss + growth) {
                SOAK_FAIL("RSS grew from %" PRIu64 " KB to %" PRIu64
                          " KB after ramp (limit %u MB)\n",
                          ramp_rss, rss, cfg.rss_growth_mb);
                rss_gate_noted = true;
            }
        }

        if (csv != NULL && st_ok) {
            fprintf(csv, "%" PRIu64 ",%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64
                    ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                    ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                    ",%" PRIu64 ",%" PRIu64 "\n",
                    elapsed_ns / 1000000000ull, soak_phase_name(ph),
                    st.live_bytes, st.active_bytes, st.frag_bytes,
                    st.high_water_live, st.live_objects, rss, cached,
                    soak_total_ops(), ev_guard, ev_leak, ev_oom, ev_purge,
                    ev_other);
            fflush(csv);
        }
    }

    atomic_store_explicit(&g_stop, true, memory_order_relaxed);

    for (unsigned i = 0; i < cfg.threads; i++)
        (void)pthread_join(g_workers[i].tid, NULL);
    if (churn_started)
        (void)pthread_join(churn_tid, NULL);

    soak_drain_all_rings();

    if (cfg.bg) {
        if (wmm_ctl("bg.disable") != 0)
            SOAK_FAIL("wmm_ctl(bg.disable) failed\n");
    }
    if (wmm_ctl("gc") != 0)
        SOAK_FAIL("wmm_ctl(gc) failed\n");
    if (wmm_ctl("purge") != 0)
        SOAK_FAIL("wmm_ctl(purge) failed\n");

    wmm_thread_exit();
    (void)wmm_ctl("purge");

    v_event_counts_t ev_final;
    if (v_events_drain(&ev_final) != 0) {
        SOAK_FAIL("final event drain failed\n");
    } else {
        ev_guard += ev_final.guard;
        ev_leak  += ev_final.leak;
        ev_oom   += ev_final.oom;
        ev_purge += ev_final.purge;
        ev_other += ev_final.other;
        if (ev_final.guard != 0 || ev_final.oom != 0 ||
            ev_final.other != 0) {
            SOAK_FAIL("final event check: guard=%" PRIu64 " oom=%" PRIu64
                      " other=%" PRIu64 "\n",
                      ev_final.guard, ev_final.oom, ev_final.other);
        }
    }

    wmm_stats_t fin;
    if (wmm_stats_snapshot(&fin) != 0) {
        SOAK_FAIL("final snapshot failed\n");
        memset(&fin, 0, sizeof fin);
    } else {
        unsigned n = v_stats_check(&fin, &prev, prev_valid);
        n += v_stats_large_check(&fin);
        soak_add_fail(n);

        if (fin.live_bytes != base.live_bytes) {
            SOAK_FAIL("final live_bytes=%" PRIu64 " baseline=%" PRIu64
                      " (delta=%lld)\n", fin.live_bytes, base.live_bytes,
                      (long long)(fin.live_bytes - base.live_bytes));
        }
        if (fin.live_objects != base.live_objects) {
            SOAK_FAIL("final live_objects=%" PRIu64 " baseline=%" PRIu64
                      "\n", fin.live_objects, base.live_objects);
        }
        if (fin.active_bytes != base.active_bytes) {
            SOAK_FAIL("final active_bytes=%" PRIu64 " baseline=%" PRIu64
                      "\n", fin.active_bytes, base.active_bytes);
        }
        for (szind_t i = 0; i <= WMM_NBINS; i++) {
            if (fin.cls[i].live != 0) {
                SOAK_FAIL("final cls[%u].live=%" PRIu64 " expected 0\n",
                          (unsigned)i, fin.cls[i].live);
            }
        }
    }

    uint64_t cached_final = wmm_tcache_cached_total();
    if (cached_final != 0) {
        SOAK_FAIL("final cached objects=%" PRIu64 " expected 0\n",
                  cached_final);
    }

    uint64_t ops = soak_total_ops();
    unsigned failures = atomic_load_explicit(&g_fail, memory_order_relaxed);
    uint64_t elapsed_sec = (v_now_ns() - g_start_ns) / 1000000000ull;

    if (csv != NULL)
        (void)fclose(csv);

    printf("SOAK_RESULT: %s seconds=%" PRIu64 " threads=%u ops=%" PRIu64
           " peak_live=%" PRIu64 " peak_rss_kb=%" PRIu64
           " peak_cached=%" PRIu64 " cached_final=%" PRIu64
           " leak_warnings=%" PRIu64 " purge_events=%" PRIu64
           " failures=%u\n",
           failures == 0 ? "PASS" : "FAIL", elapsed_sec, cfg.threads, ops,
           peak_live, peak_rss, peak_cached, cached_final, ev_leak,
           ev_purge, failures);

    return failures == 0 ? 0 : 1;
}
