/* wmm_ctl.c —— 运行期控制的命令分发
 *
 * 采样、guard、purge、后台线程……所有可调参数不做成一个个导出
 * 函数,而是收敛成字符串命令经 wmm_ctl 分发:新能力只需在下面
 * 表里加一行,公共头文件的 ABI 不动——这是签名冻结后唯一的
 * 扩展通道。
 *
 * 当前为骨架:全部命令已登记、统一返回 ENOSYS,随各子系统实现
 * 逐条接入。分发对 15 项做线性扫描:控制命令不在热路径,不值得
 * 上哈希。约定:返回 0 成功;未知命令 -1/EINVAL;已登记未实现
 * -1/ENOSYS。
 *
 * 本文件只做分发,不做业务;禁止任何内部分配(与库其余部分同约)。
 */
#include "wmm_internal.h"

#include <stdarg.h>
#include <string.h>

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

static const wmm_ctl_cmd_t g_ctl_cmds[] = {
    { "stats.snapshot",      ctl_enosys, "取统计快照(同步合并各线程计数)" },
    { "stats.per_class",     ctl_enosys, "取按尺寸类的直方图" },
    { "sampling.enable",     ctl_enosys, "调用点采样开(默认关)" },
    { "sampling.disable",    ctl_enosys, "调用点采样关" },
    { "leak.check",          ctl_enosys, "泄漏嫌疑的 TOP N 增量对比" },
    { "leak.dump",           ctl_enosys, "全量 dump 存活对象的调用点" },
    { "guard.enable",        ctl_enosys, "内存错误检测开(默认关)" },
    { "guard.disable",       ctl_enosys, "检测关" },
    { "purge",               ctl_enosys, "立即回收一次空闲内存(同步)" },
    { "gc",                  ctl_enosys, "全部线程缓存强制收缩" },
    { "bg.enable",           ctl_enosys, "后台线程启(回收+聚合)" },
    { "bg.disable",          ctl_enosys, "后台线程停(退出时 join)" },
    { "config.lg_page",      ctl_enosys, "查询页大小 2^N" },
    { "config.madvise_mode", ctl_enosys, "切换内核回收方式 DONTNEED/FREE" },
    { "config.verbose",      ctl_enosys, "调试输出开关" },
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
