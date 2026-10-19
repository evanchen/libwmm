/* wmm_ctl.c —— 运行期控制的命令分发
 *
 * 采样、guard、purge、后台线程……所有可调参数不做成一个个导出
 * 函数,而是收敛成字符串命令经 wmm_ctl 分发:新能力只需在下面
 * 表里加一行,公共头文件的 ABI 不动——这是签名冻结后唯一的
 * 扩展通道。
 *
 * 已接线:stats.snapshot、stats.per_class、purge、bg.enable/disable、
 * config.madvise_mode。其余命令已登记、返回 ENOSYS,随各子系统
 * 实现逐条接入。分发对 15 项做线性扫描:控制命令不在热路径,
 * 不值得上哈希。约定:返回 0 成功;未知命令 -1/EINVAL;已登记
 * 未实现 -1/ENOSYS。
 *
 * 变参约定:带参数的命令由处理函数按自己的 ABI 取 va_arg;整型
 * 实参按 C 默认提升取 int(如 config.madvise_mode 传 MADV_* 常量),
 * 指针实参直接取指针(如 stats.* 传 wmm_stats_t *)。
 *
 * 本文件只做分发,不做业务;禁止任何内部分配(与库其余部分同约)。
 */
#include "wmm_internal.h"

#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>

/* 统一处理函数原型:cmd = 完整命令串(含点号分级),ap = 变参。 */
typedef int (*wmm_ctl_fn_t)(const char *cmd, va_list ap);

typedef struct {
    const char  *name;   /* 完整命令名(精确匹配) */
    wmm_ctl_fn_t fn;     /* 处理函数;落地前统一指向 ctl_enosys */
    const char  *help;   /* 一句话说明(命令即文档) */
} wmm_ctl_cmd_t;

static int ctl_enosys(const char *cmd, va_list ap)
{
    (void)cmd;
    (void)ap;
    errno = ENOSYS;
    return -1;
}

/* 取完整统计快照:变参为 wmm_stats_t *。委托 wmm_stats_snapshot,
 * NULL 检查与 errno 由它统一给。 */
static int ctl_stats_snapshot(const char *cmd, va_list ap)
{
    (void)cmd;
    wmm_stats_t *st = va_arg(ap, wmm_stats_t *);
    return wmm_stats_snapshot(st);
}

/* 只取按尺寸类的直方图(轻量):变参为 wmm_stats_t *,只填 cls[],
 * epoch 与线程表不动——监控端高频轮询直方图时不必付整快照的代价。
 * 每类读数与快照同源(锁内读),口径一致。 */
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
    pthread_mutex_lock(&g_ext_lock);
    st->cls[WMM_NBINS].live    = g_large.live;
    st->cls[WMM_NBINS].ndalloc = g_large.ndalloc;
    st->cls[WMM_NBINS].size    = 0;
    pthread_mutex_unlock(&g_ext_lock);
    return 0;
}

/* 同步回收一次:不看阈值,立即把全部 FREE_DIRTY 交还内核。 */
static int ctl_purge(const char *cmd, va_list ap)
{
    (void)cmd;
    (void)ap;
    wmm_phys_purge_now();
    return 0;
}

static int ctl_bg_enable(const char *cmd, va_list ap)
{
    (void)cmd;
    (void)ap;
    return wmm_bg_start();     /* 幂等;失败已置 errno */
}

static int ctl_bg_disable(const char *cmd, va_list ap)
{
    (void)cmd;
    (void)ap;
    return wmm_bg_stop();      /* 幂等;返回时线程已 join */
}

/* 切换 madvise 方式:变参为 int(MADV_DONTNEED / MADV_FREE)。
 * 只影响之后的 purge;已 RETAINED 的页不回改。并发写一个 int 在
 * x86-64 上原子;读到旧值的 purge 只是用上一轮的 advice,无害。 */
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

static const wmm_ctl_cmd_t g_ctl_cmds[] = {
    { "stats.snapshot",      ctl_stats_snapshot, "取统计快照(同步合并各线程计数)" },
    { "stats.per_class",     ctl_stats_per_class, "取按尺寸类的直方图" },
    { "sampling.enable",     ctl_enosys,        "调用点采样开(默认关)" },
    { "sampling.disable",    ctl_enosys,        "调用点采样关" },
    { "leak.check",          ctl_enosys,        "泄漏嫌疑的 TOP N 增量对比" },
    { "leak.dump",           ctl_enosys,        "全量 dump 存活对象的调用点" },
    { "guard.enable",        ctl_enosys,        "内存错误检测开(默认关)" },
    { "guard.disable",       ctl_enosys,        "检测关" },
    { "purge",               ctl_purge,         "立即回收一次空闲内存(同步)" },
    { "gc",                  ctl_enosys,        "全部线程缓存强制收缩" },
    { "bg.enable",           ctl_bg_enable,     "后台线程启(回收+聚合)" },
    { "bg.disable",          ctl_bg_disable,    "后台线程停(退出时 join)" },
    { "config.lg_page",      ctl_enosys,        "查询页大小 2^N" },
    { "config.madvise_mode", ctl_madvise_mode,  "切换内核回收方式 DONTNEED/FREE" },
    { "config.verbose",      ctl_enosys,        "调试输出开关" },
};

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
    errno = EINVAL;    /* 未知命令 */
    return -1;
}
