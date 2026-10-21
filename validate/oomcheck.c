/* oomcheck.c —— OOM 契约:返回 NULL、旧块保持有效、可配置 abort
 *
 * 窗口用 PROT_NONE 预留,boot 之后再设 RLIMIT_AS 不会让 MAP_FIXED
 * 失败:替换预留区间不增加 VmSize。这里直接把真实窗口填满:从
 * 64MB 开始,逐级降到 16KB,直到分配失败。数据页不碰,只写页表,
 * 64GB 窗口约 128MB PTE 内存。
 *
 * --bounded 是给 cgroup 用的低水位循环:在 live_mb 上限内不断分配、
 * 整块触碰、释放,结束时要求 live/active 回到基线。它只做内存压力,
 * 是否被杀由外层 cgroup 决定,本工具不自行判断。
 */

#define _GNU_SOURCE

#include "validate_common.h"

#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>

#define OOM_FILL_MAX       65536u
#define OOM_BOUNDED_SLOTS  8192u
#define OOM_MB             ((size_t)1 << 20)

typedef struct {
    void  *p;
    size_t size;
} oom_chunk_t;

typedef struct {
    bool window_fill;
    bool abort_child;
    bool bounded;
    unsigned live_mb;
    unsigned seconds;
    uint64_t seed;
    bool bounded_set;
    bool mode_set;
} oom_cfg_t;

static oom_cfg_t  g_cfg;
static unsigned   g_fail;
static oom_chunk_t *g_chunks;
static unsigned   g_nchunks;

static uint64_t g_ev_guard;
static uint64_t g_ev_leak;
static uint64_t g_ev_oom;
static uint64_t g_ev_purge;
static uint64_t g_ev_other;

static volatile sig_atomic_t g_stop_sig;

#define OOM_FAIL(...)                                                    \
    do {                                                                 \
        g_fail++;                                                        \
        fprintf(stderr, "oomcheck: " __VA_ARGS__);                       \
    } while (0)

static void oom_on_term(int sig) {
    (void)sig;
    g_stop_sig = 1;
}

static void oom_on_alarm(int sig) {
    (void)sig;
    _exit(124);
}

static void oom_install_signals(void) {
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = oom_on_term;
    if (sigaction(SIGINT, &sa, NULL) != 0 ||
        sigaction(SIGTERM, &sa, NULL) != 0) {
        perror("oomcheck: sigaction");
        exit(2);
    }

    sa.sa_handler = oom_on_alarm;
    if (sigaction(SIGALRM, &sa, NULL) != 0) {
        perror("oomcheck: sigaction SIGALRM");
        exit(2);
    }
}

static void *oom_map(size_t bytes) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (p == MAP_FAILED) {
        perror("oomcheck: mmap");
        exit(2);
    }
    return p;
}

static int oom_drain_events(void) {
    v_event_counts_t ev;

    if (v_events_drain(&ev) != 0) {
        OOM_FAIL("event drain failed\n");
        return -1;
    }

    g_ev_guard += ev.guard;
    g_ev_leak  += ev.leak;
    g_ev_oom   += ev.oom;
    g_ev_purge += ev.purge;
    g_ev_other += ev.other;

    if (ev.guard != 0)
        OOM_FAIL("%" PRIu64 " guard event(s)\n", ev.guard);
    if (ev.other != 0)
        OOM_FAIL("%" PRIu64 " unknown event type(s)\n", ev.other);
    return 0;
}

static void oom_usage(const char *arg0) {
    fprintf(stderr,
            "usage: %s [--window-fill] [--abort-child] [--bounded]\n"
            "          [--live-mb N] [--seconds N] [--seed N]\n"
            "default: --window-fill\n",
            arg0);
}

static int oom_parse(int argc, char **argv, oom_cfg_t *cfg) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        uint64_t v = 0;

        if (strcmp(a, "--help") == 0) {
            oom_usage(argv[0]);
            exit(0);
        }
        if (strcmp(a, "--window-fill") == 0) {
            cfg->window_fill = true;
            cfg->mode_set = true;
            continue;
        }
        if (strcmp(a, "--abort-child") == 0) {
            cfg->abort_child = true;
            cfg->mode_set = true;
            continue;
        }
        if (strcmp(a, "--bounded") == 0) {
            cfg->bounded = true;
            cfg->bounded_set = true;
            cfg->mode_set = true;
            continue;
        }

        if (i + 1 >= argc) {
            fprintf(stderr, "oomcheck: %s needs a value\n", a);
            return -1;
        }
        const char *val = argv[++i];

        if (strcmp(a, "--live-mb") == 0) {
            if (v_parse_u64(val, 1, 32768, &v) != 0)
                return -1;
            cfg->live_mb = (unsigned)v;
        } else if (strcmp(a, "--seconds") == 0) {
            if (v_parse_u64(val, 1, 3600, &v) != 0)
                return -1;
            cfg->seconds = (unsigned)v;
        } else if (strcmp(a, "--seed") == 0) {
            if (v_parse_u64(val, 0, UINT64_MAX, &v) != 0)
                return -1;
            cfg->seed = v;
        } else {
            fprintf(stderr, "oomcheck: unknown option %s\n", a);
            return -1;
        }
    }

    if (cfg->bounded && (cfg->window_fill || cfg->abort_child)) {
        fprintf(stderr, "oomcheck: --bounded cannot be combined with "
                "window-fill modes\n");
        return -1;
    }
    if (!cfg->mode_set)
        cfg->window_fill = true;
    return 0;
}

static void oom_chunk_push(void *p, size_t size) {
    if (g_nchunks >= OOM_FILL_MAX) {
        OOM_FAIL("fill bookkeeping array exhausted\n");
        return;
    }
    g_chunks[g_nchunks].p = p;
    g_chunks[g_nchunks].size = size;
    g_nchunks++;
}

static size_t oom_usable_sum(void) {
    size_t sum = 0;

    for (unsigned i = 0; i < g_nchunks; i++)
        sum += wmm_usable_size(g_chunks[i].p);
    return sum;
}

static void oom_free_all_chunks(void) {
    for (unsigned i = 0; i < g_nchunks; i++) {
        if (g_chunks[i].p != NULL)
            wmm_free(g_chunks[i].p);
        g_chunks[i].p = NULL;
    }
    g_nchunks = 0;
}

static void oom_final_checks(const wmm_stats_t *base, const char *mode) {
    wmm_tcache_flush_all();
    if (wmm_ctl("purge") != 0)
        OOM_FAIL("wmm_ctl(purge) failed\n");

    wmm_thread_exit();
    (void)wmm_ctl("purge");

    wmm_stats_t fin;
    if (wmm_stats_snapshot(&fin) != 0) {
        OOM_FAIL("final snapshot failed\n");
        return;
    }

    unsigned n = v_stats_check(&fin, NULL, false);
    n += v_stats_large_check(&fin);
    g_fail += n;

    if (fin.live_bytes != base->live_bytes)
        OOM_FAIL("%s: final live_bytes=%" PRIu64 " baseline=%" PRIu64 "\n",
                 mode, fin.live_bytes, base->live_bytes);
    if (fin.live_objects != base->live_objects)
        OOM_FAIL("%s: final live_objects=%" PRIu64 " baseline=%" PRIu64
                 "\n", mode, fin.live_objects, base->live_objects);
    if (fin.active_bytes != base->active_bytes)
        OOM_FAIL("%s: final active_bytes=%" PRIu64 " baseline=%" PRIu64
                 "\n", mode, fin.active_bytes, base->active_bytes);

    for (szind_t i = 0; i <= WMM_NBINS; i++) {
        if (fin.cls[i].live != 0)
            OOM_FAIL("%s: final cls[%u].live=%" PRIu64 " expected 0\n",
                     mode, (unsigned)i, fin.cls[i].live);
    }

    if (wmm_tcache_cached_total() != 0)
        OOM_FAIL("%s: final tcache objects remain\n", mode);
}

static void oom_fill_window(void) {
    static const size_t sizes[] = {
        (size_t)64 << 20, (size_t)16 << 20, (size_t)4 << 20,
        (size_t)1 << 20, (size_t)64 << 10, (size_t)16 << 10
    };
    unsigned failures = 0;

    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        for (;;) {
            if (g_stop_sig != 0) {
                OOM_FAIL("interrupted while filling window\n");
                return;
            }

            void *p = wmm_alloc(sizes[i]);
            if (p == NULL) {
                failures++;
                break;
            }
            oom_chunk_push(p, sizes[i]);

            if (g_nchunks >= OOM_FILL_MAX) {
                OOM_FAIL("window fill exceeded bookkeeping capacity\n");
                return;
            }
        }
    }

    printf("OOM_FILL: chunks=%u failed_requests=%u\n", g_nchunks, failures);
    fflush(stdout);

    if (failures == 0) {
        OOM_FAIL("window fill never failed; not actually full\n");
        return;
    }
}

static void oom_contract_checks(wmm_stats_t *base) {
    wmm_stats_t st;

    if (wmm_stats_snapshot(&st) != 0) {
        OOM_FAIL("snapshot after fill failed\n");
        return;
    }
    g_fail += v_stats_check(&st, NULL, false);
    g_fail += v_stats_large_check(&st);

    size_t live_sum = oom_usable_sum();
    if (st.live_bytes != base->live_bytes + (uint64_t)live_sum) {
        OOM_FAIL("live_bytes=%" PRIu64 " expected %" PRIu64 "\n",
                 st.live_bytes, base->live_bytes + (uint64_t)live_sum);
    }
    if (st.live_objects != base->live_objects + (uint64_t)g_nchunks) {
        OOM_FAIL("live_objects=%" PRIu64 " expected %" PRIu64 "\n",
                 st.live_objects, base->live_objects + (uint64_t)g_nchunks);
    }

    if (oom_drain_events() == 0 && g_ev_oom == 0)
        OOM_FAIL("no OOM event was logged while filling the window\n");

    void *p = wmm_calloc(1, OOM_MB);
    if (p != NULL) {
        OOM_FAIL("calloc(1, 1MB) succeeded with a full window\n");
        wmm_free(p);
    }

    if (g_nchunks == 0) {
        OOM_FAIL("no chunks to exercise realloc\n");
        return;
    }

    unsigned char *old = (unsigned char *)g_chunks[0].p;
    size_t old_size = g_chunks[0].size;

    old[0] = 0x11;
    old[old_size - 1] = 0x22;

    void *nr = wmm_realloc(old, (size_t)128 << 20);
    if (nr != NULL) {
        OOM_FAIL("realloc(128MB) succeeded with a full window\n");
        wmm_free(nr);
    } else if (old[0] != 0x11 || old[old_size - 1] != 0x22 ||
               wmm_usable_size(old) != old_size) {
        OOM_FAIL("realloc failure damaged the old 64MB block\n");
    }

    void *m = (void *)0x1;
    int rc = wmm_posix_memalign(&m, 4096, (size_t)64 << 20);
    if (rc != ENOMEM || m != NULL)
        OOM_FAIL("posix_memalign at full window: rc=%d out=%p\n", rc, m);

    errno = 0;
    if (wmm_aligned_alloc(4096, (size_t)64 << 20) != NULL ||
        errno != ENOMEM) {
        OOM_FAIL("aligned_alloc at full window did not fail with ENOMEM\n");
    }
}

static void oom_reuse_checks(void) {
    if (g_nchunks < 2) {
        OOM_FAIL("need two chunks for the reuse check\n");
        return;
    }

    wmm_free(g_chunks[0].p);
    g_chunks[0].p = NULL;
    wmm_free(g_chunks[1].p);
    g_chunks[1].p = NULL;

    void *again = wmm_alloc((size_t)64 << 20);
    if (again == NULL || wmm_usable_size(again) < ((size_t)64 << 20)) {
        OOM_FAIL("cannot reuse a freed 64MB extent\n");
        wmm_free(again);
        return;
    }

    void *small = wmm_alloc(OOM_MB);
    if (small == NULL) {
        OOM_FAIL("cannot split the remaining extent for 1MB\n");
        wmm_free(again);
        return;
    }

    void *third = wmm_alloc((size_t)64 << 20);
    if (third != NULL) {
        OOM_FAIL("64MB allocation succeeded with less than 64MB free\n");
        wmm_free(third);
    }

    wmm_free(again);
    wmm_free(small);
}

static int oom_abort_child_case(void);

static void oom_window_fill_case(void) {
    wmm_stats_t base;

    if (wmm_stats_snapshot(&base) != 0) {
        OOM_FAIL("baseline snapshot failed\n");
        return;
    }

    oom_fill_window();

    /* 窗口仍满时 fork:子进程第一次请求就会走 window_map 失败并 abort,
       不依赖子进程遍历已归还的 retained extent,结果确定且开销很小 */
    if (g_fail == 0 && g_cfg.abort_child)
        (void)oom_abort_child_case();

    if (g_fail == 0)
        oom_contract_checks(&base);
    if (g_fail == 0)
        oom_reuse_checks();

    oom_free_all_chunks();
    oom_final_checks(&base, "window-fill");

    if (oom_drain_events() == 0 && g_ev_oom == 0)
        OOM_FAIL("no OOM event visible after window-fill cleanup\n");

    printf("OOM_FILL_RESULT: %s chunks_freed events_oom=%" PRIu64
           " purge_events=%" PRIu64 " failures=%u\n",
           g_fail == 0 ? "PASS" : "FAIL", g_ev_oom, g_ev_purge, g_fail);
}

static int oom_abort_child_case(void) {
    pid_t pid = fork();

    if (pid < 0) {
        OOM_FAIL("fork for abort child failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        struct rlimit no_core = { 0, 0 };

        /* 不写核心转储:64GB 地址空间的核心文件会让父进程等待超时 */
        (void)setrlimit(RLIMIT_CORE, &no_core);
        g_opt.abort_oom = true;
        for (;;) {
            void *p = wmm_alloc((size_t)64 << 20);

            if (p == NULL)
                _exit(2);
        }
    }

    uint64_t deadline = v_now_ns() + 30000000000ull;

    for (;;) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);

        if (r == pid) {
            if (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT) {
                printf("OOM_ABORT: PASS signal=SIGABRT\n");
                return 0;
            }
            OOM_FAIL("abort child ended without SIGABRT "
                     "(exited=%d code=%d signalled=%d sig=%d)\n",
                     WIFEXITED(status), WEXITSTATUS(status),
                     WIFSIGNALED(status),
                     WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            return -1;
        }

        if (r < 0 && errno != EINTR) {
            OOM_FAIL("waitpid(abort child): %s\n", strerror(errno));
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
            return -1;
        }

        if (g_stop_sig != 0 || v_now_ns() >= deadline) {
            OOM_FAIL("abort child timed out; killed\n");
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
            return -1;
        }
        v_sleep_ms(10);
    }
}

static bool oom_read_first(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");

    if (f == NULL)
        return false;

    char line[256] = { 0 };
    bool ok = fgets(line, sizeof line, f) != NULL;
    (void)fclose(f);
    if (!ok)
        return false;

    line[strcspn(line, "\n")] = '\0';
    snprintf(out, n, "%s", line);
    return true;
}

static bool oom_read_key(const char *path, const char *key,
                         char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return false;

    size_t klen = strlen(key);
    char line[256];
    bool ok = false;

    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, key, klen) != 0 ||
            (line[klen] != ' ' && line[klen] != '\t'))
            continue;

        char *p = line + klen;
        while (*p == ' ' || *p == '\t')
            p++;
        size_t len = strcspn(p, "\n");
        if (len >= n)
            len = n - 1;
        memcpy(out, p, len);
        out[len] = '\0';
        ok = true;
        break;
    }

    (void)fclose(f);
    return ok;
}

static void oom_print_cgroup(void) {
    char path[1024] = "";
    FILE *f = fopen("/proc/self/cgroup", "r");

    if (f != NULL) {
        char line[1024];

        while (fgets(line, sizeof line, f) != NULL) {
            if (strncmp(line, "0::", 3) == 0) {
                line[strcspn(line, "\n")] = '\0';
                snprintf(path, sizeof path, "%s", line + 3);
                break;
            }
        }
        (void)fclose(f);
    }

    if (path[0] == '\0') {
        printf("CGROUP: unavailable (no cgroup v2 entry in "
               "/proc/self/cgroup)\n");
        return;
    }

    char dir[1400];
    char file[1600];
    char max[64] = "na", cur[64] = "na", peak[64] = "na";
    char kills[64] = "na";

    snprintf(dir, sizeof dir, "/sys/fs/cgroup%s", path);

    snprintf(file, sizeof file, "%s/memory.max", dir);
    (void)oom_read_first(file, max, sizeof max);

    snprintf(file, sizeof file, "%s/memory.current", dir);
    (void)oom_read_first(file, cur, sizeof cur);

    snprintf(file, sizeof file, "%s/memory.peak", dir);
    (void)oom_read_first(file, peak, sizeof peak);

    snprintf(file, sizeof file, "%s/memory.events", dir);
    (void)oom_read_key(file, "oom_kill", kills, sizeof kills);

    printf("CGROUP: path=%s max=%s current=%s peak=%s oom_kill=%s\n",
           path, max, cur, peak, kills);
}

static void oom_bounded_case(void) {
    static const size_t sizes[] = {
        4096, 8192, 16384, 32768, 65536, 262144, 1048576
    };
    oom_chunk_t *slots = (oom_chunk_t *)oom_map((size_t)OOM_BOUNDED_SLOTS *
                                                sizeof *slots);
    unsigned n = 0;
    size_t live = 0;
    size_t cap = (size_t)g_cfg.live_mb << 20;
    v_rng_t rng = { v_seed(g_cfg.seed, 0xB0) };
    uint64_t deadline = v_now_ns() +
                        (uint64_t)g_cfg.seconds * 1000000000ull;
    uint64_t ops = 0;

    wmm_stats_t base;
    if (wmm_stats_snapshot(&base) != 0) {
        OOM_FAIL("bounded baseline snapshot failed\n");
        return;
    }

    while (!g_stop_sig && v_now_ns() < deadline) {
        uint64_t r = v_rng_next(&rng);

        if (n != 0 && (live >= cap || n >= OOM_BOUNDED_SLOTS ||
                       (r % 3u) == 0)) {
            unsigned j = (unsigned)((r >> 8) % n);
            wmm_free(slots[j].p);
            live -= slots[j].size;
            slots[j] = slots[--n];
            ops++;
            continue;
        }

        size_t idx = (size_t)((r >> 16) % (sizeof sizes / sizeof sizes[0]));
        size_t req = sizes[idx];
        void *p = wmm_alloc(req);

        if (p == NULL) {
            OOM_FAIL("bounded allocation of %zu bytes failed\n", req);
            break;
        }

        size_t usable = wmm_usable_size(p);
        memset(p, (int)(r & 0xffu), req);
        slots[n].p = p;
        slots[n].size = usable;
        live += usable;
        n++;
        ops++;
    }

    for (unsigned i = 0; i < n; i++)
        wmm_free(slots[i].p);

    oom_final_checks(&base, "bounded");
    (void)oom_drain_events();

    oom_print_cgroup();
    printf("OOM_RESULT: %s mode=bounded live_mb=%u seconds=%u ops=%" PRIu64
           " failures=%u\n",
           g_fail == 0 ? "PASS" : "FAIL", g_cfg.live_mb, g_cfg.seconds,
           ops, g_fail);
}

int main(int argc, char **argv) {
    oom_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.live_mb = 64;
    cfg.seconds = 5;
    cfg.seed = 0x00C0FFEE00000001ull;

    if (oom_parse(argc, argv, &cfg) != 0) {
        oom_usage(argv[0]);
        return 2;
    }
    g_cfg = cfg;

    g_chunks = (oom_chunk_t *)oom_map((size_t)OOM_FILL_MAX *
                                      sizeof *g_chunks);

    wmm_boot();
    oom_install_signals();
    alarm(cfg.bounded ? cfg.seconds + 120u : 300u);

    if (cfg.bounded) {
        oom_bounded_case();
        return g_fail == 0 ? 0 : 1;
    }

    if (cfg.window_fill)
        oom_window_fill_case();
    else if (cfg.abort_child)
        (void)oom_abort_child_case();

    printf("OOM_RESULT: %s mode=%s failures=%u oom_events=%" PRIu64
           " purge_events=%" PRIu64 "\n",
           g_fail == 0 ? "PASS" : "FAIL",
           cfg.window_fill ? (cfg.abort_child ? "window-fill+abort"
                                              : "window-fill")
                           : "abort-child",
           g_fail, g_ev_oom, g_ev_purge);

    return g_fail == 0 ? 0 : 1;
}
