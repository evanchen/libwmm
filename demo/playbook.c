/* playbook.c —— 问题定位演练负载
 *
 * 七类线上症状各对应一个模式,配合 monitor/ 与共享监控区使用:
 *   leak      持续分配不释放,验证泄漏告警与调用点 TOP 增量;
 *   slowleak  低频小量泄漏,验证"天级缓慢上涨"也能被趋势窗口抓住;
 *   rss       大量分配后稀疏释放,制造 active 高、live 低的碎片现场;
 *   oob       写尾部 redzone,验证越界写事件;
 *   uaf       释放后再写,验证隔离期写入事件;
 *   double    连续释放两次,验证双释放事件;
 *   peak      批量起落,验证高水位与逐线程净增量。
 *
 * 每个模式都先创建共享监控区并启动后台线程,监控进程按同一路径只读
 * mmap 即可观察。故障模式故意写坏内存;本程序链接 release 库,不能
 * 套 ASan 运行,否则测试工具会先于分配器发现问题。
 *
 * 用法:
 *   playbook <模式> [--shm PATH] [--seconds N] [--size BYTES] [--rate N]
 *
 * 默认共享文件是 /tmp/libwmm-playbook-<模式>.shm,不同模式互不覆盖。
 * 进程打印 pid,监控命令用 --pid 读它的 RSS。
 *
 * 可执行文件用 -no-pie 构建:泄漏与 guard 事件里的 pc 是用户调用点,
 * 与文件虚拟地址一致,可以直接 addr2line -e build/demo/playbook 解析,
 * 不必再从 /proc/PID/maps 减加载基址。
 */
#include "wmm.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    const char *mode;       /* 子命令名,同时决定默认参数 */
    const char *shm_path;   /* 共享文件路径 */
    unsigned    seconds;    /* 运行秒数;故障模式是触发后保持的秒数 */
    size_t      size;       /* 对象大小 */
    size_t      rate;       /* leak/slowleak 每秒对象数;peak 每批对象数;
                               rss 总对象数 */
} demo_options_t;

/* 简单指针向量:元素本身用 libc 分配,不经过被测分配器,避免演练
   负载的元数据混进 libwmm 的统计与 RSS */
typedef struct {
    void  **v;              /* 元素数组;容量按 2 倍增长 */
    size_t  n;              /* 当前元素数 */
    size_t  cap;            /* 已分配容量 */
} ptrvec_t;

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static uint64_t mono_ms(void) {
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void sleep_ms(unsigned ms) {
    struct timespec ts;

    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR && !g_stop)
        ;
}

static bool vec_push(ptrvec_t *pv, void *p) {
    if (pv->n == pv->cap) {
        size_t cap = pv->cap != 0 ? pv->cap * 2 : 4096;
        void **nv = realloc(pv->v, cap * sizeof(*nv));
        if (nv == NULL)
            return false;
        pv->v = nv;
        pv->cap = cap;
    }
    pv->v[pv->n++] = p;
    return true;
}

static void touch(void *p, size_t size) {
    volatile unsigned char *b = (volatile unsigned char *)p;

    if (p == NULL || size == 0)
        return;
    b[0] = 0xA5;
    b[size - 1] = 0x5A;
}

static void print_stats(const char *tag) {
    wmm_stats_t st;

    if (wmm_stats_snapshot(&st) != 0) {
        fprintf(stderr, "playbook: stats.snapshot: %s\n", strerror(errno));
        return;
    }
    double frag = st.active_bytes != 0
        ? (double)st.frag_bytes * 100.0 / (double)st.active_bytes : 0.0;
    printf("[%s] live=%" PRIu64 " active=%" PRIu64 " frag=%.1f%% objs=%"
           PRIu64 " peak=%" PRIu64 "\n",
           tag, st.live_bytes, st.active_bytes, frag, st.live_objects,
           st.high_water_live);
}

/* leak.check 的第一次调用只建立基线,返回 0 行;之后每次给出与上次
   调用之间的增量。pc 是本进程的调用点,配合 -no-pie 可直接解析 */
static bool g_leak_primed;

static void print_leak_check(void) {
    wmm_prof_entry_t rows[8];
    errno = 0;
    int n = wmm_ctl("leak.check", 8, rows);
    if (n < 0) {
        fprintf(stderr, "playbook: leak.check: %s\n", strerror(errno));
        return;
    }
    if (!g_leak_primed) {
        printf("leak.check: baseline established\n");
        g_leak_primed = true;
        return;
    }
    printf("leak.check: %d 个增量调用点\n", n);
    for (int i = 0; i < n; i++) {
        if (rows[i].count == 0)
            continue;
        printf("  pc=0x%" PRIxPTR " bytes=%" PRIu64 " count=%" PRIu64
               "\n",
               (uintptr_t)rows[i].pc, rows[i].bytes, rows[i].count);
    }
}

static bool setup(const demo_options_t *o) {
    wmm_boot();

    if (wmm_ctl("shm.create", o->shm_path) != 0) {
        fprintf(stderr, "playbook: shm.create %s: %s\n",
                o->shm_path, strerror(errno));
        return false;
    }
    /* 后台线程负责每秒发布快照、推进 guard/泄漏窗口并合并采样;
       没有它,监控只能看到创建时的那一代数据 */
    if (wmm_ctl("bg.enable") != 0) {
        fprintf(stderr, "playbook: bg.enable: %s\n", strerror(errno));
        return false;
    }

    printf("playbook: mode=%s pid=%ld shm=%s size=%zu rate=%zu seconds=%u\n",
           o->mode, (long)getpid(), o->shm_path, o->size, o->rate,
           o->seconds);
    printf("monitor: ./build/monitor/monitor %s --pid %ld\n",
           o->shm_path, (long)getpid());
    fflush(stdout);
    return true;
}

/* 泄漏演练:按每秒 rate 个对象的预算匀速分配,不释放。匀速而不是
   每秒一次性爆发,是为了让相邻两个后台窗口都看到 live 上涨,连续
   上涨计数不会被"两次采样看到同一批"打断 */
static int mode_leak(const demo_options_t *o, bool slow) {
    if (wmm_ctl("sampling.enable") != 0) {
        fprintf(stderr, "playbook: sampling.enable: %s\n", strerror(errno));
        return 1;
    }

    ptrvec_t held = {0};
    const uint64_t deadline = mono_ms() + (uint64_t)o->seconds * 1000ull;
    uint64_t next_report = mono_ms() + 5000;
    uint64_t last_ms = mono_ms();
    double budget = 0.0;

    printf("%s: %zu B/object, %zu objects/s, %u s, 不释放\n",
           slow ? "slowleak" : "leak", o->size, o->rate, o->seconds);

    while (!g_stop && mono_ms() < deadline) {
        uint64_t now = mono_ms();
        budget += (double)o->rate * (double)(now - last_ms) / 1000.0;
        last_ms = now;

        size_t made = 0;
        while (budget >= 1.0 && !g_stop) {
            budget -= 1.0;
            void *p = wmm_alloc(o->size);
            if (p == NULL) {
                fprintf(stderr, "playbook: wmm_alloc 失败: %s\n",
                        strerror(errno));
                break;
            }
            touch(p, o->size);
            if (!vec_push(&held, p)) {
                wmm_free(p);
                break;
            }
            made++;
            /* 一次积压过多时先让出,下一轮再补;避免调度抖动后的
               瞬间爆发改变泄漏曲线的形状 */
            if (made >= 100000)
                break;
        }

        if (mono_ms() >= next_report) {
            print_leak_check();
            print_stats(slow ? "slowleak" : "leak");
            next_report += 5000;
        }
        sleep_ms(100);
    }

    print_leak_check();
    print_stats("final");
    printf("held=%zu objects, %zu bytes(进程退出前故意不释放)\n",
           held.n, held.n * o->size);
    return 0;
}

/* RSS 演练:先拉起一大片跨类占用,再释放其中 7/8、每个 slab 都留一个
   存活对象。每个 slab 都有活对象,整块不能退休,于是 active 停在
   峰值附近,live 只剩零头——两者之差就是 per-class 直方图要解释的
   碎片。这与"全部释放"不同:全空 slab 会被后台 purge 回收,反而
   看不到持久碎片 */
static int mode_rss(const demo_options_t *o) {
    ptrvec_t all = {0};
    ptrvec_t keep = {0};

    printf("rss: allocate %zu objects across four sizes, then free 7/8\n",
           o->rate);

    for (size_t i = 0; i < o->rate && !g_stop; i++) {
        size_t sz = o->size * (1 + (i & 3u));
        void *p = wmm_alloc(sz);
        if (p == NULL)
            break;
        touch(p, sz);
        if (!vec_push(&all, p)) {
            wmm_free(p);
            break;
        }
    }
    print_stats("rss-full");

    /* 稀疏保留:每 32 个对象留 4 个。尺寸按 i&3 轮转,而 i&31 的低
       两位恰好就是尺寸序号,所以四种尺寸各自每 8 次分配留一个。
       最大类(512B)的 slab 只有 8 个槽位,这个节拍保证每个 slab 至少
       有一个幸存者,退休判据"全空"不会成立 */
    for (size_t i = 0; i < all.n; i++) {
        if ((i & 31u) < 4) {
            if (!vec_push(&keep, all.v[i]))
                wmm_free(all.v[i]);
        } else {
            wmm_free(all.v[i]);
        }
    }
    free(all.v);

    /* 先收缓存再同步回收:把"全空 slab"当场清干净。剩下的 active
       就只由还有幸存者的 slab 撑着,后台线程之后也没有脏页可回收,
       监控读到的 frag 会稳定保持,而不是等一个周期就消失 */
    (void)wmm_ctl("gc");
    (void)wmm_ctl("purge");

    printf("rss: kept 1/8 objects; slabs with survivors pin active\n");
    print_stats("rss-frag");

    const uint64_t deadline = mono_ms() + (uint64_t)o->seconds * 1000ull;
    while (!g_stop && mono_ms() < deadline) {
        sleep_ms(1000);
        print_stats("rss");
    }

    for (size_t i = 0; i < keep.n; i++)
        wmm_free(keep.v[i]);
    free(keep.v);
    print_stats("final");
    return 0;
}

/* 峰值演练:每批拉起 rate 个对象,停一秒让监控至少发布一代,再全部
   释放。高水位在统计快照时更新,所以每批保持一秒,确保峰值至少被
   一次后台发布抓到 */
static int mode_peak(const demo_options_t *o) {
    const uint64_t deadline = mono_ms() + (uint64_t)o->seconds * 1000ull;

    printf("peak: %zu objects of %zu B per batch\n", o->rate, o->size);
    while (!g_stop && mono_ms() < deadline) {
        void **batch = calloc(o->rate, sizeof(*batch));
        if (batch == NULL) {
            fprintf(stderr, "playbook: calloc batch: %s\n", strerror(errno));
            return 1;
        }

        for (size_t i = 0; i < o->rate; i++) {
            void *p = wmm_alloc(o->size);
            if (p == NULL)
                break;
            touch(p, o->size);
            batch[i] = p;
        }
        print_stats("peak-high");

        for (int s = 0; s < 10 && !g_stop; s++)
            sleep_ms(100);

        for (size_t i = 0; i < o->rate; i++)
            if (batch[i] != NULL)
                wmm_free(batch[i]);
        free(batch);
        print_stats("peak-free");

        for (int s = 0; s < 10 && !g_stop; s++)
            sleep_ms(100);
    }
    print_stats("final");
    return 0;
}

/* guard 演练:三种故障各注入一次,然后让进程活够后台 tick 与发布
   周期。oob 在 free 时立即校验尾部哨兵;uaf 要等下一次隔离区检查;
   double 在第二次 free 当场记录 */
static int mode_guard(const demo_options_t *o, char kind) {
    if (wmm_ctl("guard.enable") != 0) {
        fprintf(stderr, "playbook: guard.enable: %s\n", strerror(errno));
        return 1;
    }

    void *p = NULL;
    bool alloc_failed = false;
    switch (kind) {
    case 'o': {
        p = wmm_alloc(o->size);
        if (p == NULL) {
            alloc_failed = true;
            break;
        }
        size_t usable = wmm_usable_size(p);
        /* 数据区尾部紧接 WMM_GUARD_RZ 字节 0xBD 哨兵;写 usable 处
           正是哨兵首字节,free 时逐字节校验 */
        ((volatile unsigned char *)p)[usable] = 0x55;
        printf("oob: wrote p[usable=%zu], free 时校验尾部 redzone\n",
               usable);
        wmm_free(p);
        p = NULL;
        break;
    }
    case 'u':
        p = wmm_alloc(o->size);
        if (p == NULL) {
            alloc_failed = true;
            break;
        }
        wmm_free(p);
        ((volatile unsigned char *)p)[0] = 0xAA;
        printf("uaf: freed %p then wrote p[0]; 等待隔离区检查\n", p);
        p = NULL;
        break;
    case 'd':
        p = wmm_alloc(o->size);
        if (p == NULL) {
            alloc_failed = true;
            break;
        }
        wmm_free(p);
        wmm_free(p);
        printf("double: freed %p twice\n", p);
        p = NULL;
        break;
    default:
        return 2;
    }
    if (alloc_failed)
        return 1;

    print_stats("guard-trigger");

    const uint64_t deadline = mono_ms() + (uint64_t)o->seconds * 1000ull;
    while (!g_stop && mono_ms() < deadline)
        sleep_ms(200);
    print_stats("final");
    return 0;
}

static void usage(FILE *out) {
    fprintf(out,
        "用法: playbook <模式> [选项]\n"
        "模式:\n"
        "  leak      持续分配不释放,触发泄漏告警与调用点增量\n"
        "  slowleak  低频小量泄漏,验证缓慢上涨趋势\n"
        "  rss       大量分配后稀疏释放,制造 active 高、live 低的碎片\n"
        "  oob       越界写尾部 redzone\n"
        "  uaf       释放后再写\n"
        "  double    双重释放\n"
        "  peak      批量起落,观察高水位与线程增量\n"
        "选项:\n"
        "  --shm PATH      共享文件路径(默认 /tmp/libwmm-playbook-<模式>.shm)\n"
        "  --seconds N     运行秒数;故障模式是触发后保持的秒数\n"
        "  --size BYTES    对象大小\n"
        "  --rate N        leak/slowleak 每秒对象数;peak 每批对象数;\n"
        "                  rss 总对象数\n"
        "  --help          显示本帮助\n");
}

static int parse_size(const char *s, size_t lo, size_t hi, size_t *out) {
    char *end = NULL;

    if (s == NULL || *s == '\0' || *s == '-')
        return -1;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi)
        return -1;
    *out = (size_t)v;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(stdout);
        return 0;
    }

    demo_options_t opt = {
        .mode    = argv[1],
        .seconds = 0,
        .size    = 0,
        .rate    = 0,
    };

    /* 每类症状的默认时长与强度:泄漏要跨过连续上涨窗口,guard 要
       等至少一次后台 tick 与发布,峰值至少走完两个起落 */
    if (strcmp(opt.mode, "leak") == 0) {
        opt.seconds = 15; opt.size = 1024;  opt.rate = 1024;
    } else if (strcmp(opt.mode, "slowleak") == 0) {
        /* 16KiB 走大对象路径,每次分配当场计入 live。小对象经
           tcache 批量填充:每秒 2 次分配要等十来秒才凑满一批,
           live 呈阶梯状,连续上涨窗口会被平台期打断,测不到告警。
           泄漏力度 32KiB/s,是 leak 模式(1MiB/s)的三十二分之一 */
        opt.seconds = 25; opt.size = 16384; opt.rate = 2;
    } else if (strcmp(opt.mode, "rss") == 0) {
        opt.seconds = 12; opt.size = 128;   opt.rate = 200000;
    } else if (strcmp(opt.mode, "oob") == 0) {
        opt.seconds = 6;  opt.size = 64;    opt.rate = 1;
    } else if (strcmp(opt.mode, "uaf") == 0) {
        opt.seconds = 6;  opt.size = 2560;  opt.rate = 1;
    } else if (strcmp(opt.mode, "double") == 0) {
        opt.seconds = 6;  opt.size = 2048;  opt.rate = 1;
    } else if (strcmp(opt.mode, "peak") == 0) {
        opt.seconds = 12; opt.size = 4096;  opt.rate = 2048;
    } else {
        fprintf(stderr, "playbook: 未知模式 '%s'\n", opt.mode);
        usage(stderr);
        return 2;
    }

    const char *shm = NULL;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--shm") == 0 && i + 1 < argc) {
            shm = argv[++i];
        } else if (strcmp(a, "--seconds") == 0 && i + 1 < argc) {
            size_t v;
            if (parse_size(argv[++i], 1, 3600, &v) != 0)
                goto bad_arg;
            opt.seconds = (unsigned)v;
        } else if (strcmp(a, "--size") == 0 && i + 1 < argc) {
            if (parse_size(argv[++i], 1, 1u << 20, &opt.size) != 0)
                goto bad_arg;
        } else if (strcmp(a, "--rate") == 0 && i + 1 < argc) {
            if (parse_size(argv[++i], 1, 5000000, &opt.rate) != 0)
                goto bad_arg;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            goto bad_arg;
        }
        continue;
bad_arg:
        fprintf(stderr, "playbook: 参数无效或缺少取值: %s\n", a);
        usage(stderr);
        return 2;
    }

    char shm_buf[256];
    if (shm == NULL) {
        (void)snprintf(shm_buf, sizeof(shm_buf),
                       "/tmp/libwmm-playbook-%s.shm", opt.mode);
        shm = shm_buf;
    }
    opt.shm_path = shm;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    (void)sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);

    if (!setup(&opt))
        return 1;

    int rc;
    if (strcmp(opt.mode, "leak") == 0)
        rc = mode_leak(&opt, false);
    else if (strcmp(opt.mode, "slowleak") == 0)
        rc = mode_leak(&opt, true);
    else if (strcmp(opt.mode, "rss") == 0)
        rc = mode_rss(&opt);
    else if (strcmp(opt.mode, "peak") == 0)
        rc = mode_peak(&opt);
    else if (strcmp(opt.mode, "oob") == 0)
        rc = mode_guard(&opt, 'o');
    else if (strcmp(opt.mode, "uaf") == 0)
        rc = mode_guard(&opt, 'u');
    else
        rc = mode_guard(&opt, 'd');

    printf("事后读数(文件保留最后一代数据): "
           "./build/monitor/monitor %s --once\n", opt.shm_path);
    return rc;
}
