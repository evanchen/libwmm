/* monitor.c —— libwmm 共享监控区的独立只读读端
 *
 * 监控进程与目标进程完全解耦:按约定路径只读映射同一个文件,用
 * wmm_shm.h 里的 seqlock 读口取快照、事件环与发布时间,不链接
 * libwmm,也不向目标进程发任何请求。目标进程崩溃后文件仍在,最后
 * 一代数据还能拿来做现场分析。
 *
 * 输出分三层:
 *   - 控制台:每个周期一行内存曲线、尺寸类 TOP5 与线程净增量;
 *   - CSV:同一份快照按列落盘,事件只取最近一条的类型/线程/指针,
 *     完整事件明细在控制台,避免一个事件一行打断表结构;
 *   - 告警:live 高水位、frag 占比、新事件,统一写 stderr,不污染
 *     数据流。
 *
 * 用法:
 *   monitor <共享文件> [--pid PID] [--interval SEC] [--csv FILE]
 *           [--live-high BYTES] [--frag-pct PCT] [--no-events]
 *           [--once]
 *
 * 指标口径直接沿用 wmm_stats_t:live 是应用持有量(含线程缓存),
 * active 是已映射页,RSS 由内核 /proc/PID/statm 给出——三者的差是
 * 回答"内存去哪"的入口。per-class 字节数按 live × size 计算;大
 * 对象单对象大小不一,只聚合一行。线程表给的是累计量,监控端对
 * 相邻两代做差,才能看出"哪个线程正在涨"。
 *
 * 编译期只依赖两个头文件,不链库:线上读端可以单独升级、单独部署,
 * 与分配器版本互不牵连。
 */
#include "wmm_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* 小对象类数;布局与 wmm_stats_t.cls[] 的前 36 项一致 */
#define MON_SMALL_CLASSES 36u

/* 控制台显示尺寸类 TOP5;线程增量也取 TOP5 */
#define MON_TOP_CLASSES 5u
#define MON_TOP_THREADS 5u

/* attach 时回放最近几条事件:足够让人看到"刚刚发生过什么",又不会
   把历史环一次性刷满终端 */
#define MON_ATTACH_EVENTS 8u

/* 单个周期最多逐条展开多少条新事件;其余只报总数。一次回收或一次
   大风暴可能产生成百上千条事件,逐条打印会让监控本身成为瓶颈 */
#define MON_EVENT_DETAIL_MAX 20u

typedef struct {
    const char *shm_path;     /* 共享文件路径;只读打开 */
    const char *csv_path;     /* 可选 CSV 输出;NULL = 不落盘 */
    long        pid;          /* 目标进程号;0 = 不读 RSS */
    int         interval_ms;  /* 采样周期毫秒;下限 10ms */
    uint64_t    live_high;    /* live 告警阈值;0 = 关闭 */
    int         frag_pct;     /* frag/active 阈值百分比;0 = 关闭 */
    bool        show_events;  /* 事件明细与事件告警 */
    bool        once;         /* 采一次就退出 */
} mon_options_t;

typedef struct {
    uint64_t bytes;           /* live × size,TOP5 的排序键 */
    uint64_t live;            /* 本类存活对象数 */
    uint64_t size;            /* 本类单个对象的规格大小 */
    unsigned idx;             /* 0..35 = 小对象类;36 = 大对象聚合 */
} class_row_t;

typedef struct {
    uint64_t tid;             /* 线程号,与上一代按 tid 配对 */
    int64_t  net;             /* 本周期 alloc - free 的字节增量 */
    uint64_t alloc_delta;     /* 本周期累计分配字节增量 */
} thread_row_t;

/* 信号只置一个标志:主循环在每个周期边界检查。读端没有需要保护
   的共享状态,不需要在 handler 里做任何清理 */
static volatile sig_atomic_t g_stop;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static uint64_t mono_ns(void) {
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_ms(int ms) {
    struct timespec ts;

    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
        if (g_stop)
            break;
    }
}

/* 追加格式化:返回后 *len 始终是缓冲区里已有内容的长度,截断时
   也不会越过容量。用 vsnprintf 而不是逐个 snprintf 拼,避免编译器
   对固定格式做截断告警,也少写重复的长度维护 */
static void appendf(char *buf, size_t cap, size_t *len,
                    const char *fmt, ...) {
    va_list ap;

    if (*len >= cap)
        return;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    size_t un = (size_t)n;
    *len += un < cap - *len ? un : cap - *len - 1;
}

static void fmt_bytes(uint64_t v, char *buf, size_t cap) {
    static const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double x = (double)v;
    unsigned u = 0;

    while (x >= 1024.0 && u + 1 < sizeof(unit) / sizeof(unit[0])) {
        x /= 1024.0;
        u++;
    }
    if (u == 0)
        (void)snprintf(buf, cap, "%" PRIu64 "B", v);
    else
        (void)snprintf(buf, cap, "%.1f%s", x, unit[u]);
}

static void wall_clock(char *buf, size_t cap, bool iso) {
    struct timespec ts;
    struct tm tm;

    (void)clock_gettime(CLOCK_REALTIME, &ts);
    (void)localtime_r(&ts.tv_sec, &tm);
    (void)strftime(buf, cap, iso ? "%Y-%m-%dT%H:%M:%S" : "%H:%M:%S", &tm);
}

static const char *event_name(uint32_t type) {
    switch (type) {
    case 0xBC: return "uaf-pool-write";     /* 释放后、复用前被写 */
    case 0xBD: return "redzone-overflow";   /* 越界写踩到哨兵 */
    case 0xBE: return "bad-free";           /* 双释放/释放野指针 */
    case 0xBF: return "uaf-quarantine";     /* 隔离期被写 */
    case 0xDA: return "leak-alert";         /* live 持续上涨 */
    case 0xDB: return "oom";                /* 分配失败 */
    case 0xDC: return "purge";              /* 物理回收 */
    default:   return "unknown";
    }
}

static void usage(FILE *out) {
    fprintf(out,
        "用法: monitor <共享文件> [选项]\n"
        "  --pid PID           目标进程号;给出后从 /proc/PID/statm 读 RSS\n"
        "  --interval SEC      采样周期秒数,默认 1,可带小数(最小 0.01)\n"
        "  --csv FILE          启动时截断,之后每个周期追加一行 CSV\n"
        "  --live-high BYTES   live 字节超过阈值时告警;支持 K/M/G 后缀\n"
        "  --frag-pct PCT      frag/active 达到百分比时告警(1..100)\n"
        "  --events            显示并告警新事件(默认开)\n"
        "  --no-events         关闭事件明细与事件告警\n"
        "  --once              采一次就退出,便于脚本与事后读残留文件\n"
        "  --help              显示本帮助\n");
}

static int parse_bytes(const char *s, uint64_t *out) {
    char *end = NULL;
    uint64_t mul = 1;

    if (s == NULL || *s == '\0' || *s == '-')
        return -1;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno != 0 || end == s)
        return -1;
    if (*end != '\0') {
        if (strcmp(end, "K") == 0 || strcmp(end, "KiB") == 0)
            mul = 1ull << 10;
        else if (strcmp(end, "M") == 0 || strcmp(end, "MiB") == 0)
            mul = 1ull << 20;
        else if (strcmp(end, "G") == 0 || strcmp(end, "GiB") == 0)
            mul = 1ull << 30;
        else
            return -1;
    }
    if ((uint64_t)v > UINT64_MAX / mul)
        return -1;
    *out = (uint64_t)v * mul;
    return 0;
}

static int parse_interval(const char *s, int *ms) {
    char *end = NULL;

    errno = 0;
    double sec = strtod(s, &end);
    if (errno != 0 || end == s || *end != '\0' || !(sec > 0.0) || sec > 3600.0)
        return -1;
    long v = (long)(sec * 1000.0 + 0.5);
    if (v < 10)
        v = 10;
    *ms = (int)v;
    return 0;
}

static int parse_long(const char *s, long lo, long hi, long *out) {
    char *end = NULL;

    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi)
        return -1;
    *out = v;
    return 0;
}

/* 只读映射共享区;任何一项检查失败都返回 NULL 并打印原因。文件
   小于协议尺寸说明目标进程还没成功发布过,直接 mmap 会在访问尾部
   时收到 SIGBUS,所以必须在映射前挡掉 */
static const wmm_shm_t *map_shm(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        int e = errno;
        fprintf(stderr, "monitor: open %s: %s\n", path, strerror(e));
        return NULL;
    }

    struct stat sb;
    if (fstat(fd, &sb) != 0) {
        int e = errno;
        fprintf(stderr, "monitor: fstat %s: %s\n", path, strerror(e));
        (void)close(fd);
        return NULL;
    }
    if (sb.st_size < (off_t)WMM_SHM_SIZE) {
        fprintf(stderr,
                "monitor: %s 只有 %lld 字节,协议需要 %u;目标进程可能"
                "还没完成 shm.create\n",
                path, (long long)sb.st_size, (unsigned)WMM_SHM_SIZE);
        (void)close(fd);
        return NULL;
    }

    void *p = mmap(NULL, WMM_SHM_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    int e = errno;
    (void)close(fd);
    if (p == MAP_FAILED) {
        fprintf(stderr, "monitor: mmap %s: %s\n", path, strerror(e));
        return NULL;
    }

    const wmm_shm_t *h = (const wmm_shm_t *)p;
    if (h->magic != WMM_SHM_MAGIC) {
        fprintf(stderr,
                "monitor: magic 不匹配(文件 0x%08X,期望 0x%08X),"
                "这不是 libwmm 的共享区\n",
                (unsigned)h->magic, (unsigned)WMM_SHM_MAGIC);
        (void)munmap(p, WMM_SHM_SIZE);
        return NULL;
    }
    if (h->version != WMM_SHM_VERSION) {
        fprintf(stderr,
                "monitor: 协议版本不匹配(文件 %u,期望 %u);监控端与"
                "分配器必须按同一版布局解析\n",
                (unsigned)h->version, (unsigned)WMM_SHM_VERSION);
        (void)munmap(p, WMM_SHM_SIZE);
        return NULL;
    }
    return h;
}

static FILE *csv_open(const char *path) {
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        int e = errno;
        fprintf(stderr, "monitor: 创建 %s: %s\n", path, strerror(e));
        return NULL;
    }
    fprintf(f,
            "wall,epoch,live_bytes,active_bytes,frag_bytes,frag_pct,"
            "high_water_live,live_objects,allocated_bytes,deallocated_bytes,"
            "rss_kb,publish_age_ms,events_total,new_events,"
            "last_event_type,last_event_tid,last_event_ptr,last_event_cls,"
            "top_classes,top_threads\n");
    fflush(f);
    return f;
}

/* 读 /proc/PID/statm 的第二个字段(驻留页数)。目标退出后读不到,
   调用方显示 n/a;这不是错误,监控可以比目标活得更久 */
static int read_rss_kb(long pid, uint64_t *out) {
    if (pid <= 0)
        return -1;

    char path[64];
    (void)snprintf(path, sizeof(path), "/proc/%ld/statm", pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;

    unsigned long total = 0, resident = 0;
    int rc = fscanf(f, "%lu %lu", &total, &resident);
    (void)fclose(f);
    if (rc != 2)
        return -1;

    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0)
        return -1;
    *out = (uint64_t)resident * (uint64_t)pg / 1024u;
    return 0;
}

/* 小对象类按 live × reg_size 排序取 TOP。大对象不进这个列表:
   它们的单对象大小不一,cls[36].size 恒为 0,只能单独聚合 */
static unsigned top_classes(const wmm_stats_t *st, class_row_t *out,
                            unsigned max) {
    class_row_t all[MON_SMALL_CLASSES];
    unsigned n = 0;

    for (unsigned i = 0; i < MON_SMALL_CLASSES; i++) {
        uint64_t live = st->cls[i].live;
        uint64_t size = st->cls[i].size;

        if (live == 0 || size == 0)
            continue;
        all[n].bytes = live * size;
        all[n].live  = live;
        all[n].size  = size;
        all[n].idx   = i;
        n++;
    }

    unsigned take = n < max ? n : max;
    for (unsigned k = 0; k < take; k++) {
        unsigned best = k;

        for (unsigned j = k + 1; j < n; j++)
            if (all[j].bytes > all[best].bytes)
                best = j;
        class_row_t tmp = all[k];
        all[k] = all[best];
        all[best] = tmp;
    }
    if (take > 0)
        memcpy(out, all, take * sizeof(*out));
    return take;
}

/* 大对象存活字节 = live_bytes - 所有小对象类的字节和。这个差值是
   快照内部恒等式的一部分,不是估计;负数只可能出现在撕裂读上,
   这里夹到 0,避免打印荒唐值 */
static uint64_t large_live_bytes(const wmm_stats_t *st) {
    uint64_t small = 0;

    for (unsigned i = 0; i < MON_SMALL_CLASSES; i++)
        small += st->cls[i].live * st->cls[i].size;
    return st->live_bytes > small ? st->live_bytes - small : 0;
}

/* 尺寸类文本两种形态:控制台可读(带空格与单位),CSV 紧凑
   (size:live:bytes,分号分隔,不含逗号因此无需引号) */
static void build_class_text(const wmm_stats_t *st, char *buf, size_t cap,
                             bool human) {
    class_row_t top[MON_TOP_CLASSES];
    unsigned n = top_classes(st, top, MON_TOP_CLASSES);
    size_t len = 0;

    buf[0] = '\0';
    for (unsigned i = 0; i < n; i++) {
        char bytes[32];

        fmt_bytes(top[i].bytes, bytes, sizeof(bytes));
        if (human)
            appendf(buf, cap, &len, "%s%uB live=%" PRIu64 " (%s)",
                    i == 0 ? "" : " | ", (unsigned)top[i].size,
                    top[i].live, bytes);
        else
            appendf(buf, cap, &len, "%s%u:%" PRIu64 ":%" PRIu64,
                    i == 0 ? "" : ";", (unsigned)top[i].size,
                    top[i].live, top[i].bytes);
    }

    uint64_t lb = large_live_bytes(st);
    uint64_t lc = st->cls[MON_SMALL_CLASSES].live;

    if (lc > 0 || lb > 0) {
        char bytes[32];

        fmt_bytes(lb, bytes, sizeof(bytes));
        if (human)
            appendf(buf, cap, &len, "%slarge live=%" PRIu64 " (%s)",
                    n == 0 ? "" : " | ", lc, bytes);
        else
            appendf(buf, cap, &len, "%slarge:%" PRIu64 ":%" PRIu64,
                    n == 0 ? "" : ";", lc, lb);
    }
    if (len == 0)
        (void)snprintf(buf, cap, "%s", human ? "(empty)" : "");
}

/* 线程净增量:线程表本身只有累计 alloc/free 字节,按 tid 与上一
   代配对做差,才能回答"这个周期谁在涨"。新注册的线程没有基线,
   留到下一个周期;tid 为 0 的聚合桶不参与,否则多线程聚合会掩盖
   单个线程 */
static unsigned top_threads(const wmm_stats_t *cur, const wmm_stats_t *prev,
                            thread_row_t *out, unsigned max) {
    thread_row_t all[128];
    unsigned n = 0;

    if (prev == NULL)
        return 0;

    for (unsigned i = 0; i < 128; i++) {
        uint64_t tid = cur->threads[i].tid;
        if (tid == 0)
            continue;

        unsigned j;
        for (j = 0; j < 128; j++)
            if (prev->threads[j].tid == tid)
                break;
        if (j == 128)
            continue;

        uint64_t ca = cur->threads[i].alloc_bytes;
        uint64_t cf = cur->threads[i].free_bytes;
        uint64_t pa = prev->threads[j].alloc_bytes;
        uint64_t pf = prev->threads[j].free_bytes;
        if (ca < pa || cf < pf)
            continue;                     /* 计数回退:视为重建,跳过 */

        uint64_t da = ca - pa;
        uint64_t df = cf - pf;
        if (da > (uint64_t)INT64_MAX || df > (uint64_t)INT64_MAX)
            continue;
        int64_t net = (int64_t)da - (int64_t)df;
        if (net <= 0)
            continue;

        all[n].tid = tid;
        all[n].net = net;
        all[n].alloc_delta = da;
        n++;
    }

    unsigned take = n < max ? n : max;
    for (unsigned k = 0; k < take; k++) {
        unsigned best = k;

        for (unsigned j = k + 1; j < n; j++)
            if (all[j].net > all[best].net)
                best = j;
        thread_row_t tmp = all[k];
        all[k] = all[best];
        all[best] = tmp;
    }
    if (take > 0)
        memcpy(out, all, take * sizeof(*out));
    return take;
}

static void build_thread_text(const wmm_stats_t *cur, const wmm_stats_t *prev,
                              char *buf, size_t cap, bool human) {
    thread_row_t top[MON_TOP_THREADS];
    unsigned n = top_threads(cur, prev, top, MON_TOP_THREADS);
    size_t len = 0;

    buf[0] = '\0';
    if (prev == NULL) {
        (void)snprintf(buf, cap, "%s", human ? "(next interval)" : "");
        return;
    }
    for (unsigned i = 0; i < n; i++) {
        if (human) {
            char bytes[32];

            fmt_bytes((uint64_t)top[i].net, bytes, sizeof(bytes));
            appendf(buf, cap, &len, "%stid=%" PRIu64 " +%s",
                    i == 0 ? "" : " | ", top[i].tid, bytes);
        } else {
            appendf(buf, cap, &len, "%s%" PRIu64 ":%" PRId64,
                    i == 0 ? "" : ";", top[i].tid, top[i].net);
        }
    }
    if (len == 0)
        (void)snprintf(buf, cap, "%s", human ? "(none)" : "");
}

static void print_event(const wmm_event_t *ev, uint64_t now) {
    char age[32];

    if (ev->ts != 0 && now >= ev->ts) {
        uint64_t ms = (now - ev->ts) / 1000000ull;
        if (ms >= 1000)
            (void)snprintf(age, sizeof(age), "%.1fs", (double)ms / 1000.0);
        else
            (void)snprintf(age, sizeof(age), "%" PRIu64 "ms", ms);
    } else {
        (void)snprintf(age, sizeof(age), "n/a");
    }

    printf("  ev type=0x%02X %-16s tid=%u ptr=0x%" PRIxPTR
           " cls=%u pc0=0x%" PRIxPTR " pc1=0x%" PRIxPTR " age=%s\n",
           (unsigned)ev->type, event_name(ev->type), (unsigned)ev->tid,
           (uintptr_t)ev->ptr, (unsigned)ev->cls,
           (uintptr_t)ev->pc0, (uintptr_t)ev->pc1, age);
}

/* attach 时回放最近 limit 条:事件环是整环镜像,最新一条的全局
   编号是 total-1,按模容量取下标即可。超过环容量的历史已经被覆盖,
   不需要也不能再回放 */
static uint64_t show_recent_events(const wmm_shm_t *h, uint64_t total,
                                   wmm_event_t *latest, bool *have_latest) {
    wmm_event_t ring[WMM_SHM_EVENT_RING];
    uint64_t total2 = 0;

    if (wmm_shm_read_events(h, ring, &total2) != 0) {
        fprintf(stderr, "monitor: 事件环读取失败: %s\n", strerror(errno));
        return total;
    }
    if (total2 > total)
        total = total2;           /* 事件比快照新:以事件读段为准 */
    if (total == 0)
        return 0;

    uint64_t start = total > MON_ATTACH_EVENTS ? total - MON_ATTACH_EVENTS : 0;
    printf("  %s: 最近 %" PRIu64 " 条事件(共 %" PRIu64 " 条)\n",
           "attach", total - start, total);
    uint64_t now = mono_ns();
    for (uint64_t k = start; k < total; k++)
        print_event(&ring[k % WMM_SHM_EVENT_RING], now);

    *latest = ring[(total - 1) % WMM_SHM_EVENT_RING];
    *have_latest = true;
    return total;
}

/* 打印 [from, total) 的新事件,最多展开 limit 条。若丢失超过环容量
   (from 太老),从 total-RING 开始——更早的槽位已被覆盖,读出来
   只会是错位的旧事件 */
static void show_new_events(const wmm_shm_t *h, uint64_t from, uint64_t total,
                            wmm_event_t *latest, bool *have_latest) {
    wmm_event_t ring[WMM_SHM_EVENT_RING];
    uint64_t total2 = 0;

    if (wmm_shm_read_events(h, ring, &total2) != 0) {
        fprintf(stderr, "monitor: 事件环读取失败: %s\n", strerror(errno));
        return;
    }
    if (total2 < total)
        total = total2;           /* 读段更旧,以读段为准 */
    uint64_t window = total > WMM_SHM_EVENT_RING
        ? WMM_SHM_EVENT_RING : total;
    if (from < total - window)
        from = total - window;
    if (from >= total)
        return;

    uint64_t now = mono_ns();
    uint64_t n = total - from;
    uint64_t shown = n < MON_EVENT_DETAIL_MAX ? n : MON_EVENT_DETAIL_MAX;

    for (uint64_t k = 0; k < shown; k++)
        print_event(&ring[(from + k) % WMM_SHM_EVENT_RING], now);
    if (n > shown)
        printf("  ... 另有 %" PRIu64 " 条新事件未展开\n", n - shown);

    *latest = ring[(total - 1) % WMM_SHM_EVENT_RING];
    *have_latest = true;
}

int main(int argc, char **argv) {
    mon_options_t opt = {
        .shm_path     = NULL,
        .csv_path     = NULL,
        .pid          = 0,
        .interval_ms  = 1000,
        .live_high    = 0,
        .frag_pct     = 0,
        .show_events  = true,
        .once         = false,
    };

    if (argc == 2 && (strcmp(argv[1], "--help") == 0 ||
                      strcmp(argv[1], "-h") == 0)) {
        usage(stdout);
        return 0;
    }
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    opt.shm_path = argv[1];

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--pid") == 0 && i + 1 < argc) {
            if (parse_long(argv[++i], 1, 1L << 30, &opt.pid) != 0)
                goto bad_arg;
        } else if (strcmp(a, "--interval") == 0 && i + 1 < argc) {
            if (parse_interval(argv[++i], &opt.interval_ms) != 0)
                goto bad_arg;
        } else if (strcmp(a, "--csv") == 0 && i + 1 < argc) {
            opt.csv_path = argv[++i];
        } else if (strcmp(a, "--live-high") == 0 && i + 1 < argc) {
            if (parse_bytes(argv[++i], &opt.live_high) != 0)
                goto bad_arg;
        } else if (strcmp(a, "--frag-pct") == 0 && i + 1 < argc) {
            long v;
            if (parse_long(argv[++i], 1, 100, &v) != 0)
                goto bad_arg;
            opt.frag_pct = (int)v;
        } else if (strcmp(a, "--events") == 0) {
            opt.show_events = true;
        } else if (strcmp(a, "--no-events") == 0) {
            opt.show_events = false;
        } else if (strcmp(a, "--once") == 0) {
            opt.once = true;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            goto bad_arg;
        }
        continue;
bad_arg:
        fprintf(stderr, "monitor: 参数无效或缺少取值: %s\n", a);
        usage(stderr);
        return 2;
    }

    /* SIGTERM 与 SIGINT 同一路径:运维脚本更常用 TERM,两者都该让
       CSV 刷完最后一行再退出 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    (void)sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0)
        fprintf(stderr, "monitor: 信号处理安装失败: %s(继续运行)\n",
                strerror(errno));

    const wmm_shm_t *h = map_shm(opt.shm_path);
    if (h == NULL)
        return 1;

    FILE *csv = NULL;
    if (opt.csv_path != NULL) {
        csv = csv_open(opt.csv_path);
        if (csv == NULL) {
            (void)munmap((void *)h, WMM_SHM_SIZE);
            return 1;
        }
    }

    char hb[32];
    if (opt.live_high > 0)
        fmt_bytes(opt.live_high, hb, sizeof(hb));
    else
        (void)snprintf(hb, sizeof(hb), "off");
    fprintf(stderr,
            "monitor: %s (协议 v%u), pid=%ld, interval=%dms, "
            "live-high=%s, frag-pct=%s, events=%s%s%s\n",
            opt.shm_path, (unsigned)WMM_SHM_VERSION, opt.pid,
            opt.interval_ms, hb,
            opt.frag_pct > 0 ? "on" : "off",
            opt.show_events ? "on" : "off",
            opt.csv_path != NULL ? ", csv=" : "",
            opt.csv_path != NULL ? opt.csv_path : "");

    wmm_stats_t prev = {0};
    bool have_prev = false;
    uint64_t last_total = 0;
    bool have_total = false;
    wmm_event_t latest;
    bool have_latest = false;
    bool live_alerted = false;
    bool frag_alerted = false;

    memset(&latest, 0, sizeof(latest));
    bool last_ok = false;

    while (!g_stop) {
        uint64_t now = mono_ns();
        wmm_stats_t st;
        uint64_t total = 0;
        uint64_t pub_ts = 0;

        if (wmm_shm_read(h, &st, &total, &pub_ts) != 0) {
            fprintf(stderr,
                    "monitor: 快照读取失败(%s);写者可能停在写段中间,"
                    "本周期保持上一代数据\n",
                    strerror(errno));
        } else {
            uint64_t new_events = 0;

            last_ok = true;
            if (!have_total) {
                if (opt.show_events) {
                    uint64_t shown_total =
                        show_recent_events(h, total, &latest, &have_latest);
                    if (shown_total > total)
                        total = shown_total;
                    /* 事件环非空本身就是告警条件:attach 时把已在环里
                       的事件也报一次,避免脚本只在"新事件"上等待而漏掉
                       目标崩溃前留下的现场 */
                    if (have_latest)
                        fprintf(stderr,
                                "ALERT events: 共享区已有 %" PRIu64
                                " 条事件(attach 回放最近至多 %u 条)\n",
                                total, MON_ATTACH_EVENTS);
                }
                have_total = true;
            } else if (total < last_total) {
                fprintf(stderr,
                        "monitor: events_total 回退(%" PRIu64 " -> %" PRIu64
                        "),共享区疑似被重建;重新建立事件基线\n",
                        last_total, total);
                have_latest = false;
            } else {
                new_events = total - last_total;
            }
            last_total = total;

            if (opt.show_events && new_events > 0) {
                fprintf(stderr,
                        "ALERT events: %" PRIu64 " 条新事件(累计 %" PRIu64
                        ")\n",
                        new_events, total);
                show_new_events(h, total - new_events, total,
                                &latest, &have_latest);
            }

            uint64_t rss_kb = 0;
            bool have_rss = read_rss_kb(opt.pid, &rss_kb) == 0;

            char tbuf[32];
            wall_clock(tbuf, sizeof(tbuf), false);
            char live_b[32], active_b[32], frag_b[32], peak_b[32], rss_b[32];
            fmt_bytes(st.live_bytes, live_b, sizeof(live_b));
            fmt_bytes(st.active_bytes, active_b, sizeof(active_b));
            fmt_bytes(st.frag_bytes, frag_b, sizeof(frag_b));
            fmt_bytes(st.high_water_live, peak_b, sizeof(peak_b));
            if (have_rss)
                fmt_bytes(rss_kb * 1024ull, rss_b, sizeof(rss_b));
            else
                (void)snprintf(rss_b, sizeof(rss_b), "n/a");

            double frag_pct = st.active_bytes != 0
                ? (double)st.frag_bytes * 100.0 / (double)st.active_bytes
                : 0.0;
            uint64_t age_ms = pub_ts != 0 && now >= pub_ts
                ? (now - pub_ts) / 1000000ull : 0;

            char ev_suffix[48];
            if (opt.show_events)
                (void)snprintf(ev_suffix, sizeof(ev_suffix),
                               "(+%" PRIu64 ")", new_events);
            else
                ev_suffix[0] = '\0';

            printf("[%s] live=%s active=%s frag=%s(%.1f%%) objs=%" PRIu64
                   " peak=%s rss=%s alloc=%" PRIu64 " freed=%" PRIu64
                   " ev=%" PRIu64 "%s epoch=%" PRIu64 " age=%" PRIu64 "ms\n",
                   tbuf, live_b, active_b, frag_b, frag_pct,
                   st.live_objects, peak_b, rss_b,
                   st.allocated_bytes, st.deallocated_bytes, total,
                   ev_suffix, st.epoch, age_ms);

            char cls[512];
            char thr[512];
            build_class_text(&st, cls, sizeof(cls), true);
            build_thread_text(&st, have_prev ? &prev : NULL, thr, sizeof(thr),
                              true);
            printf("           cls: %s\n", cls);
            printf("           thr: %s\n", thr);

            if (csv != NULL) {
                char wall[32];
                char cls_csv[512];
                char thr_csv[512];

                wall_clock(wall, sizeof(wall), true);
                build_class_text(&st, cls_csv, sizeof(cls_csv), false);
                build_thread_text(&st, have_prev ? &prev : NULL,
                                  thr_csv, sizeof(thr_csv), false);
                fprintf(csv,
                        "%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                        ",%.2f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                        ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                        ",%u,%u,0x%" PRIxPTR ",%u,%s,%s\n",
                        wall, st.epoch, st.live_bytes, st.active_bytes,
                        st.frag_bytes, frag_pct, st.high_water_live,
                        st.live_objects, st.allocated_bytes,
                        st.deallocated_bytes,
                        have_rss ? rss_kb : 0, age_ms, total, new_events,
                        have_latest ? (unsigned)latest.type : 0u,
                        have_latest ? (unsigned)latest.tid : 0u,
                        have_latest ? (uintptr_t)latest.ptr : (uintptr_t)0,
                        have_latest ? (unsigned)latest.cls : 0u,
                        cls_csv, thr_csv);
                fflush(csv);
            }

            if (opt.live_high > 0 && st.live_bytes > opt.live_high) {
                if (!live_alerted) {
                    char lim[32];

                    fmt_bytes(opt.live_high, lim, sizeof(lim));
                    fprintf(stderr,
                            "ALERT live-high: live=%s 超过阈值 %s\n",
                            live_b, lim);
                    live_alerted = true;
                }
            } else {
                live_alerted = false;
            }

            if (opt.frag_pct > 0 && frag_pct >= (double)opt.frag_pct) {
                if (!frag_alerted) {
                    fprintf(stderr,
                            "ALERT frag: frag/active=%.1f%% 达到阈值 %d%% "
                            "(live=%s active=%s)\n",
                            frag_pct, opt.frag_pct, live_b, active_b);
                    frag_alerted = true;
                }
            } else {
                frag_alerted = false;
            }

            prev = st;
            have_prev = true;
        }

        if (opt.once)
            break;
        sleep_ms(opt.interval_ms);
    }

    if (csv != NULL)
        (void)fclose(csv);
    (void)munmap((void *)h, WMM_SHM_SIZE);
    if (opt.once && !last_ok)
        return 1;
    return 0;
}
