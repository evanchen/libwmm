/* wmm_stats.c —— 统计与事件日志
 *
 * 职责:线程局部计数(快路径累加,不加锁);bin 内的每类存活计数
 * (持锁更新,是"存活字节数恒等式"的唯一事实来源);快照合并
 * (遍历线程注册表汇总成 wmm_stats_t);事件环的写入与读出。
 * 统计不是附属功能:监控数据自身可信是本库的核心承诺。
 *
 * 三组数据的可信来源各不相同,口径必须分清:
 *   live/ndalloc     在各自锁内更新(bin 锁 / ext 锁),快照照同一
 *                    把锁读取——"Σ live*size == live_bytes"恒成立
 *                    不是估计值,是同一批读数的和;
 *   allocated/freed  各线程在分配/释放出口累进自己的 tcache 块,
 *                    快照遍历 g_threads 合并。所有者是唯一写者,
 *                    写与读都走 relaxed 原子访问:合并瞬间仍可能读到
 *                    上一瞬的值,但 C11 层面不存在数据竞争,快路径
 *                    也只有普通 load/store(单写者不需要读改写);
 *   active/high_water 前者在 g_ext_lock 内记账(purge 增减),后者
 *                    快照时取历史最大值;快照可能被应用线程与后台
 *                    发布线程并发调用,两个跨调用的状态必须是原子。
 *
 * 事件环为什么用互斥锁而不是无锁环:多生产者是事实——任何线程
 * 的错误路径(双释放、OOM)和后台线程的回收动作都会写。事件是
 * 低频路径,一把锁的代价可以忽略,而多生产者无锁环需要 per-slot
 * 就绪标记和读端幂等逻辑,复杂度换不来可测的收益。环满覆盖最旧:
 * 排障时新事件比旧事件值钱。
 */

#define _GNU_SOURCE        /* gettid(2),与 wmm_tcache.c 同款 */

#include "wmm_internal.h"

#include <string.h>
#include <time.h>
#include <unistd.h>

/* ==================== 事件环 ==================== */

static wmm_event_t   g_event_ring[WMM_EVENT_RING];
static unsigned      g_event_head;   /* 读游标:下一条待消费;head==tail = 空 */
static unsigned      g_event_tail;   /* 写游标:下一个写入位 */
static uint64_t      g_event_total;  /* 累计写入条数(只增);与写游标同锁更新 */
pthread_mutex_t      g_event_lock = PTHREAD_MUTEX_INITIALIZER;

uint64_t wmm_mono_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* 事件公共字段:时间戳、类型、触发线程、涉事指针与尺寸类反查。
 * 调用点两帧由各入口自己填——guard 的检查点没有"当前用户调用帧"
 * 可抓,它报的是对象的分配点或释放点。 */
static void event_fill(wmm_event_t *ev, uint32_t type, void *ptr) {
    memset(ev, 0, sizeof(*ev));
    ev->ts   = wmm_mono_ns();
    ev->type = type;
    ev->tid  = g_tcache != NULL ? g_tcache->tid : (uint32_t)gettid();
    ev->ptr  = (uintptr_t)ptr;

    /* 涉事尺寸类尽力反查:错误路径上的指针大多有效,能填就不让
       监控端猜。反查只读页表(单写者在锁内,这里非原子的读最多
       读到旧归属,不会损坏任何结构),不需要额外持锁。 */
    if (ptr != NULL && (uintptr_t)ptr >= (uintptr_t)g_heap_base &&
        (uintptr_t)ptr < (uintptr_t)g_heap_end) {
        wmm_slab_t *sl;
        void       *ext;
        if (wmm_pte_is_slab(ptr, &sl))
            ev->cls = sl->szind;
        else if (wmm_pte_is_extent(ptr, &ext))
            ev->cls = WMM_NBINS;   /* 大对象聚合类号 */
    }
}

static void event_push(const wmm_event_t *ev) {
    pthread_mutex_lock(&g_event_lock);
    g_event_ring[g_event_tail] = *ev;
    g_event_tail = (g_event_tail + 1) % WMM_EVENT_RING;
    g_event_total++;
    if (g_event_tail == g_event_head)   /* 环满:覆盖最旧的一条 */
        g_event_head = (g_event_head + 1) % WMM_EVENT_RING;
    pthread_mutex_unlock(&g_event_lock);
}

void wmm_event_log_site(uint32_t type, void *ptr, uintptr_t pc0,
                        uintptr_t pc1) {
    wmm_event_t ev;
    event_fill(&ev, type, ptr);
    ev.pc0 = pc0;
    ev.pc1 = pc1;
    event_push(&ev);
}

void wmm_event_log(uint32_t type, void *ptr) {
    wmm_event_t ev;
    event_fill(&ev, type, ptr);

    ev.pc0 = (uintptr_t)__builtin_return_address(0);   /* 事件点的调用者 */
    /* 再上一帧。GCC 对非零参数无条件发 -Wframe-address:编译器不能
       假设帧链一定可走,读上一帧可能踩空。本库构建的两套变体都强制
       -fno-omit-frame-pointer,事件又只从库内与同标志编译的测试里
       写,这一帧必然存在——所以在此把警告收窄成"已知安全的特例",
       而不是改用手工帧遍历:后者要把 x86-64 的帧布局硬编码进来,
       反而比编译器内建更脆。 */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wframe-address"
#endif
    ev.pc1 = (uintptr_t)__builtin_return_address(1);   /* 再上一帧 */
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

    event_push(&ev);
}

void wmm_event_copy(wmm_event_t *dst, uint64_t *total) {
    pthread_mutex_lock(&g_event_lock);
    memcpy(dst, g_event_ring, sizeof(g_event_ring));
    *total = g_event_total;
    pthread_mutex_unlock(&g_event_lock);
}

int wmm_event_drain(wmm_event_t *buf, size_t n, size_t *got) {
    if (buf == NULL || n == 0 || got == NULL) {
        errno = EINVAL;
        return -1;
    }
    *got = 0;

    pthread_mutex_lock(&g_event_lock);
    while (*got < n && g_event_head != g_event_tail) {
        buf[*got] = g_event_ring[g_event_head];
        g_event_head = (g_event_head + 1) % WMM_EVENT_RING;
        (*got)++;
    }
    pthread_mutex_unlock(&g_event_lock);
    return 0;
}

/* ==================== 快照 ==================== */

/* 快照不是后台线程的私有路径:应用随时可以调 wmm_stats_snapshot,
 * 后台发布每秒也调一次,两者会并发。下面两个跨调用状态因此必须是
 * 原子量——epoch 的普通 ++ 会让两个并发快照拿到同一个序号,高水位
 * 的"读-比较-写"则可能被较旧的一次快照覆盖而回退。两者都只在快照
 * 冷路径访问,原子操作的代价无关紧要。 */
static _Atomic uint64_t g_snapshot_epoch;   /* 快照序号;每次成功快照 +1 */
static _Atomic uint64_t g_high_water_live;  /* live_bytes 历史峰值;快照时更新 */

int wmm_stats_snapshot(wmm_stats_t *st) {
    if (st == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(st, 0, sizeof(*st));

    uint64_t live_bytes = 0;
    uint64_t live_objs  = 0;

    /* 1) 各 bin 的 live/ndalloc:逐类持锁读取。live_bytes 与 cls 表
       用同一批读数求和,恒等式成立是定义使然——真正要断言的是
       "读数自身与内部状态一致",见 tests/test_stats.c 的独立核算。 */
    for (szind_t i = 0; i < WMM_NBINS; i++) {
        wmm_bin_t *b = &g_bins[i];
        pthread_mutex_lock(&b->lock);
        uint64_t live = b->live;
        st->cls[i].live    = live;
        st->cls[i].ndalloc = b->ndalloc;
        st->cls[i].size    = wmm_bin_info[i].reg_size;
        pthread_mutex_unlock(&b->lock);
        live_bytes += live * (uint64_t)wmm_bin_info[i].reg_size;
        live_objs  += live;
    }

    /* 1b) guard 池的存活计数按同一口径并入 cls[i]:guard 对象和
       常规对象一样是"应用手里的内存",只是布局不同。guard bin 的
       计数在 g_guard_lock 内更新,这里持同一把锁读取,恒等式依然
       由同一批读数求和成立。锁序 bins → guard → ext,与 fork 一致。 */
    pthread_mutex_lock(&g_guard_lock);
    for (szind_t i = 0; i < WMM_NBINS; i++) {
        wmm_bin_t *b = &g_guard_bins[i];
        st->cls[i].live    += b->live;
        st->cls[i].ndalloc += b->ndalloc;
        live_bytes += b->live * (uint64_t)wmm_bin_info[i].reg_size;
        live_objs  += b->live;
    }
    pthread_mutex_unlock(&g_guard_lock);

    /* 2) 大对象与 active 页数:同一把 ext 锁内读取,与"live 恒等式"
       共享同一个事实来源。锁内只读一次到局部变量,cls[] 与两个汇总
       共用同一份读数:若解锁后再读一次 g_large,cls[36] 和
       live_objects 就取自两个瞬间,并发分配/释放大对象时恒等式
       "Σ cls[i].live == live_objects" 会破——快照的每个字段可以各取
       各的瞬间,但被同一个恒等式绑在一起的字段必须同源。 */
    pthread_mutex_lock(&g_ext_lock);
    uint64_t large_live  = g_large.live;
    uint64_t large_bytes = g_large.live_bytes;
    st->cls[WMM_NBINS].live    = large_live;
    st->cls[WMM_NBINS].ndalloc = g_large.ndalloc;
    st->cls[WMM_NBINS].size    = 0;                 /* 大对象大小不一 */
    st->active_bytes           = g_active_bytes;
    pthread_mutex_unlock(&g_ext_lock);
    live_bytes += large_bytes;
    live_objs  += large_live;

    st->live_bytes   = live_bytes;
    st->live_objects = live_objs;
    st->frag_bytes   = st->active_bytes >= live_bytes
                         ? st->active_bytes - live_bytes : 0;

    /* 3) 高水位:快照时取历史最大(监控读到的就是"至今为止")。
       CAS 循环保证并发快照下只升不降:较旧的快照即使来得晚,也只会
       在 live_bytes 更大时成功写入。 */
    uint64_t hw = atomic_load_explicit(&g_high_water_live,
                                       memory_order_relaxed);
    while (live_bytes > hw &&
           !atomic_compare_exchange_weak_explicit(&g_high_water_live, &hw,
                                                  live_bytes,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed))
        ;
    st->high_water_live = atomic_load_explicit(&g_high_water_live,
                                               memory_order_relaxed);

    /* 4) 各线程计数:遍历注册表。所有者是唯一写者,读端用 relaxed
       原子访问——可能读到上一瞬的值,但不会撕裂,也不构成 C11 数据
       竞争(见 wmm_internal.h 线程缓存段)。
       超过 128 行合并进聚合桶 threads[127](tid 记 0)。 */
    pthread_mutex_lock(&g_threads_lock);
    uint64_t alloc_total = 0;
    uint64_t free_total  = 0;
    unsigned tslot       = 0;
    for (wmm_tcache_t *t = g_threads; t != NULL; t = t->next) {
        unsigned dst = tslot < 128 ? tslot : 127;
        if (tslot < 128)
            st->threads[dst].tid = t->tid;
        else
            st->threads[dst].tid = 0;   /* 聚合桶:不再区分线程 */
        uint64_t ab = atomic_load_explicit(&t->t_alloc_bytes,
                                           memory_order_relaxed);
        uint64_t fb = atomic_load_explicit(&t->t_free_bytes,
                                           memory_order_relaxed);
        uint64_t ac = atomic_load_explicit(&t->t_alloc_cnt,
                                           memory_order_relaxed);
        st->threads[dst].alloc_bytes += ab;
        st->threads[dst].free_bytes  += fb;
        st->threads[dst].alloc_cnt   += ac;
        alloc_total += ab;
        free_total  += fb;
        tslot++;
    }
    pthread_mutex_unlock(&g_threads_lock);

    st->allocated_bytes   = alloc_total;
    st->deallocated_bytes = free_total;

    /* 并发快照各自取得一个全局唯一、递增的序号;两个调用的完成顺序
       可能与序号顺序不同,监控端只用它判断"数据是否换了一代" */
    st->epoch = atomic_fetch_add_explicit(&g_snapshot_epoch, 1,
                                          memory_order_relaxed) + 1;
    return 0;
}
