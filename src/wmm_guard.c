/* wmm_guard.c —— guard slab、隔离队列与泄漏巡检 */

#include "wmm_internal.h"

#include <stdlib.h>
#include <string.h>

wmm_bin_t       g_guard_bins[WMM_NBINS];

pthread_mutex_t g_guard_lock = PTHREAD_MUTEX_INITIALIZER;

/* 隔离队列 FIFO;超过 WMM_GUARD_QUARANTINE 就回收最旧的 */
static wmm_slab_t *g_q_head;
static wmm_slab_t *g_q_tail;
static unsigned    g_q_n;

/* 每尺寸类可复用的 guard slab 空闲池 */
static wmm_slab_t *g_pool[WMM_NBINS];

/* 页已归还、头可复用的 slab */
static wmm_slab_t *g_spare_headers;

/* 一个 guard slab 需要的页数:前垫 + 对象 + 后 redzone */
static size_t guard_npages(szind_t i) {
    size_t bytes = (size_t)wmm_guard_data_off(wmm_bin_info[i].reg_size)
                 + wmm_bin_info[i].reg_size + WMM_GUARD_RZ;
    return (bytes + WMM_PAGE - 1) >> WMM_LG_PAGE;
}

static const uint8_t *first_bad(const void *p, size_t n, uint8_t want) {
    const uint8_t *q = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++)
        if (q[i] != want)
            return q + i;
    return NULL;
}

/* 检查前后 redzone;发现破坏时连同分配/释放点记事件 */
static void guard_check_redzones(wmm_slab_t *sl, uintptr_t free_pc,
                                 bool at_free) {
    const uint8_t *bad = first_bad(sl->addr, sl->data_off, 0xBD);
    if (bad == NULL)
        bad = first_bad((char *)sl->addr + sl->data_off + sl->reg_size,
                        WMM_GUARD_RZ, 0xBD);
    if (bad == NULL)
        return;

    wmm_event_log_site(0xBD, (void *)bad,
                       at_free ? free_pc : sl->site_pc,
                       at_free ? sl->site_pc : 0);
}

static void pool_push_locked(wmm_slab_t *sl) {
    sl->full_prev = NULL;
    sl->full_next = g_pool[sl->szind];
    g_pool[sl->szind] = sl;
}

/* 出隔离队列:查 redzone 与释放填充,再把 slab 放回空闲池 */
static void quarantine_reap_locked(wmm_slab_t *sl) {
    guard_check_redzones(sl, 0, false);

    const uint8_t *bad = first_bad((char *)sl->addr + sl->data_off,
                                   sl->reg_size, 0xBC);
    if (bad != NULL)
        wmm_event_log_site(0xBF, (void *)bad, sl->site_pc, 0);

    pool_push_locked(sl);
}

/* 释放的 guard slab 先隔离,超上限才回收最旧的 */
static void quarantine_push_locked(wmm_slab_t *sl) {
    sl->full_prev = g_q_tail;
    sl->full_next = NULL;
    if (g_q_tail != NULL)
        g_q_tail->full_next = sl;
    else
        g_q_head = sl;
    g_q_tail = sl;
    g_q_n++;

    while (g_q_n > WMM_GUARD_QUARANTINE) {
        wmm_slab_t *old = g_q_head;
        g_q_head = old->full_next;
        if (g_q_head != NULL)
            g_q_head->full_prev = NULL;
        else
            g_q_tail = NULL;
        g_q_n--;
        quarantine_reap_locked(old);
    }
}

/* 还页,slab 头挂到备用链 */
static void release_slab_locked(wmm_slab_t *sl) {
    wmm_phys_free_slab(sl);
    sl->ext       = NULL;
    sl->full_prev = NULL;
    sl->full_next = g_spare_headers;
    g_spare_headers = sl;
}

/* 优先复用本类空闲池;复用时先检查上一轮留下的越界与 UAF 痕迹。
   没有可复用块时从备用头链或 base 区取头,再取新页。 */
static wmm_slab_t *guard_slab_get_locked(szind_t i) {
    wmm_slab_t *sl = g_pool[i];
    if (sl != NULL) {
        g_pool[i] = sl->full_next;

        guard_check_redzones(sl, 0, false);
        const uint8_t *bad = first_bad((char *)sl->addr + sl->data_off,
                                       sl->reg_size, 0xBC);
        if (bad != NULL)
            wmm_event_log_site(0xBC, (void *)bad, sl->site_pc, 0);

        wmm_slab_guard_rearm(sl);
        return sl;
    }

    sl = g_spare_headers;
    if (sl != NULL)
        g_spare_headers = sl->full_next;
    else {
        sl = (wmm_slab_t *)wmm_base_alloc(sizeof(*sl));
        if (sl == NULL) {
            wmm_event_log(0xDB, NULL);
            if (g_opt.abort_oom)
                abort();
            return NULL;
        }
    }

    if (wmm_phys_alloc_slab_pages(sl, i, guard_npages(i), true) == NULL) {
        sl->full_prev = NULL;
        sl->full_next = g_spare_headers;
        g_spare_headers = sl;
        return NULL;
    }
    return sl;
}

/* 走 guard 专用 bin;记录本次分配的调用点用于泄漏归因 */
void *wmm_guard_alloc(szind_t i, uintptr_t site_pc) {
    pthread_mutex_lock(&g_guard_lock);

    wmm_bin_t *b = &g_guard_bins[i];
    void      *p = wmm_bin_alloc(b);
    if (p == NULL) {
        wmm_slab_t *sl = guard_slab_get_locked(i);
        if (sl != NULL) {
            wmm_bin_slab_install(b, sl);
            p = wmm_bin_alloc(b);
        }
    }

    if (p != NULL)
        b->slabcur->site_pc = site_pc;

    pthread_mutex_unlock(&g_guard_lock);
    return p;
}

/* 校验指针必须是数据区且对象确实已分配,再查 redzone;slab 退役后,
   guard 仍开着就进隔离队列,否则立即还页。 */
bool wmm_guard_free(wmm_slab_t *sl, void *p, uintptr_t free_pc) {
    pthread_mutex_lock(&g_guard_lock);

    char *data = (char *)sl->addr + sl->data_off;

    if (p != data) {
        wmm_event_log_site(0xBE, p, free_pc, sl->site_pc);
        pthread_mutex_unlock(&g_guard_lock);
        return false;
    }

    if (sl->nfree != 0) {
        wmm_event_log_site(0xBE, p, free_pc, sl->site_pc);
        pthread_mutex_unlock(&g_guard_lock);
        return false;
    }

    guard_check_redzones(sl, free_pc, true);

    wmm_bin_t *b   = &g_guard_bins[sl->szind];
    bool       ret = wmm_bin_dalloc(b, sl, p);

    if (ret) {
        if (g_opt.guard)
            quarantine_push_locked(sl);
        else
            release_slab_locked(sl);
    }

    pthread_mutex_unlock(&g_guard_lock);
    return true;
}

/* 后台巡检:排空隔离队列 */
void wmm_guard_tick(void) {
    pthread_mutex_lock(&g_guard_lock);
    while (g_q_head != NULL) {
        wmm_slab_t *sl = g_q_head;
        g_q_head = sl->full_next;
        if (g_q_head != NULL)
            g_q_head->full_prev = NULL;
        else
            g_q_tail = NULL;
        g_q_n--;
        quarantine_reap_locked(sl);
    }
    pthread_mutex_unlock(&g_guard_lock);
}

/* 关闭 guard:排空隔离队列与空闲池,期间仍检查并上报 */
void wmm_guard_disable(void) {
    pthread_mutex_lock(&g_guard_lock);
    g_opt.guard = false;

    while (g_q_head != NULL) {
        wmm_slab_t *sl = g_q_head;
        g_q_head = sl->full_next;
        if (g_q_head != NULL)
            g_q_head->full_prev = NULL;
        else
            g_q_tail = NULL;
        g_q_n--;

        guard_check_redzones(sl, 0, false);
        const uint8_t *bad = first_bad((char *)sl->addr + sl->data_off,
                                       sl->reg_size, 0xBC);
        if (bad != NULL)
            wmm_event_log_site(0xBF, (void *)bad, sl->site_pc, 0);
        release_slab_locked(sl);
    }

    for (szind_t i = 0; i < WMM_NBINS; i++) {
        while (g_pool[i] != NULL) {
            wmm_slab_t *sl = g_pool[i];
            g_pool[i] = sl->full_next;

            guard_check_redzones(sl, 0, false);
            const uint8_t *bad = first_bad((char *)sl->addr + sl->data_off,
                                           sl->reg_size, 0xBC);
            if (bad != NULL)
                wmm_event_log_site(0xBC, (void *)bad, sl->site_pc, 0);
            release_slab_locked(sl);
        }
    }

    pthread_mutex_unlock(&g_guard_lock);
}

static void dump_add(wmm_prof_entry_t *dst, unsigned *n, unsigned k,
                     uintptr_t pc, uint32_t size) {
    for (unsigned i = 0; i < *n; i++) {
        if (dst[i].pc == pc) {
            dst[i].bytes += size;
            dst[i].count++;
            return;
        }
    }
    if (*n >= k)
        return;
    dst[*n].pc       = pc;
    dst[*n].bytes    = size;
    dst[*n].count    = 1;
    dst[*n].reserved = 0;
    (*n)++;
}

/* 汇总仍存活的 guard 对象,按分配点归并 */
unsigned wmm_guard_leak_dump(wmm_prof_entry_t *dst, unsigned k) {
    if (dst == NULL || k == 0)
        return 0;
    if (k > WMM_PROF_TOP_MAX)
        k = WMM_PROF_TOP_MAX;
    memset(dst, 0, (size_t)k * sizeof dst[0]);

    unsigned n = 0;
    pthread_mutex_lock(&g_guard_lock);

    for (szind_t i = 0; i < WMM_NBINS; i++) {
        wmm_bin_t *b = &g_guard_bins[i];
        uint32_t   size = wmm_bin_info[i].reg_size;

        if (b->slabcur != NULL && b->slabcur->nfree == 0)
            dump_add(dst, &n, k, b->slabcur->site_pc, size);

        for (wmm_slab_t *sl = b->full_head; sl != NULL; sl = sl->full_next)
            dump_add(dst, &n, k, sl->site_pc, size);
    }

    pthread_mutex_unlock(&g_guard_lock);
    return n;
}

/* 同一首帧可能来自不同深度的键,按 pc 再归并一次 */
static unsigned leak_agg_by_pc(wmm_prof_entry_t *tab, unsigned n) {
    unsigned out = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned j = 0;
        for (; j < out; j++) {
            if (tab[j].pc == tab[i].pc) {
                tab[j].bytes += tab[i].bytes;
                tab[j].count += tab[i].count;
                break;
            }
        }
        if (j == out)
            tab[out++] = tab[i];
    }
    return out;
}

/* 泄漏比较基线:上次 check 的 TOP 表 */
static wmm_prof_entry_t g_leak_base[WMM_PROF_TOP_MAX];
static unsigned         g_leak_base_n;
static bool             g_leak_base_valid;

/* 与上次基线比较,返回增长最多的 k 项;首次调用只建基线并返回 0 */
int wmm_leak_check(int k, wmm_prof_entry_t *out) {
    if (out == NULL || k < 1 || (unsigned)k > WMM_PROF_TOP_MAX) {
        errno = EINVAL;
        return -1;
    }

    wmm_prof_entry_t cur[WMM_PROF_TOP_MAX];
    unsigned cn = wmm_prof_top(cur, WMM_PROF_TOP_MAX);
    cn = leak_agg_by_pc(cur, cn);

    wmm_prof_entry_t delta[WMM_PROF_TOP_MAX];
    unsigned         dn = 0;

    pthread_mutex_lock(&g_guard_lock);

    if (!g_leak_base_valid) {
        memcpy(g_leak_base, cur, sizeof cur);
        g_leak_base_n     = cn;
        g_leak_base_valid = true;
        pthread_mutex_unlock(&g_guard_lock);
        memset(out, 0, (size_t)k * sizeof out[0]);
        return 0;
    }

    for (unsigned i = 0; i < cn; i++) {
        uint64_t prev_bytes = 0;
        uint64_t prev_count = 0;
        for (unsigned j = 0; j < g_leak_base_n; j++) {
            if (g_leak_base[j].pc == cur[i].pc) {
                prev_bytes = g_leak_base[j].bytes;
                prev_count = g_leak_base[j].count;
                break;
            }
        }
        if (cur[i].bytes <= prev_bytes)
            continue;

        uint64_t db = cur[i].bytes - prev_bytes;
        uint64_t dc = cur[i].count > prev_count
                        ? cur[i].count - prev_count : 0;

        unsigned pos = 0;
        while (pos < dn &&
               (delta[pos].bytes > db ||
                (delta[pos].bytes == db &&
                 (delta[pos].count > dc ||
                  (delta[pos].count == dc && delta[pos].pc < cur[i].pc)))))
            pos++;
        if (pos >= WMM_PROF_TOP_MAX)
            continue;

        unsigned last = dn < WMM_PROF_TOP_MAX ? dn : WMM_PROF_TOP_MAX - 1;
        for (unsigned j = last; j > pos; j--)
            delta[j] = delta[j - 1];
        delta[pos].pc       = cur[i].pc;
        delta[pos].bytes    = db;
        delta[pos].count    = dc;
        delta[pos].reserved = 0;
        if (dn < WMM_PROF_TOP_MAX)
            dn++;
    }

    memcpy(g_leak_base, cur, sizeof cur);
    g_leak_base_n = cn;
    pthread_mutex_unlock(&g_guard_lock);

    unsigned outn = dn < (unsigned)k ? dn : (unsigned)k;
    memset(out, 0, (size_t)k * sizeof out[0]);
    memcpy(out, delta, (size_t)outn * sizeof out[0]);
    return (int)outn;
}

/* 连续上涨窗口状态;由 g_guard_lock 保护 */
static uint64_t g_leak_last;
static uint64_t g_leak_streak_start;
static unsigned g_leak_rise;
static bool     g_leak_alerted;
static bool     g_leak_primed;

/* 当前存活字节:各 bin + guard bin + 大对象 */
uint64_t wmm_live_bytes_now(void) {
    uint64_t bytes = 0;

    for (szind_t i = 0; i < WMM_NBINS; i++) {
        wmm_bin_t *b = &g_bins[i];
        pthread_mutex_lock(&b->lock);
        bytes += b->live * (uint64_t)wmm_bin_info[i].reg_size;
        pthread_mutex_unlock(&b->lock);
    }

    pthread_mutex_lock(&g_guard_lock);
    for (szind_t i = 0; i < WMM_NBINS; i++)
        bytes += g_guard_bins[i].live * (uint64_t)wmm_bin_info[i].reg_size;
    pthread_mutex_unlock(&g_guard_lock);

    pthread_mutex_lock(&g_ext_lock);
    bytes += g_large.live_bytes;
    pthread_mutex_unlock(&g_ext_lock);

    return bytes;
}

/* 连续 WMM_LEAK_WINDOWS 个周期上涨且累计涨幅达标时报警:
   打开采样并记泄漏事件。 */
void wmm_leak_tick(void) {
    uint64_t now = wmm_live_bytes_now();
    bool     alert = false;

    pthread_mutex_lock(&g_guard_lock);

    if (!g_leak_primed) {
        g_leak_primed       = true;
        g_leak_streak_start = now;
        g_leak_rise         = 0;
    } else if (now > g_leak_last) {
        if (g_leak_rise == 0)
            g_leak_streak_start = g_leak_last;
        g_leak_rise++;
        if (!g_leak_alerted &&
            g_leak_rise >= WMM_LEAK_WINDOWS &&
            now - g_leak_streak_start >= WMM_LEAK_GROWTH) {
            g_leak_alerted = true;
            alert = true;
        }
    } else {
        if (now < g_leak_last)
            g_leak_alerted = false;
        g_leak_rise = 0;
    }
    g_leak_last = now;

    pthread_mutex_unlock(&g_guard_lock);

    if (alert) {
        (void)wmm_ctl("sampling.enable");
        wmm_event_log(0xDA, NULL);
    }
}

/* 清空泄漏检测基线与窗口状态 */
void wmm_leak_reset(void) {
    pthread_mutex_lock(&g_guard_lock);
    g_leak_last         = 0;
    g_leak_streak_start = 0;
    g_leak_rise         = 0;
    g_leak_alerted      = false;
    g_leak_primed       = false;
    g_leak_base_valid   = false;
    g_leak_base_n       = 0;
    pthread_mutex_unlock(&g_guard_lock);
}
