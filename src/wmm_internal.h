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

/* size → 尺寸类编号(主路径第一行)。三段:
 *   ≤4096  一步查表(绝大多数请求走这里);
 *   ≤14336 只有 8 个类,从类 29 起线性扫:最坏 8 次比较,比二分的
 *          区间收缩开销更低;更重要的是以表为唯一权威——若用公式
 *          推导常数,改生成参数时会静默错位,扫表永远跟着表走;
 *   再往上 = 大对象。查表上限以上、类 28(4096)必不能覆盖,故扫
 *   从 29 起步。 */
static inline szind_t wmm_size2index(size_t size) {
    if (size <= WMM_LOOKUP_MAX)
        return wmm_lookup_tab[(size + 7) >> 3];
    if (size <= WMM_SMALL_MAXCLASS)
        for (szind_t i = 29; i < WMM_NBINS; i++)
            if (size <= wmm_bin_info[i].reg_size)
                return i;
    return WMM_INDEX_LARGE;
}

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
 *   raw==0        → 该页从未分配过。free 到这种地址 = 双释放/野指针,
 *                   这就是检测的挂点,无需额外数据结构。
 *   raw 最低位=0  → slab 指针(小对象;slab 的每一页登记同一指针),
 *                   即 raw = (uintptr_t)slab 头
 *   raw 最低位=1  → extent 指针(大对象;同样全页登记),
 *                   即 raw = (uintptr_t)extent 结点 | 1
 * 为什么一项两用而不是两张表:free 路径一次读出即可分派,且省一半
 * 元数据。tag 为什么不用位域:位域的读出类型要走整数提升,63 位宽
 * 的字段在部分编译器上被窄化、触发指针转换告警;显式掩码
 * (raw & 1 判 tag、raw & ~1 取指针)无平台差异。最低位能腾出来,
 * 是因为 slab 头与 extent 结点都来自 base 区、至少 8 字节对齐。
 * 页表本体在初始化时一次 mmap——元数据绝不能从分配器自己拿,
 * 那时它还没建好。 */
typedef union wmm_pte_u {
    uintptr_t raw;            /* 整体读写;0 = 未分配;最低位 = tag */
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
    wmm_slab_t *ph_prev, *ph_next, *ph_child; /* 非满 slab 集合的挂链。当前
                                                 实现:ph_prev/ph_next 维护
                                                 地址升序双向链,bin 的
                                                 nonfull_root 指向最低地址,
                                                 分配永远从它取(地址聚集,
                                                 cache 局部性好);ph_child
                                                 恒 NULL,预留——若 O(n) 的
                                                 地址序插入真成为瓶颈,升级
                                                 为 jemalloc edata_heap 式
                                                 配对堆时启用它,不必改布局 */
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
    wmm_slab_t     *nonfull_root;   /* 非满 slab 地址升序链的头 = 最低地址
                                    (链体在 slab 的 ph_prev/ph_next;
                                    分配永远取它——同批对象地址聚集,
                                    cache 局部性好,低地址的 slab 也
                                    更容易先腾空整块归还) */
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

/* slab 与 bin 操作(wmm_core.c)。
 * 锁契约:以下函数自身不加锁,调用方必须已持有所属 bin 的锁——
 * 上锁成本由调用方(主路径、tcache 批量)统一摊销。 */
void  wmm_slab_init(wmm_slab_t *sl, void *addr, szind_t i);
/* 把 [addr, addr+slab_size) 初始化为类 i 的 slab:建全量空闲链、
 * 登记每一页的页表项。addr 页对齐由物理层保证,sl 头由调用方从
 * base 区取得。ext 字段留空,物理层交接 extent 时填。 */
void *wmm_slab_alloc(wmm_slab_t *sl);          /* 弹空闲链取对象;nfree==0
                                                 调用即违反内部契约,断言失败 */
void  wmm_slab_free(wmm_slab_t *sl, void *p);  /* 压链归还;nfree++ */
void  wmm_bin_slab_install(wmm_bin_t *b, wmm_slab_t *sl);
/* 新 slab 上任为 slabcur。前提:当前 slabcur==NULL——bin_alloc 返回
 * NULL 的路径已保证(旧热 slab 已挂 full);违反即断言失败 */
void *wmm_bin_alloc(wmm_bin_t *b);
/* bin 慢路径分配。b 的下标即类号,slab 自带 szind,无须再传类号。
 * NULL = 没有可用 slab:调用方去物理层取页,init + install 后
 * 重试,至多一次。 */
bool  wmm_bin_dalloc(wmm_bin_t *b, wmm_slab_t *sl, void *p);
/* 归还对象并驱动状态转移。返回真 = 该 slab 已全空并已从 bin 摘除,
 * 调用方须归还其物理页;curslabs 已减,bin 不再引用它 */
unsigned wmm_bin_fill(wmm_bin_t *b, szind_t i, unsigned want, void **out);
/* tcache 批量填充:一次持 bin 锁连弹 want 个写入 out,返回实收数
 * (小于 want = 窗口/元数据耗尽)。i 是 b 的下标:bin 结构不记录
 * 自身类号,没有新 slab 可取时要用它向物理层要页 */

/* ==================== extent 与物理层 ====================
 *
 * extent = 一段连续页的记账单位(对应 jemalloc 的 extent 元数据)。
 * 全部 extent 挂在一条全局地址序双向链上(含在用的)。这条链是
 * 对抗外部碎片的命脉:释放时链上 prev/next 就是物理邻居,合并
 * O(1);分配 first-fit 沿链从低地址找第一个够大的空闲块复用,
 * 窗口因此不会因"各处散落的小空闲块"而被迫无限增长。
 *
 * 状态三态:INUSE(服务中:承载 slab 或大对象)/ FREE_DIRTY(空闲,
 * 物理页仍在映射里,复用零系统调用)/ FREE_RETAINED(空闲,物理页
 * 已被 purge 的 madvise 交还内核,复用仍走同一条 first-fit,只是
 * 第一次访问要吃缺页,不需要新 mmap)。
 * 两种空闲状态合并时整块归 DIRTY:RETAINED 邻居的页数补回 dirty
 * 计数(见 wmm_phys.c extent_release)——状态标签与计数必须一致。
 *
 * 为什么不另挂一条独立的 free 链:first-fit 直接扫地址序链过滤
 * FREE 即可。jemalloc 用按大小分级的搜索树,那是多 arena、数十万
 * extent 规模下的选择;我们单 arena、extent 千级,全扫是慢路径里
 * 几十微秒的事,而省下的是一条链的全部插入/摘除代码和每结点 16
 * 字节——小而精取后者。
 *
 * 结点内存:壳(extent_t 本体)经回收链复用——大对象生灭频繁,
 * 逐块向 base 要新结点会把 base 区吃穿;复用的是结点这个壳,与
 * "base 区只进不出"不矛盾。slab 头不复用:量级小得多(见
 * wmm_phys.c 文件头)。
 */
enum {
    WMM_EXT_INUSE         = 1,   /* 服务中(承载 slab 或大对象) */
    WMM_EXT_FREE_DIRTY    = 2,   /* 空闲,物理页还在;复用零系统调用 */
    WMM_EXT_FREE_RETAINED = 3,   /* 空闲,物理页已还内核(purge 产物) */
};

typedef struct wmm_extent_s wmm_extent_t;
struct wmm_extent_s {
    void         *addr;      /* 区间基址,页对齐 */
    size_t        npages;    /* 区间页数 */
    uint32_t      state;     /* WMM_EXT_*;0 非法(新结点必须显式赋值) */
    wmm_extent_t *ar_prev;   /* 地址序双向链(全量,含 INUSE) */
    wmm_extent_t *ar_next;
    wmm_extent_t *next;      /* 结点回收链(g_ext_free_nodes,LIFO) */
    wmm_slab_t   *slab;      /* 承载小对象 slab 时指向其头;大对象 NULL */
};

/* 物理层全局(wmm_boot.c 定义):地址序链头、脏页计数、已投入服务页数。
 * 均在 g_ext_lock 内变更;导出给测试做白盒核对。
 * g_active_bytes = 窗口内已映射且物理页未被 purge 收回的字节
 * (即"扣除 FREE_RETAINED 之后的全部分配+空闲页"),live ≤ active
 * 恒成立,差值就是快照里的 frag_bytes。增长点:window_map 成功
 * mmap、purge 出的 RETAINED 块被重新切出(页重新投入服务,重新
 * 吃缺页);缩减点:purge 对 FREE_DIRTY 的 madvise 成功。 */
extern wmm_extent_t *g_ar_head;      /* 地址序链头(最低地址);NULL = 空 */
extern size_t        g_dirty_pages;  /* FREE_DIRTY 状态的总页数(purge 的依据) */
extern size_t        g_active_bytes; /* 已投入服务的窗口字节数(见上) */

/* 物理层接口(wmm_phys.c)。
 * 锁:g_ext_lock 保护地址序链、窗口 bump、结点回收链、g_dirty_pages
 * 与 g_large 统计;锁序 bin 锁 → g_ext_lock → base 锁(与 fork
 * prepare 一致;base 为叶子锁)。 */
wmm_slab_t   *wmm_phys_alloc_slab(szind_t i);
/* 取 pgs 页(优先 first-fit 复用,否则窗口 bump)+ slab 头(base 区),
 * 完成 slab 初始化、页表登记与 ext↔slab 双向接线。可在持有 bin 锁
 * 时调用。NULL = 窗口耗尽或元数据耗尽(OOM)。 */
void          wmm_phys_free_slab(wmm_slab_t *sl);
/* 整块 slab 退休:页入空闲(不清物理映射)、页表清零、向两侧合并。
 * 必须在 bin 锁释放后调用(bin 已不引用它;物理层只拿自己的锁)。 */
wmm_extent_t *wmm_phys_alloc_extent(size_t size);
/* 大对象分配:页数向上取整,优先复用,全页登记 tag=1。返回 extent
 * (调用方用 ext->addr 当对象地址);NULL = OOM。超窗口量级的请求
 * 在此挡下。 */
void          wmm_phys_free_extent(wmm_extent_t *ext);
/* 大对象释放:更新 g_large 计数、页入空闲、页表清零、向两侧合并、
 * 结点壳入回收链。 */

/* ==================== purge 与后台线程 ====================
 *
 * purge = 把 FREE_DIRTY 的物理页经 madvise 交还内核(方式由
 * g_opt.madvise_mode 定),状态转 FREE_RETAINED。页面仍留在映射里,
 * 复用它走普通 first-fit,不产生新 mmap。
 *   purge_now        无视阈值,立即扫全部 DIRTY(ctl "purge" 用);
 *   purge_if_needed  后台线程每 1s 调一次:脏页总字节超过
 *                    g_opt.dirty_max 才扫。
 * madvise 失败保持 DIRTY,下一轮再试。MADV_FREE 模式下内核延迟
 * 回收,状态语义是"内核可随时收回",不承诺内容已丢。
 *
 * 后台线程默认不启动(boot 不建线程),wmm_ctl("bg.enable"/
 * "bg.disable") 显式启停;stop 会 join,返回后线程确实已退出。
 * 启停必须由同一个控制线程发起(与 config 命令同纪律),不做并发
 * 启停的引用计数。创建失败返回 -1/errno=rc:没有后台线程只是不
 * 自动回收,分配器照常工作,不属于启动期契约设施。
 */
void wmm_phys_purge_if_needed(void);
void wmm_phys_purge_now(void);
int  wmm_bg_start(void);
int  wmm_bg_stop(void);
/* fork 子进程专用:后台线程不在子进程里存在,把控制锁/条件变量
 * 重建为空闲状态。由 wmm_atfork_child 在分配器锁重建后调用。 */
void wmm_bg_child_reset(void);

/* ==================== 统计与事件(wmm_stats.c) ====================
 *
 * 快照:合并各 bin 的 live/ndalloc(锁内读取,与 OBS1 恒等式同源)、
 * 大对象与 active 页数(ext 锁)、各线程的局部计数(遍历 g_threads,
 * 允许瞬时偏差)。线程表超过 128 行时第 129+ 个线程合并进
 * threads[127](聚合桶,tid 记 0)——直方图有限,聚合一个桶也比
 * 丢数据好。锁序与 fork prepare 一致:各 bin → ext → threads,
 * 不与任何既有持锁路径构成环。
 *
 * 事件环:定长环形缓冲,多生产者(任意线程的错误路径 + 后台线程
 * 的回收事件)写、wmm_event_drain 单消费者。事件低频(错误/回收
 * 路径),一把互斥锁就够:多生产者无锁环要 per-slot 就绪标记与
 * 读端幂等逻辑,复杂度换不来可测收益。环满覆盖最旧——排障场景下
 * "新事件比旧事件值钱"。写游标恒等于累计条数对容量取模,共享内存
 * 端靠这个关系从总量反推最新几条在环里的位置。
 */
#define WMM_EVENT_RING 1024u          /* 环容量;共享区镜像按整环拷贝 */

/* 写一条事件(类型/指针),自动带时间戳、线程号、涉事尺寸类
 * (尽力反查,未知为 0)与两帧调用点。低频路径专用。 */
void wmm_event_log(uint32_t type, void *ptr);
extern pthread_mutex_t g_event_lock;  /* 事件环互斥锁;fork 子进程重建 */

/* 整环快照拷贝到 dst(容量须为 WMM_EVENT_RING),累计写入条数写入
 * *total;持事件锁完成,两者必属同一时刻。与 wmm_event_drain 不同,
 * 本函数不消费(游标不动),供共享内存发布端镜像整环用。参数均须
 * 有效(内部接口)。 */
void wmm_event_copy(wmm_event_t *dst, uint64_t *total);

/* 单调时钟纳秒。事件时间戳与共享区发布时刻共用,保证监控端拿自己
 * 的同源时钟就能判断数据新鲜度(跨时钟源比较毫无意义)。 */
uint64_t wmm_mono_ns(void);

/* ==================== 共享内存监控区(wmm_shm.c) ====================
 *
 * 把一个内存映射文件当作对外的监控窗口:快照 + 事件环 + 采样表,
 * 后台线程每秒用 seqlock 发布一代;监控进程只读 mmap 同一文件,
 * 无 IPC、无锁、不改主逻辑。布局与读端在 wmm_shm.h。
 *
 * 单写者契约:运行时写者是后台线程,创建时由调用线程先同步发布
 * 一代。未创建时发布是空操作,因此发布调用点无需判断。
 */
/* 创建共享区(创建并映射文件、写头、发布第一代)。path==NULL
 * -1/EINVAL;本进程已有共享区 -1/EEXIST;open/ftruncate/mmap 失败
 * -1/errno 原样。成功返回 0。 */
int  wmm_shm_create(const char *path);
/* 发布一代(未创建则空操作)。只许单一写者调用。 */
void wmm_shm_publish(void);
/* fork 子进程专用:撤掉写映射并复位创建锁。子进程继承写权会导致
 * 父子交错发布,读者拼出从未存在过的快照。 */
void wmm_shm_child_reset(void);

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
    size_t        map_size;     /* 整块映射的字节数 = 本结构 + 各栈数组。
                                   线程退出按创建时的长度 munmap;重算
                                   会把"容量常量将来可变"变成一处静默
                                   地雷 */
    /* ---- 线程局部计数(唯一写者 = 本线程,非原子) ---- */
    uint64_t      t_alloc_bytes;   /* 累计分配字节(按可用大小计) */
    uint64_t      t_free_bytes; /* 累计释放字节 */
    uint32_t      t_alloc_cnt;  /* 累计分配次数 */
    uint32_t      t_free_cnt;   /* 累计释放次数 */

    /* ---- GC 状态(唯一写者 = 本线程,非原子) ---- */
    uint32_t      gc_evt;      /* 释放事件计数:每 WMM_TCACHE_GC_EVERY 次
                                  free 触发一轮轮转 GC。囤积只发生在释放
                                  侧(分配只会把 head 往下推),计数器放
                                  在 free 路径,快路径只多一条自增 */
    uint8_t       gc_next;     /* 轮转游标:下次事件 GC 要处理的 bin 号,
                                  0..35 循环——让"偶尔用一次"的类也有
                                  机会被压回 retain */
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
extern pthread_key_t   g_tcache_key;          /* 线程退出析构键,wmm_boot 注册一次;
                                                 静态哨兵 -1 = 尚未注册:boot 注册
                                                 失败即 abort,分配路径再见到 -1
                                                 按"没先 boot 或内存损坏"同样致命 */

/* ncached_max 公式 clamp(2*nregs, 20, 200) 的两端(实现见 wmm_tcache.c):
 * 下界保证小类一次填充的批量粒度足够摊销锁开销;上界把单线程缓存的
 * 无界膨胀钉死——线程多时,每线程的缓存都是实数。 */
#define WMM_TCACHE_NCACHED_MIN 20u
#define WMM_TCACHE_NCACHED_MAX 200u
#define WMM_TCACHE_GC_EVERY 256u   /* 释放事件轮转间隔:每这么多次 free
                                      轮转 GC 一个 bin。36 个 bin 全轮
                                      一遍 ≈ 9.2K 次 free,任何偶尔用
                                      的类都等得起这个观察窗口 */

/* 线程缓存操作(wmm_tcache.c)。同步模型:快路径只碰本线程的块,
 * 零锁零原子;填充/刷出持目标 bin 锁批量搬运;注册表增删持
 * g_threads_lock。缓存中的对象仍计入所属 bin 的 live(见 wmm_bin_t
 * 的计数纪律),所以任何时刻 Σ live = 应用持有 + 全部线程缓存。 */
void     wmm_tcache_fill(szind_t i, unsigned want);
/* 仅在 head==0 时调用:一次持 bin 锁从 arena 搬 want 个进栈,
 * 实收数封顶 ncached_max - head */
void     wmm_tcache_flush(szind_t i, unsigned n);
/* 从栈底(最冷端)刷 n 个回 bin;退休 slab 放锁后交物理层 */
void     wmm_tcache_flush_all(void);
/* 本线程全类刷空、缓存块保留:收敛核对与线程退出前的共用步骤 */
void     wmm_tcache_gc(szind_t i);
/* 对当前线程缓存的一个 bin 跑 GC:按 retain 公式(用量+25% 或种子 2)
 * 刷出富余,并按低水位信号自适应 nfill。满栈触发与事件轮转共用;
 * 白盒测试直调(TC3),将来的 wmm_ctl("gc") 也走它 */
void    *wmm_tcache_alloc(szind_t i);
void     wmm_tcache_free(szind_t i, void *p);
/* 快路径弹栈/压栈。alloc 返回 NULL = 缓存空,调用方去填充;
 * free 在缓存满时先部分刷出再压,保住热对象的 LIFO 局部性 */
uint64_t wmm_tcache_cached_total(void);
/* Σ 全部线程缓存的对象数。heads 由各线程无锁写,本函数只在全体
 * 静止时刻(屏障汇合、单线程)给出精确值;锁保护的是注册表链 */
void     wmm_tcache_child_cleanup(void);
/* fork 子进程侧:死线程节点刷空缓存后 munmap,注册表只留调用线程。
 * 由 wmm_atfork_child 在所有锁重建后调用 */
void     wmm_tcache_thread_destructor(void *val);
/* pthread 键析构;显式 exit 之后再触发是空操作(幂等) */

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
