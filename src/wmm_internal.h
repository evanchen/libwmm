/* wmm_internal.h —— 内部数据结构总装图
 *
 * 本头是全库唯一的结构定义点:尺寸类、页表、slab、bin、线程缓存、
 * 全局状态全部在此;各源文件只引用、不重定义。动手写任何模块的
 * 代码之前先在这里把结构定死——结构定了,实现就是填空。
 *
 * 学习定位:结构与算法取自 jemalloc 并裁剪(单 arena、无 hugepage
 * 子系统、无多级基树)。凡有对应物的地方,注释里都标了 jemalloc
 * 的来源,便于拿真源码对照学习。
 *
 * 五条全局不变量——任何实现不得违反;每条都写在它落地的字段旁:
 *   1. 对齐:slab 基址页对齐、尺寸类的对象大小是 16 的倍数(8 字节
 *      类除外),因此返回指针天然 16 字节对齐,不需要专门的对齐代码;
 *   2. 计数单点:每个尺寸类的存活计数只在持该类锁的代码里增减,
 *      这让"存活总字节数"在任何时刻精确成立,监控数据自身可信;
 *   3. 归属:指针只归还分配它的分配器,与 libc 混用是 UB;
 *   4. 自举:分配器的元数据绝不从分配器自己拿(初始化完成前调用
 *      分配函数 = 无限递归),一律来自 mmap 或专用 bump 区;
 *   5. 无泄漏:线程退出归还其缓存的全部对象;fork 后子进程重建锁。
 *
 * 编码约定:库内禁止 printf 与 malloc。日志走 wmm_dprintf(裸 write),
 * 元数据走 mmap / base 区——既避免递归,也不会把应用的内存统计搞脏。
 */
#ifndef WMM_INTERNAL_H_INCLUDED
#define WMM_INTERNAL_H_INCLUDED

#include "wmm.h"

#include <pthread.h>
#include <errno.h>

/* ==================== 常量(目标平台 Linux x86-64,4K 页) ==================== */

#define WMM_LG_PAGE 12                        /* log2(页大小) */
#define WMM_PAGE    ((size_t)1 << WMM_LG_PAGE)
#define WMM_NBINS   36                        /* 小对象尺寸类个数(8B..14336B) */
#define WMM_ALIGN   16                        /* 返回指针的对齐要求(x86-64
                                                 最严格基础类型 16 字节) */
#define WMM_SMALL_MAXCLASS  ((size_t)14336)   /* 小对象上限。再往上就是大对象,
                                                 界线画在 4 页:对象达到 4 页
                                                 时一个 slab 只装得下 1~2 个,
                                                 slab 机制没收益,不如直接按
                                                 整页分配( jemalloc 同界线) */
#define WMM_LARGE_MINCLASS  ((size_t)16384)   /* 大对象下限 = 4 页,整页分配 */
#define WMM_LOOKUP_MAX      4096              /* size→index 直接查表的上限:
                                                 4096 以下的请求一次访存查表
                                                 出结果;再往上改走公式,
                                                 避免查表随类数平方级膨胀 */

/* VA 堆区窗口大小:编译期常量,期望值。需要更大/更小的服务用编译
 * 命令行 -D 覆盖;它不进任何结构体、锁或对外 ABI,纯粹是 boot 时刻
 * 的映射参数。strict overcommit 机器上预留可能失败,boot 会减半
 * 重试,实际窗口以 g_heap_base/g_heap_end 为准(见下方注释)。 */
#ifndef WMM_HEAP_VA_SIZE
#define WMM_HEAP_VA_SIZE ((size_t)64 << 30)   /* 64GB */
#endif

/* ==================== 尺寸类(只读常量表,由 gen/gen_sc.py 生成) ====================
 *
 * 为什么需要尺寸类:把 1..14336 的连续请求空间离散成 36 个规格,
 * 分配只需"查表定规格";释放时不用任何对象头,由地址就能反查规格。
 * 与 jemalloc 默认配置逐值一致,是其 sc.c 算法的忠实移植。
 */

typedef uint32_t szind_t;                     /* 尺寸类编号;36 = 大对象路径 */
#define WMM_INDEX_LARGE WMM_NBINS

/* 单个尺寸类的只读参数(对应 jemalloc bin_info_t) */
typedef struct {
    uint32_t reg_size;    /* 本类每个对象的字节数 */
    uint32_t pgs;         /* 每个 slab 的页数 = LCM(页大小, reg_size)/页大小:
                             取最小的页数使得页刚好被对象整分、不留边角料。
                             由 reg_size 的奇数因子决定,恒在 1..7 页之间 */
    uint32_t slab_size;   /* slab 总字节数 = pgs * PAGE */
    uint32_t nregs;       /* 每个 slab 的对象总数 = slab_size / reg_size */
} wmm_bin_info_t;

/* 尺寸类表本体:生成头在此处包含(必须在上方 typedef 与 WMM_NBINS 之后)。
 * 表是 static const——每个编译单元各得一份只读副本(约 1KB),换来
 * 编译器在快路径里看得见表内容、可常量折叠,且无跨 TU 符号。 */
#include "wmm_sc.h"

/* ==================== 页表:地址 → 所属对象 的反向映射 ====================
 *
 * free(p) 只拿到裸指针,必须回答"p 属于哪个 slab / 哪个大块"。
 * 解法:堆放在一块预留的固定 VA 窗口里,每 4K 页配一个 8 字节项,
 * free 时查一次表就拿到归属。jemalloc 用多级基树(rtree)做同样
 * 的事以省内存;我们的窗口定死为编译期常量(64GB),平坦页表一次 mmap
 * 到位,换来实现简单 + 单次访存——页表的 VA 预留是窗口的 1/512,
 * 物理占用则只随实际用掉的堆惰性增长。这是本库"小而精"取舍的
 * 典型一处。
 */

typedef struct wmm_slab_s wmm_slab_t;   /* 前向声明:页表原型引用 */

/* 8 字节页表项;低 1 位 tag 区分指向 slab 还是 extent:
 *   tag=0 → wmm_slab_t*(小对象;slab 的每一页都登记同一指针)
 *   tag=1 → wmm_extent_t*(大对象;同样全页登记)
 *   raw==0 → 该页从未分配过。free 到这种地址 = 双释放/野指针,
 *            这就是检测的挂点,无需额外数据结构。
 * 为什么一项两用而不是两张表:free 路径一次读出即可分派,且省一半
 * 元数据。位域 tag 声明在前,gcc x86-64 小端下落最低有效位;
 * 判空与整体比较用 raw。
 * 页表本体在初始化时一次 mmap——元数据绝不能从分配器自己拿,
 * 那时它还没建好。 */
typedef union wmm_pte_u {
    struct {
        uintptr_t tag : 1;    /* 0 = slab,1 = extent */
        uintptr_t ptr : 63;   /* 指向 slab 头或 extent 结点(来自 base 区) */
    } s;
    uintptr_t raw;            /* 整体读写;0 = 未分配 */
} wmm_pte_t;

extern wmm_pte_t *g_pte;      /* 大小 = VA 窗口页数。mmap 后严禁 memset:
                                 匿名私有映射内核保证零填充,手写 memset
                                 等于把整个数组触碰一遍,凭空给 RSS 垫上
                                 页表全额(64GB 窗口 = 128MB)的底,还废掉
                                 "物理随用增长"的整个设计。零值即"未分配" */

/* 页表操作(wmm_core.c) */
wmm_pte_t wmm_pte_get(const void *p);          /* p 所在页的表项(下标 p>>LG_PAGE) */
void      wmm_pte_set(const void *p, wmm_pte_t v);
bool      wmm_pte_is_slab(const void *p, wmm_slab_t **out);  /* tag=0 ⇒ *out=slab,真 */
bool      wmm_pte_is_extent(const void *p, void **out_ext);  /* tag=1 ⇒ *out=extent,真 */

/* ==================== slab:把连续页切成等大对象 ====================
 *
 * 一个 slab = pgs 个连续页 = nregs 个等大对象。对象本身不带任何
 * 头部:空闲对象的首 4 字节复用为侵入式空闲链(存下一个空闲对象
 * 的编号),其余元数据集中在下面这个头里——头从 base 区分配,
 * 一点不占堆页,对象密度做到了理论最大。
 * jemalloc 的对应物是 extent 头 + bitmap;bitmap 支持按位查找但
 * 多约 200 行代码,freelist 一样 O(1) 且批量时从链头连续摘,够用。
 *
 * 特例:4096/8192/12288 三个类每 slab 只有 1~2 个对象,分配即满、
 * 释放即空,永远不会进 nonfull 堆——状态机不必为它们写插入分支
 * (jemalloc bin.c 同样跳过)。它们存在的意义:请求 4K 差一点、
 * 8K 差一点时给出近-exact 的规格,而不是向上凑整页浪费近一页。
 */
struct wmm_slab_s {
    uint32_t    szind;      /* 所属尺寸类编号(0..35)。free 反查拿到 slab
                               后,从这里得知对象大小与归属 bin */
    uint32_t    nfree;      /* 空闲对象数。两个边界驱动 bin 状态机:
                               0 = 满(挂 full 链),nregs = 全空(整块
                               还给物理层) */
    uint32_t    free_head;  /* 空闲链头(对象编号);UINT32_MAX = 链空 */
    uint32_t    pad;        /* 占位凑 8 字节对齐 */
    void       *addr;       /* slab 基址(页对齐)。对象 r 的地址 =
                               addr + r*reg_size,编号与地址一次除法互推 */
    /* ---- 挂链(以下三组链都在所属 bin 的锁内维护) ---- */
    wmm_slab_t *full_prev, *full_next;        /* 满双向链:满 slab 仍需 O(1)
                                                 摘除(释放使它由满转非满) */
    wmm_slab_t *ph_prev, *ph_next, *ph_child; /* 非满 slab 的地址序配对堆
                                                 (jemalloc edata_heap 同款,
                                                 堆顶 = 最低地址;开发期可
                                                 先退化成 ph_prev/ph_next
                                                 有序双链,行为等价) */
    void       *ext;        /* 承载本 slab 的物理块(extent)。全空归还时
                               由此找到 extent 做相邻合并 */
};

/* ==================== bin:一个尺寸类的 slab 管理者 ====================
 *
 * 全局共 36 个 bin,即"单 arena"。jemalloc 默认按 CPU 数开多个
 * arena 分摊锁竞争;我们砍到 1 个,靠线程缓存吸收绝大部分并发
 * ——bin 锁只在批量填充/刷出的慢路径出现,竞争窗口天然很小。
 */
typedef struct {
    pthread_mutex_t lock;        /* 保护本类全部 slab 链与下方所有计数。
                                    为什么每类一把而不是全局一把:不同
                                    尺寸类的操作互不相交,共享只会排队 */
    wmm_slab_t     *slabcur;     /* 当前供分配的热 slab。多数分配都落在
                                    同一个 slab 上,免去每次去 nonfull 堆
                                    里找;满了才换人 */
    wmm_slab_t     *full_head;   /* 满 slab 双向链头。只用于 O(1) 摘除,
                                    不做查找 */
    wmm_slab_t     *nonfull_root;   /* 非满 slab 的地址序配对堆根;分配永远
                                    取堆顶(最低地址)——同批对象地址
                                    聚集,cache 局部性好,低地址的 slab
                                    也更容易先腾空整块归还 */
    /* ---- 观测计数(全部在本锁内更新,因此任意时刻恒自洽) ---- */
    uint64_t live;               /* 本类存活对象数,含各线程缓存里的——
                                    对象进缓存对 arena 而言仍算存活。
                                    只在持本锁的代码里增减:这条纪律
                                    保证"Σ live*size == live_bytes"全局
                                    恒等,是监控数据可信的根 */
    uint64_t ndalloc;            /* 累计释放次数(慢路径逐个 + 批量刷出) */
    uint64_t nslabs;             /* 累计创建过的 slab 数 */
    uint64_t curslabs;           /* 当前在管 slab 数 = slabcur + full 链 +
                                    nonfull 堆 的全部 */
} wmm_bin_t;

extern wmm_bin_t g_bins[WMM_NBINS];   /* 全局单 arena,初始化时全零 */

/* ==================== 线程缓存:无锁快路径 ====================
 *
 * 分配/释放的绝大多数不该碰 bin 锁。每线程为每个尺寸类缓存一小
 * 撇对象,弹栈即分配、压栈即释放(对应 jemalloc cache_bin;这里
 * 用 head 索引计数而非其"低 16 位指针水位"技巧——少一层心智负担,
 * 语义等价,快路径同样只有一次比较)。
 */
typedef struct {
    void    **stack;        /* 栈数组,位于 tcache 整块 mmap 内。
                               stack[0..head) 是缓存的对象,栈顶在
                               stack[head-1] */
    uint16_t ncached_max;   /* 容量上限。经验公式 clamp(2*nregs, 20, 200):
                               小对象一 slab 装得多就多缓存点;上下限
                               防止单线程缓存无界膨胀(线程多时,
                               每线程多缓存 1KB,千线程就是 1GB) */
    uint16_t head;          /* 当前缓存数;0 = 空,分配转慢路径批量补充 */
    uint16_t low_water;     /* 自上次 GC 以来 head 到过的最小值。
                               GC 用 (head − low_water) 估计"这段时间
                               真正用掉多少":偶尔用一次的类这个值
                               接近 0,GC 就能识别并回收缓存 */
    uint16_t nfill;         /* 下次批量补充的目标数,自适应:触发过 GC
                               说明缓存供过于求,减半;频繁耗尽说明
                               供不应求,翻倍(封顶容量的一半) */
    uint64_t nrequests;     /* 本类累计请求数,仅供观测;批量操作时一次
                               加总,避免逐对象累加拖慢慢路径 */
} wmm_cache_bin_t;

/* wmm_tcache_t —— 线程缓存主体,兼任线程注册表节点。
 *
 * 内存来源:一次 mmap 一整块,布局 = [ 本结构 | 各 bin 的栈数组 ]。
 * 为什么不用分配器自己分配:首次分配时它还没建好(自举死锁);
 * 且线程会反复创建销毁,必须能整块 munmap 还给内核——base 区只进
 * 不出,不适合生命周期跟着线程走的内存。
 *
 * 结构本身链入全局 g_threads 链:统计快照要合并各线程的计数、
 * wmm_ctl("gc") 要遍历所有线程的缓存,都靠这张链找到人。
 * 链指针的改动必须持 g_threads_lock;计数字段只由本线程写、
 * 不加原子——快照读到瞬时旧值无妨,换来快路径零同步开销。
 */
typedef struct wmm_tcache_s wmm_tcache_t;
struct wmm_tcache_s {
    wmm_tcache_t *prev, *next;  /* g_threads 双向链;g_threads_lock 保护 */
    uint32_t      tid;          /* 线程号缓存,事件与快照直接用 */
    bool          enabled;      /* 本线程缓存开关;关掉则直走 bin——
                                   给"怀疑缓存导致问题"的调试留后门 */
    /* ---- 线程局部计数(唯一写者 = 本线程,非原子) ---- */
    uint64_t      t_alloc_bytes;   /* 累计分配字节(按可用大小计) */
    uint64_t      t_free_bytes; /* 累计释放字节 */
    uint32_t      t_alloc_cnt;  /* 累计分配次数 */
    uint32_t      t_free_cnt;   /* 累计释放次数 */
    wmm_cache_bin_t bins[WMM_NBINS + 1];  /* [0..35] = 各小对象类的缓存;
                                             [36] 预留。
                                             大对象刻意不做线程缓存:所有
                                             大对象混用同一个槽,弹栈无法
                                             保证规格匹配请求(要 1MB 可能
                                             弹出 16KB 的块),而且固定槽位
                                             数 × 任意大的对象能囤出 GB 级
                                             缓存。jemalloc 的"大对象 20 槽"
                                             是"每个大对象类各一个缓存 bin"
                                             语境下的每类槽位,不能剥离语境
                                             移植。大对象分配/释放直走物理层;
                                             将来若要缓存,须在释放对象首
                                             8 字节存页数、弹栈时校验,[36]
                                             即为此预留(届时不必改本结构
                                             布局) */
};

extern _Thread_local wmm_tcache_t *g_tcache;  /* 惰性创建;NULL = 本线程尚未建 */
extern wmm_tcache_t *g_threads;               /* 线程注册表链头 */
extern pthread_mutex_t g_threads_lock;

/* ==================== 全局状态(初始化时定死) ==================== */

/* VA 堆区窗口:boot 时 mmap(PROT_NONE|MAP_NORESERVE) 预留整段——只占
 * 地址空间、不占物理内存;真正的分配在窗口内 MAP_FIXED 改权限。
 * 为什么预留固定窗口:窗口定死,页表才能是一次 mmap 到位的平坦数组,
 * 免去"堆长大要扩页表"的全部动态管理代码。
 * 代价模型(窗口选多大只看这两行):
 *   VA 预留 = 窗口大小/512(64GB 窗口 → 页表数组 128MB;x86-64 的
 *   地址空间有 128TB,VA 免费付得起);
 *   物理占用 ≈ 实际用掉的堆/512(约 0.2%),随分配惰性增长,与窗口
 *   大小无关——前提是页表绝不 memset(见 g_pte)。
 * strict overcommit(vm.overcommit_memory=2)会把预留计入 commit 配额,
 * 预留可能失败:boot 减半重试,实际窗口以 g_heap_base/g_heap_end 为准。
 * 窗口耗尽按 OOM 处理并写事件(附当前用量):撞的是人为上限、不是
 * 机器没内存,运维要能一眼区分。要更大的窗口 = 改常量重编;不做
 * 动态扩——动态扩等于把平坦页表换回二级页表的世界。 */
extern void *g_heap_base;    /* 窗口基址(boot 定死) */
extern void *g_heap_end;     /* 窗口末尾 = base + 实际窗口大小(减半重试
                               后可能小于 WMM_HEAP_VA_SIZE;越界判据) */
extern void *g_heap_cur;     /* bump 指针:内核新映射永远从这里向前推进,
                               保证映射必落在页表覆盖范围内 */

extern pthread_mutex_t g_ext_lock;  /* 物理层的粗粒度锁:extent 地址链、
                                       空闲链、大对象统计共用一把。物理层
                                       只在慢路径与后台线程出现,冲突窗口
                                       小,分锁徒增复杂 */

typedef struct {
    uint64_t live;           /* 大对象存活个数 */
    uint64_t live_bytes;     /* 大对象存活字节(页对齐) */
    uint64_t ndalloc;        /* 累计释放次数 */
} wmm_large_stats_t;

extern wmm_large_stats_t g_large;   /* 大对象(整页分配)的统计;与 bin 的
                                       live 计数同一纪律:在 g_ext_lock
                                       内更新,保证与 live_bytes 恒等式
                                       一起成立 */

/* 运行期选项:初始化前为默认值,wmm_ctl("config.*") 可改 */
typedef struct {
    size_t   dirty_max;      /* 空闲"脏"内存回收阈值:攒到超过它才向内核
                               归还。为什么用简单阈值而不是 jemalloc 的
                               decay 平滑算法:行为可预期(后台每秒查一
                               次)、代码少两个数量级 */
    int      madvise_mode;   /* 内核回收方式,运行期可切:
                               MADV_DONTNEED 立即还物理页,RSS 收得干脆,
                               代价是再访问要缺页;
                               MADV_FREE 懒还,内核内存紧张才收,延迟更稳。
                               RSS 敏感选前者,延迟敏感选后者 */
    unsigned sample_n;       /* 调用点采样间隔:每 N 次慢路径分配采一次栈;
                               0 = 关(默认关,采样有开销) */
    bool     guard;          /* 越界/UAF/双释放检测总开关(默认关)。
                               检测要 poison/redzone/隔离,内存放大且
                               慢路径变慢——只排障时开 */
    bool     verbose;        /* wmm_dprintf 调试输出开关 */
    bool     abort_oom;      /* OOM 行为:false 返回 NULL(默认,跟 glibc
                               一致);true 直接 abort——快速失败比带病
                               延续好排查 */
} wmm_opt_t;

extern wmm_opt_t g_opt;

/* ==================== 元数据 bump 区(对应 jemalloc base.c) ====================
 *
 * slab 头、extent 结点这类长命元数据从哪来?不能从分配器自己来
 * (自举死锁),逐个 mmap 又浪费内核内存。解法:初始化时 mmap 一块
 * 专用区,8 字节对齐地线性切割,只分配不回收——元数据与堆同寿,
 * 根本不存在"回收元数据"的问题。随对象生灭反复申请的东西
 * (extent 结点)不许来这,走显式空闲链复用(见 wmm_phys.c)。
 */
void *wmm_base_alloc(size_t size);

/* ==================== 小工具(全库通用) ==================== */

/* write(2) 直出的受限 printf(仅 %s/%p/%zu/%u/%x/%d 等基础转换)。
 * 为什么不用标准 printf:它会调 malloc,在 OOM 或初始化未完成的
 * 路径上等于递归自爆;直写 stderr 也在最坏情况下可用。 */
void wmm_dprintf(const char *fmt, ...);

/* 对象编号 = (p − slab基址) / reg_size。reg_size 是运行期常数但
 * 编译器不知道,先用普通除法(约 20 周期,只在慢路径);jemalloc
 * 用预计算魔术数把除法换成乘法,成为瓶颈时照抄即可。 */
uint32_t wmm_div_regind(const void *p, const void *base, uint32_t reg_size);

#endif /* WMM_INTERNAL_H_INCLUDED */
