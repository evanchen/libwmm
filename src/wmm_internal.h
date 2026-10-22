/* wmm_internal.h —— 模块间共享的常量、类型、全局状态与内部接口 */

#ifndef WMM_INTERNAL_H_INCLUDED
#define WMM_INTERNAL_H_INCLUDED

#include "wmm.h"

#include <pthread.h>
#include <errno.h>
#include <stdatomic.h>

#define WMM_LG_PAGE 12                          /* 4KB 页 */
#define WMM_PAGE    ((size_t)1 << WMM_LG_PAGE)
#define WMM_NBINS   36                          /* 小对象尺寸类:8B..14336B */
#define WMM_ALIGN   16                          /* x86-64 max_align_t */

#define WMM_SMALL_MAXCLASS  ((size_t)14336)     /* 更大走整页 extent */

#define WMM_LARGE_MINCLASS  ((size_t)16384)     /* 大对象下限 = 4 页 */
#define WMM_LOOKUP_MAX      4096                /* 该范围内查表定类 */

#ifndef WMM_HEAP_VA_SIZE
#define WMM_HEAP_VA_SIZE ((size_t)64 << 30)     /* 堆窗口,可构建时覆盖 */
#endif

typedef uint32_t szind_t;                       /* 尺寸类下标 */
#define WMM_INDEX_LARGE WMM_NBINS

/* 尺寸类几何:对象字节数、每 slab 页数、slab 字节数、每 slab 对象数 */
typedef struct {
    uint32_t reg_size;
    uint32_t pgs;

    uint32_t slab_size;
    uint32_t nregs;
} wmm_bin_info_t;

#include "wmm_sc.h"

/* size -> 尺寸类;超过 WMM_SMALL_MAXCLASS 返回 WMM_INDEX_LARGE */
static inline szind_t wmm_size2index(size_t size) {
    if (size <= WMM_LOOKUP_MAX)
        return wmm_lookup_tab[(size + 7) >> 3];
    if (size <= WMM_SMALL_MAXCLASS)
        for (szind_t i = 29; i < WMM_NBINS; i++)
            if (size <= wmm_bin_info[i].reg_size)
                return i;
    return WMM_INDEX_LARGE;
}

typedef struct wmm_slab_s wmm_slab_t;

/* 页表项:0=无归属;bit0=1 指向 extent,bit0=0 指向 slab */
typedef union wmm_pte_u {
    uintptr_t raw;
} wmm_pte_t;

extern wmm_pte_t *g_pte;                        /* 平坦页表,每页一项 */

wmm_pte_t wmm_pte_get(const void *p);
void      wmm_pte_set(const void *p, wmm_pte_t v);
bool      wmm_pte_is_slab(const void *p, wmm_slab_t **out);
bool      wmm_pte_is_extent(const void *p, void **out_ext);

/* slab 头:对象空闲链与归属 bin 的链指针 */
struct wmm_slab_s {
    uint32_t    szind;
    uint32_t    nfree;
    uint32_t    free_head;                      /* 空闲链头,存 regind */
    uint32_t    nregs;
    uint32_t    reg_size;
    uint32_t    data_off;                       /* 首对象偏移;guard 给哨兵让位 */
    bool        guarded;                        /* 整块只放一个对象 + redzone */
    uint8_t     pad[7];
    void       *addr;

    wmm_slab_t *full_prev, *full_next;          /* full/隔离/回收池/头复用共用链 */
    wmm_slab_t *ph_prev, *ph_next, *ph_child;   /* nonfull 地址序链;ph_child 预留 */
    void       *ext;                            /* 物理层 extent;NULL=自备页 */
    uintptr_t   site_pc;                        /* guard 分配点,泄漏归因 */
};

/* 一个尺寸类的全局 bin;全部字段由 lock 保护 */
typedef struct {
    pthread_mutex_t lock;

    wmm_slab_t     *slabcur;
    wmm_slab_t     *full_head;
    wmm_slab_t     *nonfull_root;               /* 非满 slab 地址序链 */

    uint64_t live;                              /* 已分配对象数 */
    uint64_t ndalloc;
    uint64_t nslabs;
    uint64_t curslabs;
} wmm_bin_t;

extern wmm_bin_t g_bins[WMM_NBINS];

void  wmm_slab_init(wmm_slab_t *sl, void *addr, szind_t i);

void  wmm_slab_init_guard(wmm_slab_t *sl, void *addr, szind_t i,
                          size_t npages);

void  wmm_slab_guard_rearm(wmm_slab_t *sl);

void *wmm_slab_alloc(wmm_slab_t *sl);

void  wmm_slab_free(wmm_slab_t *sl, void *p);

void  wmm_bin_slab_install(wmm_bin_t *b, wmm_slab_t *sl);

void *wmm_bin_alloc(wmm_bin_t *b);

bool  wmm_bin_dalloc(wmm_bin_t *b, wmm_slab_t *sl, void *p);

unsigned wmm_bin_fill(wmm_bin_t *b, szind_t i, unsigned want, void **out);

enum {
    WMM_EXT_INUSE         = 1,                  /* 已分配 */
    WMM_EXT_FREE_DIRTY    = 2,                  /* 空闲,页内容仍可复用 */
    WMM_EXT_FREE_RETAINED = 3,                  /* 已 madvise,复用要缺页 */
};

/* extent:窗口内一段连续页;ar 链按地址排序,sz 链挂尺寸桶 */
typedef struct wmm_extent_s wmm_extent_t;
struct wmm_extent_s {
    void         *addr;
    size_t        npages;
    uint32_t      state;                        /* WMM_EXT_* */
    wmm_extent_t *ar_prev;
    wmm_extent_t *ar_next;
    wmm_extent_t *sz_prev;                      /* 尺寸桶前后链,仅空闲时有效 */
    wmm_extent_t *sz_next;
    wmm_extent_t *next;                         /* 空闲结点回收链 */
    wmm_extent_t *cache_next;                   /* 线程缓存链,仅缓存时有效 */
    wmm_slab_t   *slab;                         /* 承载的 slab,可为 NULL */
};

extern wmm_extent_t *g_ar_head;                 /* 地址序 extent 链头 */
extern size_t        g_dirty_pages;             /* FREE_DIRTY 页数 */
extern size_t        g_active_bytes;            /* 未 purge 的映射字节数 */

/* 新建 slab:头来自 base 区,页来自物理层 */
wmm_slab_t   *wmm_phys_alloc_slab(szind_t i);

/* 给已有头的 slab 接页;失败时只返回 NULL,头不动 */
wmm_slab_t   *wmm_phys_alloc_slab_pages(wmm_slab_t *sl, szind_t i,
                                        size_t npages, bool guarded);

void          wmm_phys_free_slab(wmm_slab_t *sl);
void          wmm_phys_retire_slab(wmm_slab_t *sl);

wmm_extent_t *wmm_phys_alloc_extent(size_t size);

void          wmm_phys_free_extent(wmm_extent_t *ext);

void          wmm_phys_clear_extent_ptes(wmm_extent_t *ext);
void          wmm_phys_stamp_extent_ptes(wmm_extent_t *ext);

/* 缓存块页表已清:锁内扣 live 并归还,不计 ndalloc */
void          wmm_phys_drain_cached_extents(wmm_extent_t *const *exts,
                                            uint32_t n);

size_t        wmm_phys_audit(void);             /* 白盒:尺寸桶一致性 */

void wmm_phys_purge_if_needed(void);            /* 脏页超阈值才回收 */
void wmm_phys_purge_now(void);                  /* 无条件回收 */
int  wmm_bg_start(void);                        /* 后台回收/发布线程 */
int  wmm_bg_stop(void);                         /* 停止并 join;幂等 */

void wmm_bg_child_reset(void);                  /* fork 子进程重建控制状态 */

#define WMM_EVENT_RING 1024u

void wmm_event_log(uint32_t type, void *ptr);

void wmm_event_log_site(uint32_t type, void *ptr, uintptr_t pc0,
                        uintptr_t pc1);
extern pthread_mutex_t g_event_lock;

void wmm_event_copy(wmm_event_t *dst, uint64_t *total);

uint64_t wmm_mono_ns(void);                     /* CLOCK_MONOTONIC 纳秒 */

int  wmm_shm_create(const char *path);

void wmm_shm_publish(void);                     /* 单写者契约 */

void wmm_shm_child_reset(void);

/* 每线程每尺寸类的对象栈 */
typedef struct {
    void    **stack;                            /* 栈底;对象紧跟在 tcache 后 */
    uint16_t ncached_max;
    uint16_t head;                              /* 栈顶(下一个空位) */
    uint16_t low_water;                         /* 上次 GC 以来的最低水位 */
    uint16_t nfill;                             /* 自适应批量填充目标 */
    uint64_t nrequests;
} wmm_cache_bin_t;

#define WMM_PROF_KEY_FRAMES 4u                  /* 调用点键保留帧数 */

#define WMM_PROF_DEPTH    WMM_PROF_KEY_FRAMES

#define WMM_PROF_RING     256                   /* 每线程样本环容量 */

#define WMM_PROF_SAMPLE_N 1000u                 /* 每 N 次分配采一个样本 */

#define WMM_PROF_TAB_N    1024u

#define WMM_PROF_TOP_MAX  256u

/* 采样环槽:seq 是逐槽 seqlock,index 是绝对写入序号 */
typedef struct {
    _Atomic uint32_t seq;

    uint32_t         depth;
    uint64_t         index;
    size_t           usize;
    uint32_t         weight;
    uint32_t         pad;
    void            *pc[WMM_PROF_DEPTH];
} wmm_prof_slot_t;

typedef struct {
    _Atomic uint64_t head;                      /* 只增的写序号 */
    uint64_t         merged;                    /* 已合并到的绝对序号 */
    wmm_prof_slot_t  slot[WMM_PROF_RING];
} wmm_prof_ring_t;

/* 大 extent 线程缓存:类 = 精确页数,每类独立 LIFO;默认单块上限
   8 页(32 KiB),与 jemalloc 的 tcache_max 默认值一致 */
#ifndef WMM_TCACHE_LARGE_MIN_PAGES
#define WMM_TCACHE_LARGE_MIN_PAGES 4u
#endif
#ifndef WMM_TCACHE_LARGE_NSLOTS
#define WMM_TCACHE_LARGE_NSLOTS 20u
#endif
#ifndef WMM_TCACHE_LARGE_MAX_PAGES
#define WMM_TCACHE_LARGE_MAX_PAGES 2048u
#endif
#ifndef WMM_TCACHE_LARGE_DEFAULT_PAGES
#define WMM_TCACHE_LARGE_DEFAULT_PAGES 8u
#endif
#ifndef WMM_TCACHE_LARGE_DEFAULT_BUDGET_PAGES
#define WMM_TCACHE_LARGE_DEFAULT_BUDGET_PAGES 2048u
#endif
#ifndef WMM_TCACHE_LARGE_MAX_BUDGET_PAGES
#define WMM_TCACHE_LARGE_MAX_BUDGET_PAGES 16384u
#endif
#ifndef WMM_TCACHE_LARGE_GC_EVERY
#define WMM_TCACHE_LARGE_GC_EVERY 256u
#endif
#ifndef WMM_TCACHE_LARGE_GC_PAGES
#define WMM_TCACHE_LARGE_GC_PAGES 128u
#endif

_Static_assert(WMM_TCACHE_LARGE_MIN_PAGES >= 1u &&
               WMM_TCACHE_LARGE_MIN_PAGES <= WMM_TCACHE_LARGE_MAX_PAGES,
               "大缓存页数下限非法");
_Static_assert(WMM_TCACHE_LARGE_DEFAULT_PAGES <= WMM_TCACHE_LARGE_MAX_PAGES,
               "默认大缓存上限超过硬上限");
_Static_assert(WMM_TCACHE_LARGE_NSLOTS >= 1u &&
               WMM_TCACHE_LARGE_NSLOTS <= UINT16_MAX,
               "大缓存每类槽数非法");
_Static_assert(WMM_TCACHE_LARGE_GC_EVERY >= 1u &&
               WMM_TCACHE_LARGE_GC_EVERY <= UINT16_MAX,
               "大缓存滚动 GC 周期非法");
_Static_assert(WMM_TCACHE_LARGE_GC_PAGES >= 1u,
               "大缓存滚动 GC 页预算非法");
_Static_assert(WMM_TCACHE_LARGE_MAX_PAGES <= UINT16_MAX,
               "大缓存页数类游标溢出 uint16");
_Static_assert(WMM_TCACHE_LARGE_DEFAULT_BUDGET_PAGES <=
               WMM_TCACHE_LARGE_MAX_BUDGET_PAGES,
               "默认大缓存预算超过硬上限");
_Static_assert((uint64_t)WMM_TCACHE_LARGE_NSLOTS *
               (WMM_TCACHE_LARGE_MAX_PAGES + WMM_TCACHE_LARGE_MIN_PAGES) *
               (WMM_TCACHE_LARGE_MAX_PAGES - WMM_TCACHE_LARGE_MIN_PAGES + 1u)
               / 2u <= UINT32_MAX,
               "大缓存页计数上界溢出 uint32");

typedef struct wmm_tcache_s wmm_tcache_t;

/* 线程缓存块:自身与各 bin 的对象栈在同一块 mmap 中 */
struct wmm_tcache_s {
    wmm_tcache_t *prev, *next;                  /* g_threads 链,锁保护 */
    uint32_t      tid;
    bool          enabled;                      /* 关闭则直走 bin,排障用 */
    size_t        map_size;                     /* 整块映射长度,退出时 munmap */

    _Atomic uint64_t t_alloc_bytes;             /* 线程累计计数:本线程写, */
    _Atomic uint64_t t_free_bytes;              /* 统计快照跨线程 relaxed 读 */
    _Atomic uint64_t t_alloc_cnt;
    _Atomic uint64_t t_free_cnt;

    _Atomic uint32_t gc_req;                    /* 跨线程 GC 请求位 */
    uint32_t      gc_evt;
    uint8_t       gc_next;                      /* 下一轮 GC 起始 bin */

    wmm_cache_bin_t bins[WMM_NBINS + 1];        /* [36] 保留,勿用 */

    /* 整 extent 大对象缓存:属主线程无锁改;按精确页数分类 */
    _Atomic uint32_t nlarge;                    /* 缓存块总数,统计跨线程读 */
    uint32_t         large_pages;               /* 已缓存页数,预算记账 */
    uint32_t         large_max_pages;           /* 本缓存数组上界,创建时定 */
    uint16_t         large_gc_evt;              /* 大对象 put 滚动 GC 计数 */
    uint16_t         large_gc_cursor;           /* 下一页数类,完整 GC 复位 */
    wmm_extent_t   **large_head;                /* [large_max_pages+1] */
    uint16_t        *large_count;               /* 每类槽数 */
    uint16_t        *large_used;                /* 上次 GC 以来取用次数 */

    uint32_t        prof_tick;
    wmm_prof_ring_t prof;
};

/* 唯一所有者写入;relaxed load/store,统计快照并发读 */
static inline void wmm_tcache_count_alloc(wmm_tcache_t *t, size_t bytes) {
    uint64_t v = atomic_load_explicit(&t->t_alloc_bytes,
                                      memory_order_relaxed);
    atomic_store_explicit(&t->t_alloc_bytes, v + bytes,
                          memory_order_relaxed);
    v = atomic_load_explicit(&t->t_alloc_cnt, memory_order_relaxed);
    atomic_store_explicit(&t->t_alloc_cnt, v + 1, memory_order_relaxed);
}

static inline void wmm_tcache_count_free(wmm_tcache_t *t, size_t bytes) {
    uint64_t v = atomic_load_explicit(&t->t_free_bytes,
                                      memory_order_relaxed);
    atomic_store_explicit(&t->t_free_bytes, v + bytes,
                          memory_order_relaxed);
    v = atomic_load_explicit(&t->t_free_cnt, memory_order_relaxed);
    atomic_store_explicit(&t->t_free_cnt, v + 1, memory_order_relaxed);
}

extern _Thread_local wmm_tcache_t *g_tcache;    /* NULL = 本线程未建 */
extern wmm_tcache_t *g_threads;                 /* 线程注册表 */
extern pthread_mutex_t g_threads_lock;
extern pthread_key_t   g_tcache_key;

#define WMM_TCACHE_NCACHED_MIN 20u
#define WMM_TCACHE_NCACHED_MAX 200u
#define WMM_TCACHE_GC_EVERY 256u

void     wmm_tcache_fill(szind_t i, unsigned want);

void     wmm_tcache_flush(szind_t i, unsigned n);

void     wmm_tcache_flush_all(void);

void     wmm_tcache_gc(szind_t i);

void     wmm_tcache_gc_all(void);

void    *wmm_tcache_alloc(szind_t i);
void     wmm_tcache_free(szind_t i, void *p);

uint32_t wmm_tcache_max_pages_now(void);
uint64_t wmm_tcache_budget_pages_now(void);

wmm_extent_t *wmm_tcache_large_take(wmm_tcache_t *t, size_t npages);
bool          wmm_tcache_large_put(wmm_tcache_t *t, wmm_extent_t *ext);

uint64_t wmm_tcache_cached_total(void);

void     wmm_tcache_child_cleanup(void);

void     wmm_tcache_thread_destructor(void *val);

void wmm_prof_sample(wmm_tcache_t *t, size_t usize, void *alloc_frame);

void wmm_prof_merge(void);

void wmm_prof_thread_drain(wmm_tcache_t *t);

unsigned wmm_prof_top(wmm_prof_entry_t *dst, unsigned k);

extern pthread_mutex_t g_prof_lock;

#define WMM_GUARD_RZ 16u                        /* 前后 redzone 字节数 */

#define WMM_GUARD_QUARANTINE 512u               /* 隔离队列上限 */

#define WMM_LEAK_WINDOWS 5u                     /* 连续上涨窗口数 */

#define WMM_LEAK_GROWTH (64u << 10)             /* 判定泄漏的最小涨幅 */

extern wmm_bin_t g_guard_bins[WMM_NBINS];       /* guard 专用 bin 池 */
extern pthread_mutex_t g_guard_lock;

/* guard 数据区偏移:按对象对齐取 16B..4KB */
static inline uint32_t wmm_guard_data_off(uint32_t reg_size) {
    uint32_t align = reg_size & (~reg_size + 1u);
    if (align > (uint32_t)WMM_PAGE)
        align = (uint32_t)WMM_PAGE;
    if (align < (uint32_t)WMM_ALIGN)
        align = (uint32_t)WMM_ALIGN;
    return align;
}

void *wmm_guard_alloc(szind_t i, uintptr_t site_pc);

bool  wmm_guard_free(wmm_slab_t *sl, void *p, uintptr_t site_pc);

void  wmm_guard_tick(void);

void  wmm_guard_disable(void);

unsigned wmm_guard_leak_dump(wmm_prof_entry_t *dst, unsigned k);

int wmm_leak_check(int k, wmm_prof_entry_t *out);

void wmm_leak_tick(void);

void wmm_leak_reset(void);

uint64_t wmm_live_bytes_now(void);

extern void *g_heap_base;
extern void *g_heap_end;

extern void *g_heap_cur;                        /* 窗口 bump 指针 */

extern pthread_mutex_t g_ext_lock;

/* 大对象统计:live/live_bytes 由 g_ext_lock 保护;ndalloc 原子,
   缓存释放路径在锁外累加 */
typedef struct {
    uint64_t        live;
    uint64_t        live_bytes;
    _Atomic uint64_t ndalloc;
} wmm_large_stats_t;

extern wmm_large_stats_t g_large;

typedef struct {
    size_t   dirty_max;                         /* 脏页阈值(字节) */
    _Atomic uint32_t tcache_max_pages;          /* 单块页数上限;0=关 */
    _Atomic uint64_t tcache_budget_pages;       /* 每线程页预算;0=不限 */
    _Atomic int madvise_mode;                   /* MADV_DONTNEED / MADV_FREE */
    _Atomic unsigned sample_n;                  /* 采样间隔;0=关 */
    _Atomic bool guard;
    _Atomic bool verbose;
    bool     abort_oom;                         /* OOM 时 abort 而非返回 NULL */
} wmm_opt_t;

extern wmm_opt_t g_opt;

/* 测试专用故障注入点 */
enum {
    WMM_FAULT_NONE    = 0,
    WMM_FAULT_MAP     = 1,                      /* 窗口 mmap */
    WMM_FAULT_MADVISE = 2,                      /* purge madvise */
    WMM_FAULT_TCACHE  = 3,                      /* 线程缓存 mmap */
    WMM_FAULT_BASE    = 4,                      /* base 元数据 */
    WMM_FAULT_SLAB_HDR = 5,                     /* slab 头分配 */
};
#define WMM_FAULT_ANY    0x7FFFFFFFu            /* 匹配任意 site */
#define WMM_FAULT_ALWAYS 0xFFFFFFFFu            /* 永不倒数,始终命中 */

extern _Atomic unsigned g_fault_site;
extern _Atomic unsigned g_fault_after;

bool wmm_fault_check(unsigned site);

void *wmm_base_alloc(size_t size);              /* 元数据 bump 分配 */
size_t wmm_base_bytes(void);                    /* 已映射元数据字节数 */

void wmm_dprintf(const char *fmt, ...);

uint32_t wmm_div_regind(const void *p, const void *base, uint32_t reg_size);

#endif
