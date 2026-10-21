/* wmm_tcache.c —— 每线程对象缓存:填充、溢出回收与自适应 GC */

#define _GNU_SOURCE

#include "wmm_internal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

/* 一次 mmap 装下结构体与各 bin 的对象栈;容量取 2*nregs,
   夹在 20..200。析构键注册失败直接 abort,否则线程退出会漏缓存。 */
static bool tcache_create(void) {
    uint16_t ncm[WMM_NBINS];
    size_t   total = sizeof(wmm_tcache_t);

    for (szind_t i = 0; i < WMM_NBINS; i++) {
        unsigned v = 2u * wmm_bin_info[i].nregs;
        if (v < WMM_TCACHE_NCACHED_MIN)
            v = WMM_TCACHE_NCACHED_MIN;
        if (v > WMM_TCACHE_NCACHED_MAX)
            v = WMM_TCACHE_NCACHED_MAX;
        ncm[i] = (uint16_t)v;
        total += (size_t)v * sizeof(void *);
    }

    if (wmm_fault_check(WMM_FAULT_TCACHE))
        return false;

    void *blk = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (blk == MAP_FAILED)
        return false;

    wmm_tcache_t *t = (wmm_tcache_t *)blk;

    t->prev    = NULL;
    t->next    = NULL;
    t->tid     = (uint32_t)gettid();
    t->enabled = true;
    t->map_size = total;

    atomic_init(&t->t_alloc_bytes, 0);
    atomic_init(&t->t_free_bytes, 0);
    atomic_init(&t->t_alloc_cnt, 0);
    atomic_init(&t->t_free_cnt, 0);
    t->gc_evt        = 0;
    t->gc_next       = 0;

    atomic_init(&t->gc_req, 0);

    t->prof_tick = 0;
    atomic_init(&t->prof.head, 0);
    t->prof.merged = 0;

    char *stacks = (char *)(t + 1);
    for (szind_t i = 0; i < WMM_NBINS; i++) {
        wmm_cache_bin_t *cb = &t->bins[i];
        cb->stack       = (void **)stacks;
        cb->ncached_max = ncm[i];
        cb->head        = 0;
        cb->low_water   = 0;
        cb->nfill       = (uint16_t)(ncm[i] >> 1);
        cb->nrequests   = 0;
        stacks += (size_t)ncm[i] * sizeof(void *);
    }

    t->bins[WMM_NBINS].stack       = NULL;
    t->bins[WMM_NBINS].ncached_max = 0;
    t->bins[WMM_NBINS].head        = 0;
    t->bins[WMM_NBINS].low_water   = 0;
    t->bins[WMM_NBINS].nfill       = 0;
    t->bins[WMM_NBINS].nrequests   = 0;

    pthread_mutex_lock(&g_threads_lock);
    t->next = g_threads;
    if (g_threads != NULL)
        g_threads->prev = t;
    g_threads = t;
    pthread_mutex_unlock(&g_threads_lock);

    g_tcache = t;

    if (g_tcache_key == (pthread_key_t)-1 ||
        pthread_setspecific(g_tcache_key, t) != 0) {
        wmm_dprintf("fatal: cannot register thread cache of tid %u for "
                    "exit-time reclamation\n", t->tid);
        abort();
    }
    return true;
}

/* 懒初始化;失败后 g_tcache 保持 NULL,调用方走 bin 慢路径 */
void wmm_thread_init(void) {
    if (g_tcache == NULL)
        (void)tcache_create();
}

/* pthread 键析构:线程退出时归还缓存 */
void wmm_tcache_thread_destructor(void *val) {
    (void)val;
    wmm_thread_exit();
}

/* 把栈顶 n 个对象还给 bin;空 slab 先收集,解锁后再还页,
   避免在 bin 锁内取 extent 锁。 */
static void tcache_flush_bins(wmm_tcache_t *t, szind_t i, unsigned n) {
    wmm_cache_bin_t *cb = &t->bins[i];

    if (n > cb->head)
        n = cb->head;
    if (n == 0)
        return;

    wmm_bin_t  *b = &g_bins[i];
    wmm_slab_t *retired[WMM_TCACHE_NCACHED_MAX];
    unsigned    nret = 0;

    pthread_mutex_lock(&b->lock);
    for (unsigned k = 0; k < n; k++) {
        void *p = cb->stack[k];

        if ((uintptr_t)p < (uintptr_t)g_heap_base ||
            (uintptr_t)p >= (uintptr_t)g_heap_end) {
            wmm_dprintf("tcache flush: %p outside heap, dropped\n", p);
            continue;
        }
        wmm_slab_t *sl;
        if (!wmm_pte_is_slab(p, &sl) || sl->szind != i) {
            wmm_dprintf("tcache flush: %p bad page table entry, dropped\n", p);
            continue;
        }
        if (wmm_bin_dalloc(b, sl, p) && nret < WMM_TCACHE_NCACHED_MAX)
            retired[nret++] = sl;
    }
    pthread_mutex_unlock(&b->lock);

    for (unsigned k = 0; k < nret; k++)
        wmm_phys_retire_slab(retired[k]);

    memmove(cb->stack, cb->stack + n,
            (size_t)(cb->head - n) * sizeof(void *));
    cb->head = (uint16_t)(cb->head - n);
    cb->low_water = cb->head;
}

void wmm_tcache_flush(szind_t i, unsigned n) {
    if (g_tcache == NULL)
        return;
    tcache_flush_bins(g_tcache, i, n);
}

/* 把本线程全部缓存刷回 bin */
void wmm_tcache_flush_all(void) {
    if (g_tcache == NULL)
        return;
    for (szind_t i = 0; i < WMM_NBINS; i++)
        if (g_tcache->bins[i].head > 0)
            tcache_flush_bins(g_tcache, i, g_tcache->bins[i].head);
}

/* 自适应收缩:保留量取上次 GC 以来的使用量加 25%;
   上次出现饥饿则填充量加倍,否则减半。 */
static void tcache_gc_bin(wmm_tcache_t *t, szind_t i) {
    wmm_cache_bin_t *cb = &t->bins[i];
    unsigned ncached = cb->head;

    if (ncached == 0)
        return;

    bool     starved       = (cb->low_water == 0);
    unsigned used_since_gc = ncached - cb->low_water;

    if (used_since_gc == 0) {
        if (ncached > 2)
            tcache_flush_bins(t, i, ncached - 2);
    } else {
        unsigned retain = used_since_gc + (used_since_gc >> 2);
        if (retain > cb->ncached_max)
            retain = cb->ncached_max;
        if (ncached > retain)
            tcache_flush_bins(t, i, ncached - retain);
    }

    if (starved) {
        unsigned nf = (unsigned)cb->nfill << 1;
        unsigned cap = (unsigned)(cb->ncached_max >> 1);
        cb->nfill = (uint16_t)(nf > cap ? cap : nf);
    } else if (cb->nfill > 2) {
        cb->nfill = (uint16_t)(cb->nfill >> 1);
    }

    cb->low_water = cb->head;
}

void wmm_tcache_gc(szind_t i) {
    if (g_tcache == NULL)
        return;
    tcache_gc_bin(g_tcache, i);
}

static void tcache_gc_all_bins(wmm_tcache_t *t) {
    for (szind_t i = 0; i < WMM_NBINS; i++)
        tcache_gc_bin(t, i);
}

/* 本线程立即执行;其他线程只置 gc_req,由它们下次释放时执行 */
void wmm_tcache_gc_all(void) {
    wmm_tcache_t *self = g_tcache;

    if (self != NULL) {
        atomic_store_explicit(&self->gc_req, 0, memory_order_relaxed);
        tcache_gc_all_bins(self);
    }

    pthread_mutex_lock(&g_threads_lock);
    for (wmm_tcache_t *t = g_threads; t != NULL; t = t->next)
        if (t != self)
            atomic_store_explicit(&t->gc_req, 1, memory_order_release);
    pthread_mutex_unlock(&g_threads_lock);
}

/* 无锁快路径;只由所属线程调用,栈空返回 NULL 交给填充 */
void *wmm_tcache_alloc(szind_t i) {
    wmm_cache_bin_t *cb = &g_tcache->bins[i];

    if (cb->head == 0)
        return NULL;
    void *p = cb->stack[--cb->head];
    if (cb->head < cb->low_water)
        cb->low_water = cb->head;
    return p;
}

/* 无锁快路径:先响应跨线程 GC 请求;栈满先尝试收缩,仍满则一次
   刷掉约 3/4 容量;每 256 次释放轮转收缩一个 bin。 */
void wmm_tcache_free(szind_t i, void *p) {
    wmm_tcache_t *t = g_tcache;

    if (atomic_load_explicit(&t->gc_req, memory_order_acquire) != 0 &&
        atomic_exchange_explicit(&t->gc_req, 0, memory_order_acquire) != 0)
        tcache_gc_all_bins(t);

    wmm_cache_bin_t *cb = &t->bins[i];

    if (cb->head == cb->ncached_max) {
        tcache_gc_bin(t, i);
        if (cb->head == cb->ncached_max)
            tcache_flush_bins(t, i,
                              (unsigned)(cb->ncached_max - cb->ncached_max / 4));
    }
    cb->stack[cb->head++] = p;

    if (++t->gc_evt >= WMM_TCACHE_GC_EVERY) {
        t->gc_evt = 0;
        szind_t bin = t->gc_next;
        t->gc_next = (uint8_t)((bin + 1) % WMM_NBINS);
        tcache_gc_bin(t, bin);
    }
}

/* 从 bin 批量取对象压栈;只把实际取到的数量计入 head */
void wmm_tcache_fill(szind_t i, unsigned want) {
    wmm_cache_bin_t *cb = &g_tcache->bins[i];

    if ((unsigned)cb->head + want > cb->ncached_max)
        want = (unsigned)(cb->ncached_max - cb->head);
    if (want == 0)
        return;

    unsigned n = wmm_bin_fill(&g_bins[i], i, want, &cb->stack[cb->head]);
    cb->head = (uint16_t)(cb->head + n);
    cb->low_water = cb->head;
}

/* 线程退出:排空采样环与对象栈,从注册表摘除,再 munmap */
void wmm_thread_exit(void) {
    wmm_tcache_t *t = g_tcache;

    if (t == NULL)
        return;

    wmm_prof_thread_drain(t);

    wmm_tcache_flush_all();

    pthread_mutex_lock(&g_threads_lock);
    if (t->prev != NULL)
        t->prev->next = t->next;
    else
        g_threads = t->next;
    if (t->next != NULL)
        t->next->prev = t->prev;
    pthread_mutex_unlock(&g_threads_lock);

    g_tcache = NULL;
    (void)munmap(t, t->map_size);
}

/* 全部线程缓存中的对象总数,统计用 */
uint64_t wmm_tcache_cached_total(void) {
    uint64_t sum = 0;

    pthread_mutex_lock(&g_threads_lock);
    for (wmm_tcache_t *t = g_threads; t != NULL; t = t->next)
        for (szind_t i = 0; i < WMM_NBINS; i++)
            sum += t->bins[i].head;
    pthread_mutex_unlock(&g_threads_lock);
    return sum;
}

/* fork 后:保留当前线程的缓存,其余线程的缓存刷回 bin 后 munmap。
   子进程里那些线程已不存在,不能继续持有对象。 */
void wmm_tcache_child_cleanup(void) {
    wmm_tcache_t *keep  = g_tcache;
    wmm_tcache_t *t     = g_threads;
    bool          found = false;

    g_threads = NULL;
    while (t != NULL) {
        wmm_tcache_t *next = t->next;

        if (t == keep) {
            found = true;
            t->prev = NULL;
            t->next = NULL;
            g_threads = t;
        } else {
            wmm_prof_thread_drain(t);
            for (szind_t i = 0; i < WMM_NBINS; i++)
                if (t->bins[i].head > 0)
                    tcache_flush_bins(t, i, t->bins[i].head);
            (void)munmap(t, t->map_size);
        }
        t = next;
    }

    if (keep != NULL && !found) {
        keep->prev = NULL;
        keep->next = NULL;
        g_threads = keep;
    }
}
