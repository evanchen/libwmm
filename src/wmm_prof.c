/* wmm_prof.c —— 调用点采样、环形缓冲与聚合 TOP 表 */

#include "wmm_internal.h"

#include <stdatomic.h>
#include <string.h>

/* 帧指针链单步允许的最大跨度,挡住损坏的帧指针 */
#define WMM_PROF_MAX_FRAME_BYTES ((uintptr_t)16 << 20)

/* 沿帧指针链取返回地址;父帧必须递增、对齐且跨度不超过上限 */
static unsigned wmm_backtrace_from(void *frame, void **buf, unsigned max) {
    void **fp = (void **)frame;

    if (fp == NULL)
        return 0;

    unsigned n = 0;
    while (n < max) {
        void *pc = fp[1];
        if (pc == NULL)
            break;
        buf[n++] = pc;

        uintptr_t parent = (uintptr_t)fp[0];
        if (parent <= (uintptr_t)fp ||
            parent - (uintptr_t)fp > WMM_PROF_MAX_FRAME_BYTES ||
            (parent & (sizeof(void *) - 1u)) != 0)
            break;
        fp = (void **)parent;
    }
    return n;
}

/* 逐槽 seqlock:seq 写奇、写 payload、写偶,最后 release 发布 head。
   环满时旧槽会被覆盖,合并端用 index 识别并跳过。 */
static void prof_ring_push(wmm_prof_ring_t *r, void *const *pc,
                           unsigned depth, size_t usize, uint32_t weight) {
    uint64_t idx = atomic_load_explicit(&r->head, memory_order_relaxed);
    wmm_prof_slot_t *s = &r->slot[idx % WMM_PROF_RING];

    uint32_t seq = atomic_load_explicit(&s->seq, memory_order_relaxed);
    atomic_store_explicit(&s->seq, seq + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_acq_rel);

    s->depth  = depth;
    s->index  = idx;
    s->usize  = usize;
    s->weight = weight;
    memcpy(s->pc, pc, (size_t)depth * sizeof pc[0]);

    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&s->seq, seq + 2, memory_order_relaxed);
    atomic_store_explicit(&r->head, idx + 1, memory_order_release);
}

/* 每 sample_n 次分配采一个样本;weight 记 n,用于还原总量 */
__attribute__((noinline))
void wmm_prof_sample(wmm_tcache_t *t, size_t usize, void *alloc_frame) {
    if (t == NULL)
        return;
    unsigned n = g_opt.sample_n;
    if (n == 0)
        return;

    if (++t->prof_tick < n)
        return;
    t->prof_tick = 0;

    void *pc[WMM_PROF_DEPTH];
    unsigned depth = wmm_backtrace_from(alloc_frame, pc, WMM_PROF_DEPTH);
    if (depth == 0)
        return;

    prof_ring_push(&t->prof, pc, depth, usize, n);
}

/* 聚合键:前 WMM_PROF_KEY_FRAMES 帧与深度;bytes/count 是累积值 */
typedef struct {
    uintptr_t pc[WMM_PROF_KEY_FRAMES];
    uint32_t  depth;
    uint32_t  pad;
    uint64_t  bytes;
    uint64_t  count;
} wmm_prof_bucket_t;

static wmm_prof_bucket_t g_prof_tab[WMM_PROF_TAB_N];

pthread_mutex_t g_prof_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t prof_hash(void *const *pc, unsigned n) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned i = 0; i < n; i++) {
        uint64_t v = (uint64_t)(uintptr_t)pc[i];
        for (unsigned b = 0; b < 8; b++) {
            h ^= (v >> (b * 8)) & 0xffu;
            h *= 1099511628211ull;
        }
    }
    return h;
}

/* 开放寻址;空槽先初始化键再比较,同键累加 */
static void prof_agg_add(void *const *pc, unsigned depth,
                         size_t usize, uint32_t weight) {
    unsigned n = depth < WMM_PROF_KEY_FRAMES ? depth : WMM_PROF_KEY_FRAMES;
    if (n == 0 || weight == 0)
        return;

    uint64_t h = prof_hash(pc, n);
    size_t   mask = (size_t)WMM_PROF_TAB_N - 1;

    for (size_t probe = 0; probe < WMM_PROF_TAB_N; probe++) {
        wmm_prof_bucket_t *b = &g_prof_tab[(size_t)(h + probe) & mask];

        if (b->depth == 0) {
            for (unsigned i = 0; i < n; i++)
                b->pc[i] = (uintptr_t)pc[i];
            for (unsigned i = n; i < WMM_PROF_KEY_FRAMES; i++)
                b->pc[i] = 0;
            b->depth = n;
            b->pad   = 0;
            b->bytes = 0;
            b->count = 0;
        }
        if (b->depth != n)
            continue;

        bool same = true;
        for (unsigned i = 0; i < n; i++)
            if (b->pc[i] != (uintptr_t)pc[i]) {
                same = false;
                break;
            }
        if (!same)
            continue;

        b->bytes += (uint64_t)usize * weight;
        b->count += weight;
        return;
    }
}

/* 把环里尚未合并的槽并入聚合表;写入中或已被覆盖的槽跳过。
   调用方必须持有 g_prof_lock。 */
static uint64_t prof_ring_merge_locked(wmm_tcache_t *t) {
    wmm_prof_ring_t *r = &t->prof;
    uint64_t head   = atomic_load_explicit(&r->head, memory_order_acquire);
    uint64_t merged = r->merged;

    if (head - merged > WMM_PROF_RING)
        merged = head - WMM_PROF_RING;

    uint64_t n = 0;
    for (; merged < head; merged++) {
        wmm_prof_slot_t *s = &r->slot[merged % WMM_PROF_RING];

        uint32_t s1 = atomic_load_explicit(&s->seq, memory_order_acquire);
        if ((s1 & 1u) != 0)
            continue;
        if (s->index != merged)
            continue;

        unsigned depth = s->depth;
        if (depth > WMM_PROF_DEPTH)
            depth = WMM_PROF_DEPTH;
        size_t   usize  = s->usize;
        uint32_t weight = s->weight;
        void    *pc[WMM_PROF_DEPTH];
        if (depth != 0)
            memcpy(pc, s->pc, (size_t)depth * sizeof pc[0]);

        atomic_thread_fence(memory_order_acquire);
        uint32_t s2 = atomic_load_explicit(&s->seq, memory_order_relaxed);
        if (s1 != s2)
            continue;

        prof_agg_add(pc, depth, usize, weight);
        n++;
    }
    r->merged = merged;
    return n;
}

/* 锁序:g_threads_lock -> g_prof_lock */
void wmm_prof_merge(void) {
    pthread_mutex_lock(&g_threads_lock);
    pthread_mutex_lock(&g_prof_lock);
    for (wmm_tcache_t *t = g_threads; t != NULL; t = t->next)
        (void)prof_ring_merge_locked(t);
    pthread_mutex_unlock(&g_prof_lock);
    pthread_mutex_unlock(&g_threads_lock);
}

/* 线程退出前把它环里的样本并掉,避免丢样本 */
void wmm_prof_thread_drain(wmm_tcache_t *t) {
    if (t == NULL)
        return;
    pthread_mutex_lock(&g_prof_lock);
    (void)prof_ring_merge_locked(t);
    pthread_mutex_unlock(&g_prof_lock);
}

/* 先合并,再按 bytes/count/pc 降序插入排序取前 k */
unsigned wmm_prof_top(wmm_prof_entry_t *dst, unsigned k) {
    if (dst == NULL || k == 0)
        return 0;
    if (k > WMM_PROF_TOP_MAX)
        k = WMM_PROF_TOP_MAX;

    wmm_prof_merge();

    pthread_mutex_lock(&g_prof_lock);
    memset(dst, 0, (size_t)k * sizeof dst[0]);

    unsigned n = 0;
    for (size_t i = 0; i < WMM_PROF_TAB_N; i++) {
        const wmm_prof_bucket_t *b = &g_prof_tab[i];
        if (b->depth == 0)
            continue;

        unsigned pos = 0;
        while (pos < n &&
               (dst[pos].bytes > b->bytes ||
                (dst[pos].bytes == b->bytes &&
                 (dst[pos].count > b->count ||
                  (dst[pos].count == b->count &&
                   dst[pos].pc <= b->pc[0])))))
            pos++;
        if (pos >= k)
            continue;

        unsigned last = n < k ? n : k - 1;
        for (unsigned j = last; j > pos; j--)
            dst[j] = dst[j - 1];
        dst[pos].pc       = b->pc[0];
        dst[pos].bytes    = b->bytes;
        dst[pos].count    = b->count;
        dst[pos].reserved = 0;
        if (n < k)
            n++;
    }
    pthread_mutex_unlock(&g_prof_lock);
    return n;
}
