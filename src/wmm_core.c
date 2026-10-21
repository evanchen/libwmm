/* wmm_core.c —— 页表访问、slab/bin 操作与公共分配入口 */

#include "wmm_internal.h"

#include <assert.h>
#include <string.h>

/* 页表项读写:下标 = (p - 窗口基址) / 页大小 */
wmm_pte_t wmm_pte_get(const void *p) {
    return g_pte[((uintptr_t)p - (uintptr_t)g_heap_base) >> WMM_LG_PAGE];
}

void wmm_pte_set(const void *p, wmm_pte_t v) {
    g_pte[((uintptr_t)p - (uintptr_t)g_heap_base) >> WMM_LG_PAGE] = v;
}

bool wmm_pte_is_slab(const void *p, wmm_slab_t **out) {
    wmm_pte_t e = wmm_pte_get(p);
    if (e.raw == 0 || (e.raw & 1u) != 0)
        return false;
    *out = (wmm_slab_t *)(e.raw & ~(uintptr_t)1);
    return true;
}

bool wmm_pte_is_extent(const void *p, void **out_ext) {
    wmm_pte_t e = wmm_pte_get(p);
    if (e.raw == 0 || (e.raw & 1u) == 0)
        return false;
    *out_ext = (void *)(e.raw & ~(uintptr_t)1);
    return true;
}

/* 普通 slab:空闲链串在对象自身前 4 字节,并登记每页页表 */
void wmm_slab_init(wmm_slab_t *sl, void *addr, szind_t i) {
    const wmm_bin_info_t *bi = &wmm_bin_info[i];

    sl->szind     = i;
    sl->nfree     = bi->nregs;
    sl->free_head = 0;
    sl->nregs     = bi->nregs;
    sl->reg_size  = bi->reg_size;
    sl->data_off  = 0;
    sl->guarded   = false;
    memset(sl->pad, 0, sizeof sl->pad);
    sl->addr      = addr;
    sl->site_pc   = 0;
    sl->full_prev = NULL;
    sl->full_next = NULL;
    sl->ph_prev   = NULL;
    sl->ph_next   = NULL;
    sl->ph_child  = NULL;
    sl->ext       = NULL;

    for (uint32_t r = 0; r + 1 < bi->nregs; r++)
        *(uint32_t *)((char *)addr + (size_t)r * bi->reg_size) = r + 1;
    *(uint32_t *)((char *)addr + (size_t)(bi->nregs - 1) * bi->reg_size)
        = UINT32_MAX;

    wmm_pte_t e;
    e.raw = (uintptr_t)sl;
    size_t npages = bi->slab_size >> WMM_LG_PAGE;
    for (size_t pg = 0; pg < npages; pg++)
        wmm_pte_set((char *)addr + pg * WMM_PAGE, e);
}

static void guard_fill_redzones(wmm_slab_t *sl) {
    memset(sl->addr, 0xBD, sl->data_off);
    memset((char *)sl->addr + sl->data_off + sl->reg_size,
           0xBD, WMM_GUARD_RZ);
}

/* guard slab:整块只放一个对象,前垫按对象对齐(上限一页) */
void wmm_slab_init_guard(wmm_slab_t *sl, void *addr, szind_t i,
                         size_t npages) {
    const wmm_bin_info_t *bi = &wmm_bin_info[i];
    uint32_t off = wmm_guard_data_off(bi->reg_size);

    assert((size_t)off + bi->reg_size + WMM_GUARD_RZ <= npages * WMM_PAGE);

    sl->szind     = i;
    sl->nfree     = 1;
    sl->free_head = UINT32_MAX;
    sl->nregs     = 1;
    sl->reg_size  = bi->reg_size;
    sl->data_off  = off;
    sl->guarded   = true;
    memset(sl->pad, 0, sizeof sl->pad);
    sl->addr      = addr;
    sl->site_pc   = 0;
    sl->full_prev = NULL;
    sl->full_next = NULL;
    sl->ph_prev   = NULL;
    sl->ph_next   = NULL;
    sl->ph_child  = NULL;
    sl->ext       = NULL;

    guard_fill_redzones(sl);

    wmm_pte_t e;
    e.raw = (uintptr_t)sl;
    for (size_t pg = 0; pg < npages; pg++)
        wmm_pte_set((char *)addr + pg * WMM_PAGE, e);
}

/* 复用前重置单对象状态并重画 redzone */
void wmm_slab_guard_rearm(wmm_slab_t *sl) {
    sl->nfree     = 1;
    sl->free_head = UINT32_MAX;
    sl->site_pc   = 0;
    sl->full_prev = NULL;
    sl->full_next = NULL;
    sl->ph_prev   = NULL;
    sl->ph_next   = NULL;
    sl->ph_child  = NULL;
    guard_fill_redzones(sl);
}

/* 从空闲链头取对象;guard slab 直接返回唯一数据区 */
void *wmm_slab_alloc(wmm_slab_t *sl) {
    assert(sl->nfree > 0);

    if (sl->guarded) {
        sl->nfree--;
        return (char *)sl->addr + sl->data_off;
    }

    uint32_t r    = sl->free_head;
    char    *p    = (char *)sl->addr + (size_t)r * sl->reg_size;
    sl->free_head = *(uint32_t *)p;
    sl->nfree--;
    return p;
}

/* 归还到空闲链头;guard slab 填 0xBC,让 UAF 读到明显脏值 */
void wmm_slab_free(wmm_slab_t *sl, void *p) {
    if (sl->guarded) {
        memset(p, 0xBC, sl->reg_size);
        sl->nfree++;
        return;
    }

    uint32_t r = (uint32_t)(((uintptr_t)p - (uintptr_t)sl->addr) / sl->reg_size);
    *(uint32_t *)p = sl->free_head;
    sl->free_head  = r;
    sl->nfree++;
}

/* 按地址插入 nonfull 链,保持地址序 */
static void nonfull_insert(wmm_slab_t **root, wmm_slab_t *sl) {
    wmm_slab_t *cur  = *root;
    wmm_slab_t *prev = NULL;

    while (cur != NULL && (uintptr_t)cur->addr < (uintptr_t)sl->addr) {
        prev = cur;
        cur  = cur->ph_next;
    }
    sl->ph_prev = prev;
    sl->ph_next = cur;
    if (prev != NULL)
        prev->ph_next = sl;
    else
        *root = sl;
    if (cur != NULL)
        cur->ph_prev = sl;
}

static void nonfull_remove(wmm_slab_t **root, wmm_slab_t *sl) {
    if (sl->ph_prev != NULL)
        sl->ph_prev->ph_next = sl->ph_next;
    else
        *root = sl->ph_next;
    if (sl->ph_next != NULL)
        sl->ph_next->ph_prev = sl->ph_prev;
    sl->ph_prev = NULL;
    sl->ph_next = NULL;
}

static void bin_push_full(wmm_bin_t *b, wmm_slab_t *sl) {
    sl->full_prev = NULL;
    sl->full_next = b->full_head;
    if (b->full_head != NULL)
        b->full_head->full_prev = sl;
    b->full_head = sl;
}

static void bin_unlink_full(wmm_bin_t *b, wmm_slab_t *sl) {
    if (sl->full_prev != NULL)
        sl->full_prev->full_next = sl->full_next;
    else
        b->full_head = sl->full_next;
    if (sl->full_next != NULL)
        sl->full_next->full_prev = sl->full_prev;
    sl->full_prev = NULL;
    sl->full_next = NULL;
}

/* 新 slab 只能装到空的热槽位;调用方持 bin 锁 */
void wmm_bin_slab_install(wmm_bin_t *b, wmm_slab_t *sl) {
    assert(b->slabcur == NULL);

    b->slabcur   = sl;
    b->nslabs++;
    b->curslabs++;
}

/* 先用热 slab,否则弹一个 nonfull;都没有返回 NULL */
void *wmm_bin_alloc(wmm_bin_t *b) {
    if (b->slabcur != NULL && b->slabcur->nfree > 0) {
        b->live++;
        return wmm_slab_alloc(b->slabcur);
    }

    if (b->slabcur != NULL) {
        bin_push_full(b, b->slabcur);
        b->slabcur = NULL;
    }

    if (b->nonfull_root != NULL) {
        wmm_slab_t *sl = b->nonfull_root;
        nonfull_remove(&b->nonfull_root, sl);
        b->slabcur = sl;
        b->live++;
        return wmm_slab_alloc(sl);
    }

    return NULL;
}

/* 归还对象并更新 live/ndalloc;返回 true 表示 slab 已全空,
   调用方负责释放页。单对象类释放即退役。 */
bool wmm_bin_dalloc(wmm_bin_t *b, wmm_slab_t *sl, void *p) {
    wmm_slab_free(sl, p);
    b->live--;
    b->ndalloc++;

    if (sl == b->slabcur) {
        if (sl->nfree == sl->nregs) {
            b->slabcur = NULL;
            b->curslabs--;
            return true;
        }
        return false;
    }

    if (sl->nfree == sl->nregs) {
        if (sl->nregs == 1)
            bin_unlink_full(b, sl);
        else
            nonfull_remove(&b->nonfull_root, sl);
        b->curslabs--;
        return true;
    }

    if (sl->nfree == 1) {
        bin_unlink_full(b, sl);
        nonfull_insert(&b->nonfull_root, sl);
    }

    return false;
}

/* 在 bin 锁内批量取对象;不足时新建 slab,取多少返回多少 */
unsigned wmm_bin_fill(wmm_bin_t *b, szind_t i, unsigned want, void **out) {
    unsigned n = 0;

    pthread_mutex_lock(&b->lock);
    while (n < want) {
        void *p = wmm_bin_alloc(b);
        if (p == NULL) {
            wmm_slab_t *sl = wmm_phys_alloc_slab(i);
            if (sl == NULL)
                break;
            wmm_bin_slab_install(b, sl);
            p = wmm_bin_alloc(b);
            if (p == NULL)
                break;
        }
        out[n++] = p;
    }
    pthread_mutex_unlock(&b->lock);
    return n;
}

/* 指针 -> 对象下标;仅用于地址对齐的 slab */
uint32_t wmm_div_regind(const void *p, const void *base, uint32_t reg_size) {
    return (uint32_t)(((uintptr_t)p - (uintptr_t)base) / reg_size);
}

/* 统计与采样都要线程缓存;未初始化则补建 */
static wmm_tcache_t *stats_slot(void) {
    if (g_tcache == NULL)
        wmm_thread_init();
    return g_tcache;
}

/* 采样需要的帧指针起点 */
#define WMM_ALLOC_FRAME() __builtin_frame_address(0)

/* 分配入口:大对象走 extent;guard 走专用 bin;其余走 tcache,
   缓存空则批量填充,再失败才落到 bin 慢路径。 */
void *wmm_alloc(size_t size) {
    if (size == 0)
        size = 1;

    if (size > WMM_SMALL_MAXCLASS) {
        wmm_extent_t *ext = wmm_phys_alloc_extent(size);
        if (ext == NULL)
            return NULL;
        wmm_tcache_t *t = stats_slot();
        if (t != NULL) {
            wmm_tcache_count_alloc(t, ext->npages * WMM_PAGE);

            if (g_opt.sample_n != 0)
                wmm_prof_sample(t, ext->npages * WMM_PAGE,
                                WMM_ALLOC_FRAME());
        }
        return ext->addr;
    }

    szind_t i = wmm_size2index(size);

    if (g_opt.guard) {
        size_t reg = wmm_bin_info[i].reg_size;
        void  *p = wmm_guard_alloc(i, (uintptr_t)__builtin_return_address(0));
        if (p != NULL) {
            wmm_tcache_t *t = stats_slot();
            if (t != NULL) {
                wmm_tcache_count_alloc(t, reg);
                if (g_opt.sample_n != 0)
                    wmm_prof_sample(t, reg, WMM_ALLOC_FRAME());
            }
        }
        return p;
    }

    if (g_tcache == NULL)
        wmm_thread_init();
    if (g_tcache != NULL && g_tcache->enabled) {
        void *p = wmm_tcache_alloc(i);
        if (p == NULL) {
            wmm_tcache_fill(i, g_tcache->bins[i].nfill);
            p = wmm_tcache_alloc(i);
        }
        if (p != NULL) {
            wmm_tcache_count_alloc(g_tcache, wmm_bin_info[i].reg_size);
            if (g_opt.sample_n != 0)
                wmm_prof_sample(g_tcache, wmm_bin_info[i].reg_size,
                                WMM_ALLOC_FRAME());
            return p;
        }
    }

    wmm_bin_t *b = &g_bins[i];

    pthread_mutex_lock(&b->lock);
    void *p = wmm_bin_alloc(b);
    if (p == NULL) {
        wmm_slab_t *sl = wmm_phys_alloc_slab(i);
        if (sl != NULL) {
            wmm_bin_slab_install(b, sl);
            p = wmm_bin_alloc(b);
        }
    }
    pthread_mutex_unlock(&b->lock);
    if (p != NULL && g_tcache != NULL) {
        wmm_tcache_count_alloc(g_tcache, wmm_bin_info[i].reg_size);
        if (g_opt.sample_n != 0)
            wmm_prof_sample(g_tcache, wmm_bin_info[i].reg_size,
                            WMM_ALLOC_FRAME());
    }
    return p;
}

/* 释放入口:先拒绝窗口外指针,再按页表项区分 slab 与 extent。
   slab 分 guard、tcache、bin 三条路径;无效指针只记事件不崩。 */
void wmm_free(void *p) {
    if (p == NULL)
        return;

    if ((uintptr_t)p < (uintptr_t)g_heap_base ||
        (uintptr_t)p >= (uintptr_t)g_heap_end) {
        wmm_event_log(0xBE, p);
        wmm_dprintf("free: %p out of heap window (not from wmm?)\n", p);
        return;
    }

    wmm_slab_t *sl;
    void       *ext;

    if (wmm_pte_is_slab(p, &sl)) {
        size_t usize = sl->reg_size;

        if (sl->guarded) {
            bool ok = wmm_guard_free(sl, p,
                                     (uintptr_t)__builtin_return_address(0));
            if (ok && g_tcache != NULL)
                wmm_tcache_count_free(g_tcache, usize);
            return;
        }

        if (g_tcache != NULL && g_tcache->enabled) {
            wmm_tcache_count_free(g_tcache, usize);
            wmm_tcache_free(sl->szind, p);
            return;
        }

        wmm_bin_t *b = &g_bins[sl->szind];
        pthread_mutex_lock(&b->lock);
        bool retire = wmm_bin_dalloc(b, sl, p);
        pthread_mutex_unlock(&b->lock);
        if (g_tcache != NULL)
            wmm_tcache_count_free(g_tcache, usize);

        if (retire)
            wmm_phys_retire_slab(sl);
        return;
    }

    if (wmm_pte_is_extent(p, &ext)) {
        wmm_extent_t *e = (wmm_extent_t *)ext;

        size_t bytes = (size_t)((char *)e->addr + e->npages * WMM_PAGE
                                - (char *)p);
        wmm_phys_free_extent(e);
        if (g_tcache != NULL)
            wmm_tcache_count_free(g_tcache, bytes);
        return;
    }

    wmm_event_log(0xBE, p);
    wmm_dprintf("free: %p not allocated (double free or wild pointer?)\n", p);
}

/* 乘积溢出返回 NULL;取得内存后清零 */
void *wmm_calloc(size_t n, size_t size) {
    if (size != 0 && n > SIZE_MAX / size)
        return NULL;

    size_t bytes = n * size;
    void *p = wmm_alloc(bytes);
    if (p == NULL)
        return NULL;

    memset(p, 0, bytes);
    return p;
}

/* 乘积溢出置 ENOMEM,旧块保持有效 */
void *wmm_reallocarray(void *p, size_t n, size_t size) {
    if (size != 0 && n > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    return wmm_realloc(p, n * size);
}

/* 收缩原地返回;扩容分配新块,拷贝 min(旧可用, 新请求) 后释放旧块。
   失败时旧块保持有效。 */
void *wmm_realloc(void *p, size_t size) {
    if (p == NULL)
        return wmm_alloc(size);
    if (size == 0) {
        wmm_free(p);
        return NULL;
    }

    size_t usable = wmm_usable_size(p);
    if (size <= usable)
        return p;

    void *q = wmm_alloc(size);
    if (q == NULL)
        return NULL;

    memcpy(q, p, usable < size ? usable : size);
    wmm_free(p);
    return q;
}

/* 窗口外或未登记返回 0;大对象可用量算到 extent 末尾 */
size_t wmm_usable_size(const void *p) {
    if (p == NULL)
        return 0;
    if ((uintptr_t)p < (uintptr_t)g_heap_base ||
        (uintptr_t)p >= (uintptr_t)g_heap_end)
        return 0;

    wmm_slab_t *sl;
    void       *ext;
    if (wmm_pte_is_slab(p, &sl))
        return sl->reg_size;
    if (wmm_pte_is_extent(p, &ext)) {
        wmm_extent_t *e = (wmm_extent_t *)ext;

        return (size_t)((char *)e->addr + e->npages * WMM_PAGE - (char *)p);
    }
    return 0;
}

/* <=8 走普通分配;<=页大小按对齐向上取整;更大则多取
   alignment 字节的 extent,再对齐切出。 */
static void *alloc_aligned(size_t alignment, size_t size) {
    if (alignment <= 8)
        return wmm_alloc(size);

    if (alignment <= WMM_PAGE) {
        /* 向上取整前先挡住加法回绕;size 为 0 时按一个 alignment
           分配,否则会落进更小的尺寸类、指针不满足对齐 */
        if (size == 0)
            size = alignment;
        else if (size > SIZE_MAX - (alignment - 1))
            return NULL;

        size_t rounded = (size + alignment - 1) & ~(alignment - 1);
        return wmm_alloc(rounded);
    }

    if (size > SIZE_MAX - alignment || alignment > WMM_HEAP_VA_SIZE)
        return NULL;

    wmm_extent_t *ext = wmm_phys_alloc_extent(size + alignment);
    if (ext == NULL)
        return NULL;

    uintptr_t base = (uintptr_t)ext->addr;
    void     *p    = (void *)((base + alignment - 1)
                              & ~(uintptr_t)(alignment - 1));

    size_t bytes = (size_t)((char *)ext->addr + ext->npages * WMM_PAGE
                            - (char *)p);
    wmm_tcache_t *t = stats_slot();
    if (t != NULL) {
        wmm_tcache_count_alloc(t, bytes);
        if (g_opt.sample_n != 0)
            wmm_prof_sample(t, bytes, WMM_ALLOC_FRAME());
    }

    return p;
}

int wmm_posix_memalign(void **out, size_t alignment, size_t size) {
    if (out == NULL)
        return EINVAL;
    *out = NULL;

    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
        return EINVAL;

    void *p = alloc_aligned(alignment, size);
    if (p == NULL)
        return ENOMEM;
    *out = p;
    return 0;
}

void *wmm_aligned_alloc(size_t alignment, size_t size) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        errno = EINVAL;
        return NULL;
    }

    void *p = alloc_aligned(alignment, size);
    if (p == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    return p;
}
