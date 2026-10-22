/* wmm_stats.c —— 统计快照、事件环与单调时钟 */

#define _GNU_SOURCE

#include "wmm_internal.h"

#include <string.h>
#include <time.h>
#include <unistd.h>

static wmm_event_t   g_event_ring[WMM_EVENT_RING];
static unsigned      g_event_head;
static unsigned      g_event_tail;
static uint64_t      g_event_total;
pthread_mutex_t      g_event_lock = PTHREAD_MUTEX_INITIALIZER;

uint64_t wmm_mono_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* 填公共字段,并尽量由页表补出尺寸类;窗口外的指针不查表 */
static void event_fill(wmm_event_t *ev, uint32_t type, void *ptr) {
    memset(ev, 0, sizeof(*ev));
    ev->ts   = wmm_mono_ns();
    ev->type = type;
    ev->tid  = g_tcache != NULL ? g_tcache->tid : (uint32_t)gettid();
    ev->ptr  = (uintptr_t)ptr;

    if (ptr != NULL && (uintptr_t)ptr >= (uintptr_t)g_heap_base &&
        (uintptr_t)ptr < (uintptr_t)g_heap_end) {
        wmm_slab_t *sl;
        void       *ext;
        if (wmm_pte_is_slab(ptr, &sl))
            ev->cls = sl->szind;
        else if (wmm_pte_is_extent(ptr, &ext))
            ev->cls = WMM_NBINS;
    }
}

/* 互斥写环;写满时覆盖最旧一条,总数只增 */
static void event_push(const wmm_event_t *ev) {
    pthread_mutex_lock(&g_event_lock);
    g_event_ring[g_event_tail] = *ev;
    g_event_tail = (g_event_tail + 1) % WMM_EVENT_RING;
    g_event_total++;
    if (g_event_tail == g_event_head)
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

    ev.pc0 = (uintptr_t)__builtin_return_address(0);

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wframe-address"
#endif
    ev.pc1 = (uintptr_t)__builtin_return_address(1);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

    event_push(&ev);
}

/* 整环镜像给发布端;事件总数一并取出 */
void wmm_event_copy(wmm_event_t *dst, uint64_t *total) {
    pthread_mutex_lock(&g_event_lock);
    memcpy(dst, g_event_ring, sizeof(g_event_ring));
    *total = g_event_total;
    pthread_mutex_unlock(&g_event_lock);
}

/* 消费式读取:最多取 n 条并推进 head */
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

static _Atomic uint64_t g_snapshot_epoch;
static _Atomic uint64_t g_high_water_live;

/* 逐个 bin 加锁汇总,再并入 guard、大对象与线程计数。
   跨 bin 不是原子快照,只保证每个 bin 内部一致;high_water 用 CAS 抬高。 */
int wmm_stats_snapshot(wmm_stats_t *st) {
    if (st == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(st, 0, sizeof(*st));

    uint64_t live_bytes = 0;
    uint64_t live_objs  = 0;

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

    pthread_mutex_lock(&g_guard_lock);
    for (szind_t i = 0; i < WMM_NBINS; i++) {
        wmm_bin_t *b = &g_guard_bins[i];
        st->cls[i].live    += b->live;
        st->cls[i].ndalloc += b->ndalloc;
        live_bytes += b->live * (uint64_t)wmm_bin_info[i].reg_size;
        live_objs  += b->live;
    }
    pthread_mutex_unlock(&g_guard_lock);

    pthread_mutex_lock(&g_ext_lock);
    uint64_t large_live  = g_large.live;
    uint64_t large_bytes = g_large.live_bytes;
    st->cls[WMM_NBINS].live    = large_live;
    st->cls[WMM_NBINS].ndalloc = atomic_load_explicit(&g_large.ndalloc,
                                                      memory_order_relaxed);
    st->cls[WMM_NBINS].size    = 0;
    st->active_bytes           = g_active_bytes;
    pthread_mutex_unlock(&g_ext_lock);
    live_bytes += large_bytes;
    live_objs  += large_live;

    st->live_bytes   = live_bytes;
    st->live_objects = live_objs;
    st->frag_bytes   = st->active_bytes >= live_bytes
                         ? st->active_bytes - live_bytes : 0;

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

    pthread_mutex_lock(&g_threads_lock);
    uint64_t alloc_total = 0;
    uint64_t free_total  = 0;
    unsigned tslot       = 0;
    for (wmm_tcache_t *t = g_threads; t != NULL; t = t->next) {
        unsigned dst = tslot < 128 ? tslot : 127;
        if (tslot < 128)
            st->threads[dst].tid = t->tid;
        else
            st->threads[dst].tid = 0;
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

    st->epoch = atomic_fetch_add_explicit(&g_snapshot_epoch, 1,
                                          memory_order_relaxed) + 1;
    return 0;
}
