/* wmm_ctl.c —— 字符串命令表形式的运行时控制接口 */

#include "wmm_internal.h"

#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>

typedef int (*wmm_ctl_fn_t)(const char *cmd, va_list ap);

/* 命令项:名称、处理函数与帮助文本 */
typedef struct {
    const char  *name;
    wmm_ctl_fn_t fn;
    const char  *help;
} wmm_ctl_cmd_t;

static int ctl_stats_snapshot(const char *cmd, va_list ap)
{
    (void)cmd;
    wmm_stats_t *st = va_arg(ap, wmm_stats_t *);
    return wmm_stats_snapshot(st);
}

static int ctl_stats_per_class(const char *cmd, va_list ap)
{
    (void)cmd;
    wmm_stats_t *st = va_arg(ap, wmm_stats_t *);
    if (st == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (szind_t i = 0; i < WMM_NBINS; i++) {
        wmm_bin_t *b = &g_bins[i];
        pthread_mutex_lock(&b->lock);
        st->cls[i].live    = b->live;
        st->cls[i].ndalloc = b->ndalloc;
        st->cls[i].size    = wmm_bin_info[i].reg_size;
        pthread_mutex_unlock(&b->lock);
    }

    pthread_mutex_lock(&g_guard_lock);
    for (szind_t i = 0; i < WMM_NBINS; i++) {
        st->cls[i].live    += g_guard_bins[i].live;
        st->cls[i].ndalloc += g_guard_bins[i].ndalloc;
    }
    pthread_mutex_unlock(&g_guard_lock);
    pthread_mutex_lock(&g_ext_lock);
    st->cls[WMM_NBINS].live    = g_large.live;
    st->cls[WMM_NBINS].ndalloc = atomic_load_explicit(&g_large.ndalloc,
                                                      memory_order_relaxed);
    st->cls[WMM_NBINS].size    = 0;
    pthread_mutex_unlock(&g_ext_lock);
    return 0;
}

static int ctl_purge(const char *cmd, va_list ap)
{
    (void)cmd;
    (void)ap;
    wmm_phys_purge_now();
    return 0;
}

static int ctl_gc(const char *cmd, va_list ap)
{
    (void)cmd;
    (void)ap;
    wmm_tcache_gc_all();
    return 0;
}

static int ctl_bg_enable(const char *cmd, va_list ap)
{
    (void)cmd;
    (void)ap;
    return wmm_bg_start();
}

static int ctl_bg_disable(const char *cmd, va_list ap)
{
    (void)cmd;
    (void)ap;
    return wmm_bg_stop();
}

static int ctl_madvise_mode(const char *cmd, va_list ap)
{
    (void)cmd;
    int mode = va_arg(ap, int);
    if (mode != MADV_DONTNEED && mode != MADV_FREE) {
        errno = EINVAL;
        return -1;
    }
    g_opt.madvise_mode = mode;
    return 0;
}

static int ctl_config_lg_page(const char *cmd, va_list ap)
{
    (void)cmd;
    (void)ap;
    return (int)WMM_LG_PAGE;
}

static int ctl_config_verbose(const char *cmd, va_list ap)
{
    (void)cmd;
    int on = va_arg(ap, int);
    g_opt.verbose = (on != 0);
    return 0;
}

/* 字节数向下取整到页;负值、超硬上限、非零不足一页都拒绝 */
static int ctl_config_tcache_max(const char *cmd, va_list ap)
{
    (void)cmd;
    int bytes = va_arg(ap, int);
    if (bytes < 0 ||
        (size_t)bytes > (size_t)WMM_TCACHE_LARGE_MAX_PAGES * WMM_PAGE ||
        (bytes != 0 && (size_t)bytes < WMM_PAGE)) {
        errno = EINVAL;
        return -1;
    }
    atomic_store_explicit(&g_opt.tcache_max_pages,
                          (uint32_t)((size_t)bytes / WMM_PAGE),
                          memory_order_relaxed);
    return 0;
}

static int ctl_config_tcache_budget(const char *cmd, va_list ap)
{
    (void)cmd;
    int bytes = va_arg(ap, int);
    if (bytes < 0 ||
        (size_t)bytes >
            (size_t)WMM_TCACHE_LARGE_MAX_BUDGET_PAGES * WMM_PAGE ||
        (bytes != 0 && (size_t)bytes < WMM_PAGE)) {
        errno = EINVAL;
        return -1;
    }
    atomic_store_explicit(&g_opt.tcache_budget_pages,
                          (uint64_t)((size_t)bytes / WMM_PAGE),
                          memory_order_relaxed);
    return 0;
}

static int ctl_sampling_enable(const char *cmd, va_list ap)
{
    (void)cmd;
    (void)ap;
    g_opt.sample_n = WMM_PROF_SAMPLE_N;
    return 0;
}

static int ctl_sampling_disable(const char *cmd, va_list ap)
{
    (void)cmd;
    (void)ap;
    g_opt.sample_n = 0;
    return 0;
}

static int ctl_prof_top(const char *cmd, va_list ap)
{
    (void)cmd;
    int k = va_arg(ap, int);
    wmm_prof_entry_t *out = va_arg(ap, wmm_prof_entry_t *);
    if (out == NULL || k < 1 || (unsigned)k > WMM_PROF_TOP_MAX) {
        errno = EINVAL;
        return -1;
    }
    return (int)wmm_prof_top(out, (unsigned)k);
}

static int ctl_leak_check(const char *cmd, va_list ap)
{
    (void)cmd;
    int k = va_arg(ap, int);
    wmm_prof_entry_t *out = va_arg(ap, wmm_prof_entry_t *);
    if (out == NULL || k < 1 || (unsigned)k > WMM_PROF_TOP_MAX) {
        errno = EINVAL;
        return -1;
    }
    return wmm_leak_check(k, out);
}

static int ctl_leak_dump(const char *cmd, va_list ap)
{
    (void)cmd;
    (void)ap;
    wmm_prof_entry_t rows[WMM_PROF_TOP_MAX];
    unsigned n = wmm_guard_leak_dump(rows, WMM_PROF_TOP_MAX);
    wmm_dprintf("leak.dump: %u call sites (guard objects only)\n", n);
    for (unsigned i = 0; i < n; i++)
        wmm_dprintf("  pc=%p objs=%zu bytes=%zu\n",
                    (void *)rows[i].pc, (size_t)rows[i].count,
                    (size_t)rows[i].bytes);
    return 0;
}

static int ctl_guard_enable(const char *cmd, va_list ap)
{
    (void)cmd;
    (void)ap;
    g_opt.guard = true;
    return 0;
}

static int ctl_guard_disable(const char *cmd, va_list ap)
{
    (void)cmd;
    (void)ap;
    wmm_guard_disable();
    return 0;
}

static int ctl_shm_create(const char *cmd, va_list ap)
{
    (void)cmd;
    const char *path = va_arg(ap, const char *);
    return wmm_shm_create(path);
}

/* 命令表;wmm_ctl 按名称线性查找,新增命令在此登记 */
static const wmm_ctl_cmd_t g_ctl_cmds[] = {
    { "stats.snapshot",      ctl_stats_snapshot, "取统计快照(同步合并各线程计数)" },
    { "stats.per_class",     ctl_stats_per_class, "取按尺寸类的直方图" },
    { "sampling.enable",     ctl_sampling_enable,  "调用点采样开(间隔 1000)" },
    { "sampling.disable",    ctl_sampling_disable, "调用点采样关" },
    { "prof.top",            ctl_prof_top,      "调用点 TOP 表(参数:k, 输出数组)" },
    { "leak.check",          ctl_leak_check,    "泄漏嫌疑的 TOP N 增量对比(参数:k, 输出数组)" },
    { "leak.dump",           ctl_leak_dump,     "全量 dump 存活对象的调用点(打日志)" },
    { "guard.enable",        ctl_guard_enable,  "内存错误检测开(默认关)" },
    { "guard.disable",       ctl_guard_disable, "检测关(排空隔离区与回收池)" },
    { "purge",               ctl_purge,         "立即回收一次空闲内存(同步)" },
    { "gc",                  ctl_gc,            "全部线程缓存强制收缩(含大缓存)" },
    { "bg.enable",           ctl_bg_enable,     "后台线程启(回收+聚合)" },
    { "bg.disable",          ctl_bg_disable,    "后台线程停(退出时 join)" },
    { "config.lg_page",      ctl_config_lg_page, "查询页大小 2^N(返回值即 N)" },
    { "config.madvise_mode", ctl_madvise_mode,  "切换内核回收方式 DONTNEED/FREE" },
    { "config.verbose",      ctl_config_verbose, "调试输出开关(参数:int 非 0 开)" },
    { "config.tcache_max",   ctl_config_tcache_max,
      "大缓存单块页数上限(参数:int 字节,0=关)" },
    { "config.tcache_budget", ctl_config_tcache_budget,
      "每线程大缓存页预算(参数:int 字节,0=不限)" },
    { "shm.create",          ctl_shm_create,    "创建共享监控区(参数:文件路径)" },
};

/* 查找并执行命令;空名称或未知命令返回 EINVAL */
int wmm_ctl(const char *cmd, ...)
{
    if (cmd == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < sizeof(g_ctl_cmds) / sizeof(g_ctl_cmds[0]); i++) {
        if (strcmp(cmd, g_ctl_cmds[i].name) == 0) {
            int rc;
            va_list ap;
            va_start(ap, cmd);
            rc = g_ctl_cmds[i].fn(cmd, ap);
            va_end(ap);
            return rc;
        }
    }
    errno = EINVAL;
    return -1;
}
