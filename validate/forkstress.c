/* forkstress.c —— 活跃分配负载下反复 fork
 *
 * 工作线程持续分配/释放并周期性触发 GC 与统计快照;专用 forker
 * 线程(或主线程)在邻居线程仍忙碌时反复 fork。子进程在 atfork
 * 重建后的锁与线程缓存上继续分配,检查统计恒等式后用 _exit 退出。
 * 父进程给每个子进程设超时,避免 atfork 死锁把整个测试挂死。
 *
 * 子进程里不能用 stdio/exit:检查失败只返回非零,统一由父进程报告。
 */

#define _GNU_SOURCE

#include "validate_common.h"

#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>

#define FR_WORKER_SLOTS 128u
#define FR_CHILD_SLOTS  64u
#define FR_MAX_WORKERS  64u
#define FR_CHILD_TIMEOUT_NS 5000000000ull

typedef struct {
    unsigned  id;
    pthread_t tid;
    v_rng_t   rng;
    _Atomic uint64_t ops;
} fr_worker_t;

typedef struct {
    void  *p;
    size_t req;
} fr_obj_t;

typedef struct {
    unsigned seconds;
    unsigned workers;
    unsigned forks;
    unsigned child_ops;
    uint64_t seed;
    bool     guard;
    bool     fork_from_main;
    bool     bg;
    bool     workers_set;
} fr_cfg_t;

static fr_cfg_t       g_cfg;
static fr_worker_t   *g_workers;
static uint64_t       g_start_ns;
static uint64_t       g_deadline_ns;

static volatile sig_atomic_t g_stop_sig;
static _Atomic bool          g_stop;
static _Atomic bool          g_forker_done;
static _Atomic unsigned      g_ready_count;
static _Atomic unsigned      g_fail;
static _Atomic uint64_t      g_forks;
static _Atomic uint64_t      g_forks_ok;
static _Atomic uint64_t      g_forks_bad;
static _Atomic uint64_t      g_timeouts;

static uint64_t g_ev_guard;
static uint64_t g_ev_leak;
static uint64_t g_ev_oom;
static uint64_t g_ev_purge;
static uint64_t g_ev_other;
static wmm_stats_t g_tick_prev;
static bool        g_tick_prev_valid;

#define FR_FAIL(...)                                                     \
    do {                                                                 \
        (void)atomic_fetch_add_explicit(&g_fail, 1,                      \
                                        memory_order_relaxed);           \
        atomic_store_explicit(&g_stop, true, memory_order_relaxed);      \
        fprintf(stderr, "forkstress: " __VA_ARGS__);                     \
    } while (0)

static void fr_add_fail(unsigned n) {
    if (n != 0) {
        (void)atomic_fetch_add_explicit(&g_fail, n, memory_order_relaxed);
        atomic_store_explicit(&g_stop, true, memory_order_relaxed);
    }
}

static bool fr_stopping(void) {
    return g_stop_sig != 0 ||
           atomic_load_explicit(&g_stop, memory_order_relaxed);
}

static void fr_on_term(int sig) {
    (void)sig;
    g_stop_sig = 1;
}

static void fr_on_alarm(int sig) {
    (void)sig;
    _exit(124);
}

static void fr_install_signals(void) {
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = fr_on_term;
    if (sigaction(SIGINT, &sa, NULL) != 0 ||
        sigaction(SIGTERM, &sa, NULL) != 0) {
        perror("forkstress: sigaction");
        exit(2);
    }

    sa.sa_handler = fr_on_alarm;
    if (sigaction(SIGALRM, &sa, NULL) != 0) {
        perror("forkstress: sigaction SIGALRM");
        exit(2);
    }
}

static void *fr_map(size_t bytes) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (p == MAP_FAILED) {
        perror("forkstress: mmap");
        exit(2);
    }
    return p;
}

static size_t fr_pick_size(uint64_t r) {
    if ((r & 63u) == 0)
        return (size_t)1 << 20;
    if ((r & 15u) == 0)
        return 65536;
    return 8u + (size_t)((r >> 8) % 8184u);
}

static void fr_touch(void *p, size_t req) {
    unsigned char *b = (unsigned char *)p;

    b[0] = 0x5A;
    b[req - 1] = 0xA5;
}

static void fr_tick(void) {
    wmm_stats_t st;

    if (wmm_stats_snapshot(&st) != 0) {
        FR_FAIL("stats snapshot failed\n");
        return;
    }

    unsigned n = v_stats_check(&st, &g_tick_prev, g_tick_prev_valid);
    n += v_stats_large_check(&st);
    fr_add_fail(n);
    g_tick_prev = st;
    g_tick_prev_valid = true;

    v_event_counts_t ev;
    if (v_events_drain(&ev) != 0) {
        FR_FAIL("event drain failed\n");
        return;
    }

    g_ev_guard += ev.guard;
    g_ev_leak  += ev.leak;
    g_ev_oom   += ev.oom;
    g_ev_purge += ev.purge;
    g_ev_other += ev.other;

    if (ev.guard != 0)
        FR_FAIL("%" PRIu64 " guard event(s)\n", ev.guard);
    if (ev.oom != 0)
        FR_FAIL("%" PRIu64 " OOM event(s)\n", ev.oom);
    if (ev.other != 0)
        FR_FAIL("%" PRIu64 " unknown event type(s)\n", ev.other);
}

/* 子进程专用的只读检查:失败不打印,只用退出码告诉父进程 */
static bool fr_child_stats_ok(void) {
    wmm_stats_t st;

    if (wmm_stats_snapshot(&st) != 0)
        return false;

    uint64_t obj_sum = 0;
    for (szind_t i = 0; i <= WMM_NBINS; i++)
        obj_sum += st.cls[i].live;
    if (obj_sum != st.live_objects)
        return false;

    if (st.active_bytes < st.live_bytes)
        return false;
    if (st.frag_bytes != st.active_bytes - st.live_bytes)
        return false;
    if (st.high_water_live < st.live_bytes)
        return false;

    uint64_t small = 0;
    for (szind_t i = 0; i < WMM_NBINS; i++) {
        if (st.cls[i].size != wmm_bin_info[i].reg_size)
            return false;
        small += st.cls[i].live * st.cls[i].size;
    }
    if (st.cls[WMM_NBINS].size != 0)
        return false;
    if (small > st.live_bytes)
        return false;

    uint64_t large = st.live_bytes - small;
    if (large % (uint64_t)WMM_PAGE != 0)
        return false;
    if (large < st.cls[WMM_NBINS].live * (uint64_t)WMM_PAGE)
        return false;
    return true;
}

static int fr_child_run(unsigned ops, uint64_t seed) {
    v_rng_t rng = { v_seed(seed, 0xC1D0u) };
    void   *slots[FR_CHILD_SLOTS];
    size_t  reqs[FR_CHILD_SLOTS];
    unsigned n = 0;

    void *big = wmm_alloc((size_t)1 << 20);
    if (big == NULL || wmm_usable_size(big) < ((size_t)1 << 20))
        return 1;
    fr_touch(big, (size_t)1 << 20);
    wmm_free(big);

    void *al = wmm_aligned_alloc(4096, 8192);
    if (al == NULL || ((uintptr_t)al & 4095u) != 0 ||
        wmm_usable_size(al) < 8192)
        return 1;
    fr_touch(al, 8192);
    wmm_free(al);

    for (unsigned k = 0; k < ops; k++) {
        uint64_t r = v_rng_next(&rng);
        unsigned kind = (unsigned)(r % 100u);

        if (kind < 50 || n == 0) {
            if (n >= FR_CHILD_SLOTS) {
                unsigned j = (unsigned)((r >> 8) % n);
                wmm_free(slots[j]);
                slots[j] = slots[--n];
                reqs[j] = reqs[n];
                continue;
            }

            size_t req = fr_pick_size(r);
            void *p = wmm_alloc(req);
            if (p == NULL)
                return 1;
            fr_touch(p, req);
            slots[n] = p;
            reqs[n] = req;
            n++;
        } else if (kind < 80) {
            unsigned j = (unsigned)((r >> 8) % n);
            wmm_free(slots[j]);
            slots[j] = slots[--n];
            reqs[j] = reqs[n];
        } else if (kind < 90) {
            unsigned j = (unsigned)((r >> 8) % n);
            size_t nr = 1u + (size_t)((r >> 20) % 16384u);
            void *q = wmm_realloc(slots[j], nr);
            if (q == NULL)
                return 1;
            slots[j] = q;
            reqs[j] = nr;
            fr_touch(q, nr);
        } else if (kind < 96) {
            if (n >= FR_CHILD_SLOTS)
                continue;
            size_t req = fr_pick_size(r);
            unsigned char *p = (unsigned char *)wmm_calloc(1, req);
            if (p == NULL)
                return 1;
            if (p[0] != 0 || p[req - 1] != 0) {
                wmm_free(p);
                return 1;
            }
            fr_touch(p, req);
            slots[n] = p;
            reqs[n] = req;
            n++;
        } else {
            unsigned j = (unsigned)((r >> 8) % n);
            if (wmm_usable_size(slots[j]) < reqs[j])
                return 1;
        }
    }

    while (n != 0)
        wmm_free(slots[--n]);

    return fr_child_stats_ok() ? 0 : 1;
}

static void fr_wait_child(pid_t pid) {
    uint64_t deadline = v_now_ns() + FR_CHILD_TIMEOUT_NS;

    for (;;) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);

        if (r == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                (void)atomic_fetch_add_explicit(&g_forks_ok, 1,
                                                memory_order_relaxed);
            } else {
                (void)atomic_fetch_add_explicit(&g_forks_bad, 1,
                                                memory_order_relaxed);
                if (WIFSIGNALED(status)) {
                    FR_FAIL("child %d killed by signal %d\n", (int)pid,
                            WTERMSIG(status));
                } else {
                    FR_FAIL("child %d exited with code %d\n", (int)pid,
                            WEXITSTATUS(status));
                }
            }
            return;
        }

        if (r < 0 && errno != EINTR) {
            (void)atomic_fetch_add_explicit(&g_forks_bad, 1,
                                            memory_order_relaxed);
            FR_FAIL("waitpid(%d): %s\n", (int)pid, strerror(errno));
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
            return;
        }

        if (v_now_ns() >= deadline) {
            (void)atomic_fetch_add_explicit(&g_timeouts, 1,
                                            memory_order_relaxed);
            (void)atomic_fetch_add_explicit(&g_forks_bad, 1,
                                            memory_order_relaxed);
            FR_FAIL("child %d timed out after 5s; killed\n", (int)pid);
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
            return;
        }
        v_sleep_ms(5);
    }
}

static void fr_fork_loop(bool tick_self) {
    unsigned done = 0;
    uint64_t next_tick = v_now_ns() + 1000000000ull;

    while (!fr_stopping() && v_now_ns() < g_deadline_ns &&
           done < g_cfg.forks) {
        if (tick_self && v_now_ns() >= next_tick) {
            fr_tick();
            next_tick += 1000000000ull;
            if (fr_stopping())
                break;
        }

        uint64_t seed = v_seed(g_cfg.seed, done);
        pid_t pid = fork();

        if (pid < 0) {
            FR_FAIL("fork failed after %u children: %s\n", done,
                    strerror(errno));
            break;
        }
        if (pid == 0) {
            int rc = fr_child_run(g_cfg.child_ops, seed);
            _exit(rc);
        }

        (void)atomic_fetch_add_explicit(&g_forks, 1, memory_order_relaxed);
        fr_wait_child(pid);
        done++;
    }
}

static void *fr_forker_main(void *arg) {
    (void)arg;

    void *warm[8];
    for (unsigned i = 0; i < 8; i++) {
        warm[i] = wmm_alloc(64u + i * 16u);
        if (warm[i] == NULL) {
            FR_FAIL("forker warmup allocation failed\n");
            atomic_store_explicit(&g_forker_done, true, memory_order_relaxed);
            return NULL;
        }
    }
    for (unsigned i = 0; i < 8; i++)
        wmm_free(warm[i]);

    fr_fork_loop(false);
    atomic_store_explicit(&g_forker_done, true, memory_order_relaxed);
    return NULL;
}

static bool fr_capacity(fr_obj_t *slots, unsigned *nocc, uint64_t r) {
    if (*nocc < FR_WORKER_SLOTS)
        return true;

    unsigned j = (unsigned)((r >> 8) % *nocc);
    wmm_free(slots[j].p);
    slots[j] = slots[--*nocc];
    return false;
}

static void fr_worker_op(fr_worker_t *w, fr_obj_t *slots, unsigned *nocc,
                         uint64_t r) {
    unsigned kind = (unsigned)(r % 100u);

    if (kind < 55 || *nocc == 0) {
        if (!fr_capacity(slots, nocc, r))
            return;

        size_t req = fr_pick_size(r);
        void *p = wmm_alloc(req);
        if (p == NULL) {
            FR_FAIL("worker %u alloc(%zu) returned NULL\n", w->id, req);
            return;
        }
        fr_touch(p, req);
        slots[*nocc].p = p;
        slots[*nocc].req = req;
        (*nocc)++;
    } else if (kind < 85) {
        unsigned j = (unsigned)((r >> 8) % *nocc);
        wmm_free(slots[j].p);
        slots[j] = slots[--*nocc];
    } else if (kind < 90) {
        unsigned j = (unsigned)((r >> 8) % *nocc);
        size_t nr = 1u + (size_t)((r >> 20) % 65536u);
        void *q = wmm_realloc(slots[j].p, nr);
        if (q == NULL) {
            FR_FAIL("worker %u realloc(%zu) failed\n", w->id, nr);
            return;
        }
        slots[j].p = q;
        slots[j].req = nr;
        fr_touch(q, nr);
    } else if (kind < 95) {
        if (!fr_capacity(slots, nocc, r))
            return;

        size_t req = fr_pick_size(r);
        unsigned char *p = (unsigned char *)wmm_calloc(1, req);
        if (p == NULL) {
            FR_FAIL("worker %u calloc(%zu) returned NULL\n", w->id, req);
            return;
        }
        if (p[0] != 0 || p[req - 1] != 0) {
            FR_FAIL("worker %u calloc memory not zeroed\n", w->id);
            wmm_free(p);
            return;
        }
        fr_touch(p, req);
        slots[*nocc].p = p;
        slots[*nocc].req = req;
        (*nocc)++;
    } else if (kind < 98) {
        if (!fr_capacity(slots, nocc, r))
            return;

        size_t req = fr_pick_size(r);
        size_t align = (size_t)16u << (unsigned)(r % 9u);
        void *p = wmm_aligned_alloc(align, req);
        if (p == NULL || ((uintptr_t)p & (align - 1u)) != 0) {
            FR_FAIL("worker %u aligned_alloc(%zu, %zu) failed\n",
                    w->id, align, req);
            wmm_free(p);
            return;
        }
        fr_touch(p, req);
        slots[*nocc].p = p;
        slots[*nocc].req = req;
        (*nocc)++;
    } else {
        unsigned j = (unsigned)((r >> 8) % *nocc);
        if (wmm_usable_size(slots[j].p) < slots[j].req)
            FR_FAIL("worker %u usable_size below request\n", w->id);
    }
}

static void *fr_worker_main(void *arg) {
    fr_worker_t *w = (fr_worker_t *)arg;
    fr_obj_t slots[FR_WORKER_SLOTS];
    unsigned nocc = 0;
    wmm_stats_t prev;
    bool prev_valid = false;

    (void)atomic_fetch_add_explicit(&g_ready_count, 1,
                                    memory_order_release);

    while (!fr_stopping() && v_now_ns() < g_deadline_ns &&
           atomic_load_explicit(&g_fail, memory_order_relaxed) == 0) {
        uint64_t r = v_rng_next(&w->rng);

        fr_worker_op(w, slots, &nocc, r);

        uint64_t ops = atomic_fetch_add_explicit(&w->ops, 1,
                                                 memory_order_relaxed) + 1;
        if ((ops & 4095u) == 0) {
            if (wmm_ctl("gc") != 0)
                FR_FAIL("worker %u wmm_ctl(gc) failed\n", w->id);
        }
        if ((ops & 8191u) == 0) {
            wmm_stats_t st;
            if (wmm_stats_snapshot(&st) != 0) {
                FR_FAIL("worker %u snapshot failed\n", w->id);
            } else {
                unsigned n = v_stats_check(&st, &prev, prev_valid);
                n += v_stats_large_check(&st);
                fr_add_fail(n);
                prev = st;
                prev_valid = true;
            }
        }
    }

    for (unsigned i = 0; i < nocc; i++)
        wmm_free(slots[i].p);
    return NULL;
}

static uint64_t fr_total_ops(void) {
    uint64_t sum = 0;

    for (unsigned i = 0; i < g_cfg.workers; i++)
        sum += atomic_load_explicit(&g_workers[i].ops, memory_order_relaxed);
    return sum;
}

static void fr_usage(const char *arg0) {
    fprintf(stderr,
            "usage: %s [--seconds N] [--workers N] [--forks N] "
            "[--child-ops N]\n"
            "          [--guard] [--fork-from-main] [--no-bg] [--seed N]\n",
            arg0);
}

static int fr_parse(int argc, char **argv, fr_cfg_t *cfg) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        uint64_t v = 0;

        if (strcmp(a, "--help") == 0) {
            fr_usage(argv[0]);
            exit(0);
        }
        if (strcmp(a, "--guard") == 0) {
            cfg->guard = true;
            continue;
        }
        if (strcmp(a, "--fork-from-main") == 0) {
            cfg->fork_from_main = true;
            continue;
        }
        if (strcmp(a, "--no-bg") == 0) {
            cfg->bg = false;
            continue;
        }

        if (i + 1 >= argc) {
            fprintf(stderr, "forkstress: %s needs a value\n", a);
            return -1;
        }
        const char *val = argv[++i];

        if (strcmp(a, "--seconds") == 0) {
            if (v_parse_u64(val, 1, 86400, &v) != 0)
                return -1;
            cfg->seconds = (unsigned)v;
        } else if (strcmp(a, "--workers") == 0) {
            if (v_parse_u64(val, 1, FR_MAX_WORKERS, &v) != 0)
                return -1;
            cfg->workers = (unsigned)v;
            cfg->workers_set = true;
        } else if (strcmp(a, "--forks") == 0) {
            if (v_parse_u64(val, 1, 1000000, &v) != 0)
                return -1;
            cfg->forks = (unsigned)v;
        } else if (strcmp(a, "--child-ops") == 0) {
            if (v_parse_u64(val, 1, 1000000, &v) != 0)
                return -1;
            cfg->child_ops = (unsigned)v;
        } else if (strcmp(a, "--seed") == 0) {
            if (v_parse_u64(val, 0, UINT64_MAX, &v) != 0)
                return -1;
            cfg->seed = v;
        } else {
            fprintf(stderr, "forkstress: unknown option %s\n", a);
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    fr_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.seconds   = 10;
    cfg.workers   = 4;
    cfg.forks     = 100;
    cfg.child_ops = 512;
    cfg.seed      = 0xF0F0F0F000000001ull;
    cfg.bg        = true;

    if (fr_parse(argc, argv, &cfg) != 0) {
        fr_usage(argv[0]);
        return 2;
    }
    if (cfg.guard && !cfg.workers_set)
        cfg.workers = 2;

    g_cfg = cfg;
    g_start_ns = v_now_ns();
    g_deadline_ns = g_start_ns + (uint64_t)cfg.seconds * 1000000000ull;

    wmm_boot();
    fr_install_signals();
    alarm(cfg.seconds + 60u);

    if (cfg.guard && wmm_ctl("guard.enable") != 0) {
        fprintf(stderr, "forkstress: wmm_ctl(guard.enable) failed\n");
        return 2;
    }
    if (cfg.bg && wmm_bg_start() != 0) {
        perror("forkstress: wmm_bg_start");
        return 2;
    }

    wmm_stats_t base;
    if (wmm_stats_snapshot(&base) != 0) {
        fprintf(stderr, "forkstress: baseline snapshot failed\n");
        return 2;
    }

    g_workers = (fr_worker_t *)fr_map((size_t)cfg.workers *
                                      sizeof *g_workers);
    for (unsigned i = 0; i < cfg.workers; i++) {
        memset(&g_workers[i], 0, sizeof g_workers[i]);
        g_workers[i].id = i;
        g_workers[i].rng.state = v_seed(cfg.seed, i);
        atomic_init(&g_workers[i].ops, 0);

        if (pthread_create(&g_workers[i].tid, NULL, fr_worker_main,
                           &g_workers[i]) != 0) {
            fprintf(stderr, "forkstress: cannot start worker %u\n", i);
            return 2;
        }
    }

    while (atomic_load_explicit(&g_ready_count, memory_order_acquire) <
               cfg.workers &&
           !fr_stopping() && v_now_ns() < g_deadline_ns) {
        v_sleep_ms(5);
    }
    /* 给工作线程一小段时间把堆和缓存铺开,再开始 fork */
    v_sleep_ms(50);

    printf("forkstress: seconds=%u workers=%u forks=%u child_ops=%u "
           "guard=%d fork_from_main=%d bg=%d\n",
           cfg.seconds, cfg.workers, cfg.forks, cfg.child_ops,
           cfg.guard ? 1 : 0, cfg.fork_from_main ? 1 : 0, cfg.bg ? 1 : 0);
    fflush(stdout);

    pthread_t forker_tid;
    bool forker_started = false;

    if (cfg.fork_from_main) {
        fr_fork_loop(true);
    } else {
        if (pthread_create(&forker_tid, NULL, fr_forker_main, NULL) != 0) {
            fprintf(stderr, "forkstress: cannot start forker thread\n");
            return 2;
        }
        forker_started = true;

        uint64_t next_tick = v_now_ns() + 1000000000ull;
        while (!fr_stopping() && v_now_ns() < g_deadline_ns &&
               !atomic_load_explicit(&g_forker_done, memory_order_relaxed)) {
            if (v_now_ns() >= next_tick) {
                fr_tick();
                next_tick += 1000000000ull;
            }
            v_sleep_ms(50);
        }
    }

    atomic_store_explicit(&g_stop, true, memory_order_relaxed);

    if (forker_started)
        (void)pthread_join(forker_tid, NULL);
    for (unsigned i = 0; i < cfg.workers; i++)
        (void)pthread_join(g_workers[i].tid, NULL);

    if (cfg.bg) {
        if (wmm_ctl("bg.disable") != 0)
            FR_FAIL("wmm_ctl(bg.disable) failed\n");
    }
    if (cfg.guard) {
        if (wmm_ctl("guard.disable") != 0)
            FR_FAIL("wmm_ctl(guard.disable) failed\n");
    }
    if (wmm_ctl("gc") != 0)
        FR_FAIL("wmm_ctl(gc) failed\n");
    if (wmm_ctl("purge") != 0)
        FR_FAIL("wmm_ctl(purge) failed\n");

    wmm_thread_exit();
    (void)wmm_ctl("purge");

    v_event_counts_t ev_final;
    if (v_events_drain(&ev_final) != 0) {
        FR_FAIL("final event drain failed\n");
    } else {
        g_ev_guard += ev_final.guard;
        g_ev_leak  += ev_final.leak;
        g_ev_oom   += ev_final.oom;
        g_ev_purge += ev_final.purge;
        g_ev_other += ev_final.other;
        if (ev_final.guard != 0 || ev_final.oom != 0 ||
            ev_final.other != 0) {
            FR_FAIL("final events: guard=%" PRIu64 " oom=%" PRIu64
                    " other=%" PRIu64 "\n",
                    ev_final.guard, ev_final.oom, ev_final.other);
        }
    }

    wmm_stats_t fin;
    if (wmm_stats_snapshot(&fin) != 0) {
        FR_FAIL("final snapshot failed\n");
        memset(&fin, 0, sizeof fin);
    } else {
        unsigned n = v_stats_check(&fin, &g_tick_prev, g_tick_prev_valid);
        n += v_stats_large_check(&fin);
        fr_add_fail(n);

        if (fin.live_bytes != base.live_bytes) {
            FR_FAIL("final live_bytes=%" PRIu64 " baseline=%" PRIu64 "\n",
                    fin.live_bytes, base.live_bytes);
        }
        if (fin.live_objects != base.live_objects) {
            FR_FAIL("final live_objects=%" PRIu64 " baseline=%" PRIu64 "\n",
                    fin.live_objects, base.live_objects);
        }
        if (fin.active_bytes != base.active_bytes) {
            FR_FAIL("final active_bytes=%" PRIu64 " baseline=%" PRIu64 "\n",
                    fin.active_bytes, base.active_bytes);
        }
        for (szind_t i = 0; i <= WMM_NBINS; i++) {
            if (fin.cls[i].live != 0) {
                FR_FAIL("final cls[%u].live=%" PRIu64 " expected 0\n",
                        (unsigned)i, fin.cls[i].live);
            }
        }
    }

    uint64_t cached_final = wmm_tcache_cached_total();
    if (cached_final != 0)
        FR_FAIL("final cached objects=%" PRIu64 " expected 0\n",
                cached_final);

    uint64_t forks = atomic_load_explicit(&g_forks, memory_order_relaxed);
    uint64_t forks_ok = atomic_load_explicit(&g_forks_ok,
                                             memory_order_relaxed);
    uint64_t forks_bad = atomic_load_explicit(&g_forks_bad,
                                              memory_order_relaxed);
    uint64_t timeouts = atomic_load_explicit(&g_timeouts,
                                             memory_order_relaxed);
    unsigned failures = atomic_load_explicit(&g_fail, memory_order_relaxed);
    uint64_t ops = fr_total_ops();
    uint64_t elapsed = (v_now_ns() - g_start_ns) / 1000000000ull;

    printf("FORKSTRESS_RESULT: %s seconds=%" PRIu64 " forks=%" PRIu64
           " ok=%" PRIu64 " failed=%" PRIu64 " timeouts=%" PRIu64
           " workers=%u ops=%" PRIu64 " guard=%d leak_warnings=%" PRIu64
           " purge_events=%" PRIu64 " failures=%u\n",
           failures == 0 ? "PASS" : "FAIL", elapsed, forks, forks_ok,
           forks_bad, timeouts, cfg.workers, ops, cfg.guard ? 1 : 0,
           g_ev_leak, g_ev_purge, failures);

    return failures == 0 ? 0 : 1;
}
