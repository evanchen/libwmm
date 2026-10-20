/* wmm_ctl.c —— 运行期控制的命令分发
 *
 * 采样、guard、purge、后台线程……所有可调参数不做成一个个导出
 * 函数,而是收敛成字符串命令经 wmm_ctl 分发:新能力只需在下面
 * 表里加一行,公共头文件的 ABI 不动——这是签名冻结后唯一的
 * 扩展通道。
 *
 * 已接线:stats.snapshot、stats.per_class、sampling.enable/disable、
 * prof.top、leak.check/dump、guard.enable/disable、purge、
 * bg.enable/disable、config.madvise_mode、shm.create。
 * 其余命令已登记、返回 ENOSYS,随各子系统实现逐条接入。分发对 17 项
 * 做线性扫描:控制命令不在热路径,不值得上哈希。约定:返回 0 成功;
 * 未知命令 -1/EINVAL;已登记未实现 -1/ENOSYS。
 *
 * 变参约定:带参数的命令由处理函数按自己的 ABI 取 va_arg;整型
 * 实参按 C 默认提升取 int(如 config.madvise_mode 传 MADV_* 常量),
 * 指针实参直接取指针(如 stats.* 传 wmm_stats_t *,shm.create 传
 * const char * 路径)。
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
    /* guard 对象按同一尺寸类并入:调用方看到的是"这一类用了多少",
       布局差异不该让统计断档 */
    pthread_mutex_lock(&g_guard_lock);
    for (szind_t i = 0; i < WMM_NBINS; i++) {
        st->cls[i].live    += g_guard_bins[i].live;
        st->cls[i].ndalloc += g_guard_bins[i].ndalloc;
    }
    pthread_mutex_unlock(&g_guard_lock);
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

/* 调用点采样开关:无变参。不开放任意的采样间隔——间隔是估计精度
 * 的旋钮,不是业务配置;固定 1000 让不同进程的 TOP 表口径一致,
 * 也避免一个荒谬的小间隔把分配路径拖垮。无参还保证 M14 的自动
 * 开启可以直接 wmm_ctl("sampling.enable") 调用,不会从变参表里
 * 读到不存在的实参。 */
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

/* 取调用点 TOP 表:变参为 int k 与 wmm_prof_entry_t *out(容量 ≥ k)。
 * 返回写入条数(0..k);k 越界或 out 为空 -1/EINVAL。wmm_prof_top
 * 内部先合并各线程缓冲,因此不依赖后台线程是否在运行。 */
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

/* 泄漏 B 档:变参为 int k 与 wmm_prof_entry_t *out(容量 ≥ k)。
 * 返回写入条数;首次调用建立基线并返回 0。k/out 校验与 prof.top
 * 同一口径。 */
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

/* 泄漏 A 档:无变参。全量遍历 guard 池里存活对象,按分配调用点
 * 聚合后直接打到 stderr。为什么走日志而不是回填调用方数组:命令
 * 语义是"dump",调用方要的是人可读的现场,不是又一次结构化查询
 * (结构化增量对比是 leak.check 的职责)。没有 guard 对象时打印
 * 0 条,不是错误。 */
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

/* guard 开关:打开后新分配走单对象 slab + redzone + 隔离;关闭时
 * 排空隔离区与回收池,已在应用手里的对象保持原布局直到各自释放 */
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

/* 创建共享监控区:变参为 const char * 文件路径。命令名带参数而
 * 不是分成 shm.create/shm.path 两条,是因为路径就是这条命令的
 * 全部参数,多一条命令只多一个状态。失败原样透传 errno。 */
static int ctl_shm_create(const char *cmd, va_list ap)
{
    (void)cmd;
    const char *path = va_arg(ap, const char *);
    return wmm_shm_create(path);
}

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
    { "gc",                  ctl_enosys,        "全部线程缓存强制收缩" },
    { "bg.enable",           ctl_bg_enable,     "后台线程启(回收+聚合)" },
    { "bg.disable",          ctl_bg_disable,    "后台线程停(退出时 join)" },
    { "config.lg_page",      ctl_enosys,        "查询页大小 2^N" },
    { "config.madvise_mode", ctl_madvise_mode,  "切换内核回收方式 DONTNEED/FREE" },
    { "config.verbose",      ctl_enosys,        "调试输出开关" },
    { "shm.create",          ctl_shm_create,    "创建共享监控区(参数:文件路径)" },
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
