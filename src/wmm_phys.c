/* wmm_phys.c —— 地址序 extent 管理、purge 与后台线程 */

#include "wmm_internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

/* 地址序 extent 链尾与空闲结点链;均由 g_ext_lock 保护 */
static wmm_extent_t *g_ar_tail;

static wmm_extent_t *g_ext_free_nodes;

/* 结点优先复用空闲链;不够时从 base 区取 */
static wmm_extent_t *ext_node_get(void) {
    wmm_extent_t *e = g_ext_free_nodes;
    if (e != NULL)
        g_ext_free_nodes = e->next;
    else
        e = (wmm_extent_t *)wmm_base_alloc(sizeof(wmm_extent_t));

    return e;
}

static void ext_node_put(wmm_extent_t *e) {
    e->next = g_ext_free_nodes;
    g_ext_free_nodes = e;
}

/* 插到 pos 之后;pos 为 NULL 表示插到链头 */
static void ar_insert_after(wmm_extent_t *pos, wmm_extent_t *e) {
    e->ar_prev = pos;
    if (pos != NULL) {
        e->ar_next = pos->ar_next;
        if (pos->ar_next != NULL)
            pos->ar_next->ar_prev = e;
        else
            g_ar_tail = e;
        pos->ar_next = e;
    } else {
        e->ar_next = g_ar_head;
        if (g_ar_head != NULL)
            g_ar_head->ar_prev = e;
        else
            g_ar_tail = e;
        g_ar_head = e;
    }
}

static void ar_unlink(wmm_extent_t *e) {
    if (e->ar_prev != NULL)
        e->ar_prev->ar_next = e->ar_next;
    else
        g_ar_head = e->ar_next;
    if (e->ar_next != NULL)
        e->ar_next->ar_prev = e->ar_prev;
    else
        g_ar_tail = e->ar_prev;
    e->ar_prev = NULL;
    e->ar_next = NULL;
}

/* 从窗口 bump 映射;MAP_FIXED 复用预留区间;失败记 OOM 事件 */
static void *window_map(size_t npages) {
    size_t bytes = npages * WMM_PAGE;
    if ((char *)g_heap_cur + bytes > (char *)g_heap_end) {
        size_t used = (size_t)((char *)g_heap_cur - (char *)g_heap_base);
        size_t win  = (size_t)((char *)g_heap_end - (char *)g_heap_base);
        wmm_dprintf("OOM: heap window exhausted, %zu MB in use (window %zu MB)\n",
                    used >> 20, win >> 20);
        wmm_event_log(0xDB, NULL);
        if (g_opt.abort_oom)
            abort();
        return NULL;
    }

    if (wmm_fault_check(WMM_FAULT_MAP)) {
        wmm_dprintf("OOM: injected mmap failure, %zu pages\n", npages);
        wmm_event_log(0xDB, NULL);
        if (g_opt.abort_oom)
            abort();
        return NULL;
    }

    void *p = mmap(g_heap_cur, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        wmm_dprintf("OOM: mmap %zu pages failed, errno %d\n", npages, errno);
        wmm_event_log(0xDB, NULL);
        if (g_opt.abort_oom)
            abort();
        return NULL;
    }
    g_heap_cur = (char *)p + bytes;
    g_active_bytes += bytes;
    return p;
}

/* 两遍首次适配:先找 FREE_DIRTY,再找 FREE_RETAINED;
   大块切成请求大小,余量按原状态挂回地址链。 */
static wmm_extent_t *extent_get(size_t npages) {
    for (int pass = 0; pass < 2; pass++) {
        for (wmm_extent_t *e = g_ar_head; e != NULL; e = e->ar_next) {
            if (e->state == WMM_EXT_INUSE || e->npages < npages)
                continue;
            if (pass == 0 && e->state != WMM_EXT_FREE_DIRTY)
                continue;

            bool   was_dirty  = (e->state == WMM_EXT_FREE_DIRTY);
            size_t old_npages = e->npages;
            bool   split_done = false;

            if (e->npages > npages) {
                wmm_extent_t *rem = ext_node_get();
                if (rem != NULL) {
                    rem->addr   = (char *)e->addr + npages * WMM_PAGE;
                    rem->npages = e->npages - npages;
                    rem->state  = e->state;
                    rem->slab   = NULL;
                    rem->next   = NULL;
                    ar_insert_after(e, rem);
                    e->npages = npages;
                    split_done = true;
                }
            }

            e->state = WMM_EXT_INUSE;
            e->slab  = NULL;
            if (was_dirty) {
                g_dirty_pages -= split_done ? npages : old_npages;
            } else {
                g_active_bytes += split_done ? npages * WMM_PAGE
                                             : old_npages * WMM_PAGE;
            }
            return e;
        }
    }

    void *addr = window_map(npages);
    if (addr == NULL)
        return NULL;

    wmm_extent_t *e = ext_node_get();
    if (e == NULL) {
        wmm_event_log(0xDB, NULL);
        return NULL;
    }

    e->addr   = addr;
    e->npages = npages;
    e->state  = WMM_EXT_INUSE;
    e->slab   = NULL;
    e->next   = NULL;
    ar_insert_after(g_ar_tail, e);
    return e;
}

/* 释放 extent:清页表项,标脏并向前后合并。合并已 purge 的邻块时
   把它重新计为脏页与 active,因为整段现在又可复用。 */
static void extent_release(wmm_extent_t *e) {
    wmm_pte_t zero;
    zero.raw = 0;
    for (size_t pg = 0; pg < e->npages; pg++)
        wmm_pte_set((char *)e->addr + pg * WMM_PAGE, zero);

    e->state = WMM_EXT_FREE_DIRTY;
    e->slab  = NULL;
    g_dirty_pages += e->npages;

    wmm_extent_t *prev = e->ar_prev;
    while (prev != NULL && prev->state != WMM_EXT_INUSE &&
           (char *)prev->addr + prev->npages * WMM_PAGE == (char *)e->addr) {
        wmm_extent_t *p2 = prev->ar_prev;
        if (prev->state == WMM_EXT_FREE_RETAINED) {
            g_dirty_pages += prev->npages;
            g_active_bytes += prev->npages * WMM_PAGE;
        }
        prev->state = WMM_EXT_FREE_DIRTY;
        prev->npages += e->npages;
        ar_unlink(e);
        ext_node_put(e);
        e = prev;
        prev = p2;
    }

    wmm_extent_t *next = e->ar_next;
    while (next != NULL && next->state != WMM_EXT_INUSE &&
           (char *)e->addr + e->npages * WMM_PAGE == (char *)next->addr) {
        wmm_extent_t *n2 = next->ar_next;
        if (next->state == WMM_EXT_FREE_RETAINED) {
            g_dirty_pages += next->npages;
            g_active_bytes += next->npages * WMM_PAGE;
        }
        e->npages += next->npages;
        ar_unlink(next);
        ext_node_put(next);
        next = n2;
    }
}

/* 取 extent 并初始化 slab 头;失败时不动已分配的 sl 头 */
wmm_slab_t *wmm_phys_alloc_slab_pages(wmm_slab_t *sl, szind_t i,
                                      size_t npages, bool guarded) {
    pthread_mutex_lock(&g_ext_lock);
    wmm_extent_t *e = extent_get(npages);
    pthread_mutex_unlock(&g_ext_lock);
    if (e == NULL)
        return NULL;

    if (guarded)
        wmm_slab_init_guard(sl, e->addr, i, npages);
    else
        wmm_slab_init(sl, e->addr, i);

    sl->ext = e;
    e->slab = sl;
    return sl;
}

/* 新建 slab:头从 base 区取,页从物理层取 */
wmm_slab_t *wmm_phys_alloc_slab(szind_t i) {
    const wmm_bin_info_t *bi = &wmm_bin_info[i];

    wmm_slab_t *sl = (wmm_slab_t *)wmm_base_alloc(sizeof(wmm_slab_t));
    if (sl == NULL) {
        wmm_event_log(0xDB, NULL);
        return NULL;
    }

    if (wmm_phys_alloc_slab_pages(sl, i, bi->slab_size >> WMM_LG_PAGE,
                                  false) == NULL)
        return NULL;

    return sl;
}

/* 归还页;sl 自带页(ext == NULL)时什么都不做 */
void wmm_phys_free_slab(wmm_slab_t *sl) {
    wmm_extent_t *e = (wmm_extent_t *)sl->ext;

    if (e == NULL)
        return;

    pthread_mutex_lock(&g_ext_lock);
    extent_release(e);
    pthread_mutex_unlock(&g_ext_lock);
}

/* 大对象 extent:页表项打 extent 标签,并计入大对象统计 */
wmm_extent_t *wmm_phys_alloc_extent(size_t size) {
    if (size > WMM_HEAP_VA_SIZE) {
        wmm_dprintf("OOM: request %zu MB exceeds heap window\n", size >> 20);
        wmm_event_log(0xDB, NULL);
        if (g_opt.abort_oom)
            abort();
        return NULL;
    }

    size_t npages = (size + WMM_PAGE - 1) >> WMM_LG_PAGE;

    pthread_mutex_lock(&g_ext_lock);
    wmm_extent_t *e = extent_get(npages);
    if (e != NULL) {
        wmm_pte_t pte;
        pte.raw = (uintptr_t)e | 1u;
        for (size_t pg = 0; pg < npages; pg++)
            wmm_pte_set((char *)e->addr + pg * WMM_PAGE, pte);

        g_large.live++;
        g_large.live_bytes += npages * WMM_PAGE;
    }
    pthread_mutex_unlock(&g_ext_lock);
    return e;
}

/* 先清统计,再交给 extent_release 合并 */
void wmm_phys_free_extent(wmm_extent_t *ext) {
    pthread_mutex_lock(&g_ext_lock);
    g_large.live--;
    g_large.live_bytes -= ext->npages * WMM_PAGE;
    g_large.ndalloc++;
    extent_release(ext);
    pthread_mutex_unlock(&g_ext_lock);
}

/* 对每个脏 extent 做 madvise;成功后转 retained,active 相应减少 */
static size_t purge_dirty_locked(void) {
    size_t purged = 0;

    for (wmm_extent_t *e = g_ar_head; e != NULL; e = e->ar_next) {
        if (e->state != WMM_EXT_FREE_DIRTY)
            continue;

        if (wmm_fault_check(WMM_FAULT_MADVISE))
            continue;

        if (madvise(e->addr, e->npages * WMM_PAGE, g_opt.madvise_mode) != 0)
            continue;
        e->state = WMM_EXT_FREE_RETAINED;
        g_dirty_pages -= e->npages;
        g_active_bytes -= e->npages * WMM_PAGE;
        purged += e->npages;
    }
    if (purged != 0)
        wmm_event_log(0xDC, NULL);
    return purged;
}

void wmm_phys_purge_if_needed(void) {
    pthread_mutex_lock(&g_ext_lock);

    if (g_dirty_pages * WMM_PAGE > g_opt.dirty_max) {
        size_t purged = purge_dirty_locked();
        if (g_opt.verbose && purged != 0)
            wmm_dprintf("purge: %zu MB returned to kernel\n",
                        (purged * WMM_PAGE) >> 20);
    }
    pthread_mutex_unlock(&g_ext_lock);
}

void wmm_phys_purge_now(void) {
    pthread_mutex_lock(&g_ext_lock);
    (void)purge_dirty_locked();
    pthread_mutex_unlock(&g_ext_lock);
}

/* 后台周期:回收、巡检、采样合并与共享区发布 */
#define WMM_BG_PERIOD_SEC 1

static pthread_t       g_bg_tid;
static pthread_mutex_t g_bg_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_bg_cond = PTHREAD_COND_INITIALIZER;
static bool            g_bg_running;
static bool            g_bg_stop;

/* 周期任务线程;由 g_bg_stop 与条件变量控制退出 */
static void *wmm_bg_thread(void *arg) {
    (void)arg;

    pthread_mutex_lock(&g_bg_lock);
    for (;;) {
        struct timespec ts;
        (void)clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += WMM_BG_PERIOD_SEC;

        int rc = 0;
        while (!g_bg_stop && rc != ETIMEDOUT)
            rc = pthread_cond_timedwait(&g_bg_cond, &g_bg_lock, &ts);
        if (g_bg_stop)
            break;

        pthread_mutex_unlock(&g_bg_lock);
        wmm_phys_purge_if_needed();

        wmm_guard_tick();
        wmm_leak_tick();

        wmm_prof_merge();

        wmm_shm_publish();
        pthread_mutex_lock(&g_bg_lock);
    }
    pthread_mutex_unlock(&g_bg_lock);
    return NULL;
}

/* 幂等:已在运行则直接返回 0 */
int wmm_bg_start(void) {
    int rc = 0;

    pthread_mutex_lock(&g_bg_lock);
    if (!g_bg_running) {
        g_bg_stop = false;
        rc = pthread_create(&g_bg_tid, NULL, wmm_bg_thread, NULL);
        if (rc == 0)
            g_bg_running = true;
    }
    pthread_mutex_unlock(&g_bg_lock);

    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}

/* 置停止标志、唤醒并 join;未运行直接返回 0 */
int wmm_bg_stop(void) {
    pthread_t tid;

    pthread_mutex_lock(&g_bg_lock);
    if (!g_bg_running) {
        pthread_mutex_unlock(&g_bg_lock);
        return 0;
    }
    g_bg_stop = true;
    tid = g_bg_tid;
    pthread_cond_broadcast(&g_bg_cond);
    pthread_mutex_unlock(&g_bg_lock);

    int rc = pthread_join(tid, NULL);

    pthread_mutex_lock(&g_bg_lock);
    g_bg_running = false;
    pthread_mutex_unlock(&g_bg_lock);

    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}

/* fork 后子进程:重建锁与条件变量,后台线程不继承 */
void wmm_bg_child_reset(void) {
    (void)pthread_mutex_init(&g_bg_lock, NULL);
    (void)pthread_cond_init(&g_bg_cond, NULL);
    g_bg_running = false;
    g_bg_stop    = false;
    memset(&g_bg_tid, 0, sizeof(g_bg_tid));
}
