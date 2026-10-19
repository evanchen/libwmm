/* wmm.h —— libwmm 公共 API
 *
 * libwmm 是一个学习型项目:用 ~3k 行 C 实现 jemalloc 的核心内存管理
 * 技术(尺寸类、slab、线程缓存、extent 合并),并补上后端服务最
 * 需要的"带仪表盘"能力:统计快照、调用点采样、内存错误检测、
 * 事件日志、跨进程实时监控。
 *
 * 本文件是库的全部对外入口。函数签名视为冻结:不增、不删、不改
 * (链接进服务后改签名 = ABI 破坏);新功能一律通过 wmm_ctl 的
 * 字符串命令扩展,那里才是预留的演进通道。
 *
 * 调用方必须遵守的契约(违反 = 未定义行为):
 *   - 指针只能归还分配它的分配器。与 libc malloc 混用
 *     (malloc 出来的指针交给 wmm_free,或反过来)是 UB;
 *   - 这些函数不是 async-signal-safe,信号 handler 内禁止调用;
 *   - 除 wmm_boot 外,任何入口在 wmm_boot 之前调用是未定义行为。
 *
 * 与 glibc malloc 家族一致的行为契约(专门的 API 测试逐条覆盖):
 *   - wmm_alloc 返回 16 字节对齐指针(仅 8 字节的最小类允许 8 字节);
 *   - 内存不足(OOM)返回 NULL(可配置为直接 abort,快速失败);
 *   - wmm_free(NULL) 无操作;wmm_realloc(NULL, n) 等价 wmm_alloc(n);
 *     wmm_realloc(p, 0) 等价 wmm_free(p) 并返回 NULL;
 *   - wmm_usable_size(p) 恒 ≥ 当初请求的大小。
 *
 * 布局冻结:wmm_stats_t / wmm_event_t 的字段顺序与类型同时是共享
 * 内存监控协议的线上格式——监控进程直接 mmap 同一文件按偏移读取,
 * 重排字段就会读错。文件尾的 _Static_assert 把两个结构的大小在
 * 编译期锁死:改布局的人会立刻编译失败,而不是上线后监控读花。
 */
#ifndef WMM_H_INCLUDED
#define WMM_H_INCLUDED

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 观测结构(先于函数声明:后者引用它们) ==================== */

/* wmm_stats_t —— 某一时刻的全局内存统计快照。
 *
 * 一致性设计(统计子系统是否"可信"全在这几行):
 *   - live 系列字段在各尺寸类的锁内更新,因此任意时刻
 *     "Σ cls[i].live * cls[i].size + 大对象存活字节 == live_bytes"
 *     恒成立——监控看到的数字内部自洽,不是估计值;
 *   - allocated/deallocated 合并自各线程的线程局部计数,合并瞬间
 *     允许微小偏差(换来的是分配快路径完全不加锁);
 *   - epoch 每次快照严格 +1:监控端用它对齐两次读数、求增量。
 */
typedef struct {
    uint64_t epoch;               /* 快照序号,单调递增 */
    uint64_t allocated_bytes;     /* 累计分配字节(按实际可用大小计,只增) */
    uint64_t deallocated_bytes;   /* 累计释放字节(只增) */
    uint64_t live_bytes;          /* 当前存活字节 = 应用持有的全部内存,
                                     含各线程缓存里的对象(见 wmm_internal.h) */
    uint64_t live_objects;        /* 当前存活对象总数(小对象 + 大对象) */
    uint64_t active_bytes;        /* 已向内核映射、尚未 munmap 的字节(页对齐)。
                                     live ≤ active 恒成立,差值 = frag_bytes */
    uint64_t frag_bytes;          /* active − live:碎片 + 空闲缓存 + 元数据。
                                     "RSS 比业务用量高"的归因就看这个数 */
    uint64_t high_water_live;     /* live_bytes 的历史峰值;定位瞬时峰值抖动 */
    struct {                      /* 按尺寸类的直方图,回答"内存被谁吃掉" */
        uint64_t live;            /*   本类当前存活对象数(含线程缓存) */
        uint64_t ndalloc;         /*   本类累计释放次数 */
        uint64_t size;            /*   本类单个对象的大小;[36] 恒为 0 */
    } cls[37];                    /* [0..35] = 36 个小对象类;[36] = 大对象
                                     聚合(大对象按页分配、大小不一,只计
                                     个数与字节,不细分) */
    struct {                      /* 按线程的分配计数,回答"哪个线程在涨" */
        uint64_t tid;             /*   线程号;聚合桶(见下)为 0 */
        uint64_t alloc_bytes;     /*   本线程累计分配字节 */
        uint64_t free_bytes;      /*   本线程累计释放字节 */
        uint64_t alloc_cnt;       /*   本线程累计分配次数 */
    } threads[128];               /* 活跃线程超过 128 个时,多出的全部合并进
                                     threads[127](tid 置 0):直方图有限,
                                     聚合一个桶也比丢数据好 */
} wmm_stats_t;

/* wmm_event_t —— 事件日志的单条记录。
 *
 * 事件环是排障时最快的线索来源:内存错误、泄漏告警、OOM、回收动作
 * 都往里写,应用或监控进程再 drain 出来看。每条带两帧调用点,
 * 出问题时能直接缩小到代码位置(需按 -fno-omit-frame-pointer 编译)。
 *
 * type 取值(线上格式,监控端按同样数值解析):
 *   0xBC 释放后内存被改过(UAF 已发生)  0xBD 越界写踩到 redzone
 *   0xBE 双释放/释放野指针              0xBF quarantine 期间被写(UAF)
 *   0xDA 泄漏告警                        0xDB OOM
 *   0xDC 执行了一次物理内存回收
 */
typedef struct {
    uint64_t  ts;                 /* 事件时间戳(CLOCK_MONOTONIC,纳秒) */
    uint32_t  type;               /* 事件类型,取值见上 */
    uint32_t  tid;                /* 触发线程号 */
    uintptr_t ptr;                /* 涉事指针(分配/释放/越界的地址) */
    uint32_t  cls;                /* 涉事对象的尺寸类;大对象 = 36 */
    uintptr_t pc0;                /* 调用点第 1 帧 return address;未开启
                                     采样时为 0 */
    uintptr_t pc1;                /* 调用点第 2 帧 return address */
} wmm_event_t;

/* ==================== 分配/释放(与 malloc 家族同语义) ====================
 * `-Wl,--wrap` 链接期接管只是把这组函数挂到 malloc 名下,语义不变。 */

/* 分配 size 字节;size==0 按 1 处理(glibc 同款)。
 * 返回 16 字节对齐指针;OOM 返回 NULL(或按配置 abort)。 */
void  *wmm_alloc(size_t size);

/* calloc 语义:n*size 溢出返回 NULL;成功时内存已清零。 */
void  *wmm_calloc(size_t n, size_t size);

/* realloc 语义:p==NULL 时等价 wmm_alloc(n);n==0 时等价 wmm_free(p)
 * 并返回 NULL;n 不超过可用大小时原地返回 p(不搬移);否则新分配、
 * 拷贝 min(旧可用大小, n) 字节、释放旧块。 */
void  *wmm_realloc(void *p, size_t size);

/* 释放 p;NULL 无操作。p 必须来自本分配器(跨分配器释放是 UB)。 */
void   wmm_free(void *p);

/* p 的可用字节数(小对象 = 所在尺寸类的规格大小;大对象 = 页对齐后
 * 的大小)。恒 ≥ 当初请求的大小;p==NULL 返回 0。 */
size_t wmm_usable_size(const void *p);

/* 对齐分配,POSIX 语义:alignment 非 2 的幂或小于 sizeof(void*) 时
 * 返回 EINVAL;内存不足返回 ENOMEM,且 *out 置 NULL。
 * 成功返回 0 并写 *out(本函数走返回码,不遵循 errno 约定)。 */
int    wmm_posix_memalign(void **out, size_t align, size_t size);

/* 对齐分配,C11 语义放宽版:alignment 非 2 的幂返回 NULL 并置
 * errno=EINVAL;OOM 返回 NULL 并置 ENOMEM。与 C11 标准不同,不要求
 * size 是 alignment 的倍数(glibc aligned_alloc 同款宽松)。 */
void  *wmm_aligned_alloc(size_t align, size_t size);

/* ==================== 生命周期/集成钩子 ==================== */

/* 初始化分配器:预留 VA 堆区、建页表、建元数据区、初始化 bin 表、
 * 注册 fork 钩子。必须在任何分配之前、单线程环境下调用;幂等,
 * 重复调用无效果。
 * 为什么要求显式初始化:这些是一次性的 mmap 系统调用,放在首次
 * 分配的路径上会让"第一次 malloc"的耗时不可预测。
 */
void wmm_boot(void);

/* 为本线程预先建好线程缓存,避免首次分配走慢路径。
 * 不调用也行——首次分配会自动创建,效果等价,仅首次稍慢。 */
void wmm_thread_init(void);

/* 归还本线程缓存的全部对象并从线程注册表注销。
 * 线程缓存是按线程 mmap 的整块内存,退出不归还 = 每个线程泄一块。
 * pthread 析构会自动调用;线程池高频创建/销毁的服务建议显式调用,
 * 时点更可控。 */
void wmm_thread_exit(void);

/* fork 三钩子(由 wmm_boot 经 pthread_atfork 注册)。
 * 为什么必须处理:fork 只带走调用线程,别的线程若持着锁,子进程
 * 里这把锁永远无人释放——所以子进程必须把所有锁当作未锁定重建;
 * 父进程侧则要在 fork 前先持住全部锁(freeze the world)防止
 * fork 半途状态被改。
 */
void wmm_atfork_prepare(void);
void wmm_atfork_parent(void);
void wmm_atfork_child(void);

/* ==================== 运行期控制与查询 ==================== */

/* 字符串命令分发——公共 API 冻结后唯一的扩展通道。可用命令:
 *   stats.snapshot            取统计快照(同步合并各线程计数)
 *   stats.per_class           取按尺寸类的直方图
 *   sampling.enable/disable   打开/关闭调用点采样(默认关)
 *   leak.check                泄漏嫌疑的 TOP N 增量对比
 *   leak.dump                 全量 dump 存活对象的调用点
 *   guard.enable/disable      打开/关闭内存错误检测(默认关)
 *   purge                     立即回收一次空闲内存(同步)
 *   gc                        强制全部线程的缓存收缩
 *   bg.enable/disable         启动/停止后台线程(回收+聚合)
 *   config.lg_page            查询页大小(2^N)
 *   config.madvise_mode       切换内核回收方式,变参为 int
 *                             (MADV_DONTNEED / MADV_FREE),只影响
 *                             之后的 purge;非法值返回 -1/EINVAL
 *   config.verbose            调试输出开关
 * 返回 0 成功;未知命令返回 -1 并置 errno=EINVAL;
 * 命令已登记但功能未实现返回 -1 并置 errno=ENOSYS。
 */
int  wmm_ctl(const char *cmd, ...);

/* 统计快照到 *st(同步合并各线程计数)。st==NULL 返回 -1/EINVAL;
 * 成功返回 0,且 st->epoch 比上次成功快照严格大 1。 */
int  wmm_stats_snapshot(wmm_stats_t *st);

/* 从事件环取出至多 n 条写入 buf(取出即消费,环中不再可见),
 * 实际条数写入 *got(可为 0)。buf==NULL 或 n==0 返回 -1/EINVAL。 */
int  wmm_event_drain(wmm_event_t *buf, size_t n, size_t *got);

/* ==================== 布局锁(共享内存协议的线上格式) ====================
 * 调整任一观测结构前,必须同步升监控协议版本号。
 * 0x13B8 = 5048 = 8 字节汇总头 + 37×24 直方图 + 128×32 线程表。
 * 0x30 = 48 字节事件记录。 */
_Static_assert(sizeof(wmm_stats_t) == 0x13B8, "wmm_stats_t layout is frozen (SHM ABI)");
_Static_assert(sizeof(wmm_event_t) == 0x30,   "wmm_event_t layout is frozen (SHM ABI)");

#ifdef __cplusplus
}
#endif

#endif /* WMM_H_INCLUDED */
