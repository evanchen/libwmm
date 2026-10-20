/* wmm_guard.c —— 内存错误检测与泄漏告警
 *
 * 职责:poison(释放后填 0xBC,再分配时被改 = UAF 已发生)、
 * redzone(对象前后放哨兵字节,踩哨兵 = 越界写)、quarantine
 * (释放先进隔离区延迟复用,隔离期间被写 = UAF)、双释放/野指针
 * (free 时页表反查直接暴露);以及泄漏三档:持续上涨告警、
 * TOP N 增量对比、全量 dump。全部默认关闭,运行期开关。
 *
 * 边界先说清楚,免得对能力有不切实际的预期:
 *   - 只有走 guard 通道的小对象有 redzone。大对象整页分配,前后没有
 *     独立的哨兵页——给每块大对象加哨兵要改物理层的交付粒度,收益
 *     不抵复杂度;它们的越界写由 M4 的页边界(越界即段错误)兜底。
 *   - 只检测"写",不检测"读":redzone/poison 被读走不会改变内容,
 *     纯软件方案无从得知。要抓越界读得上 MPK/ASan 一类机制。
 *   - 检测有假阴性:写入值与哨兵/poison 相同,或对象被复用之后的
 *     陈旧指针,都无法识别。
 *
 * 状态机(每个 guard slab 只有一个对象):
 *
 *   分配 ----> 满(nfree=0,挂在 slabcur 或 full 链)
 *                |  free:校验 redzone,数据区填 0xBC
 *                v
 *   隔离队列 FIFO(容量 512)---每秒 tick:校验 poison(0xBF)
 *                |                      v
 *              类别回收池 --- 下次同类分配:校验 poison(0xBC),
 *                            重铺 redzone 后复用
 *
 *   guard.disable:隔离区与回收池全部校验后把页还给物理层,slab 头
 *   进回收链;仍在应用手里的对象保持 guard 布局,释放时直接归还。
 *
 * 为什么隔离单位是整块 slab:一个 guard slab 只装一个对象,释放即
 * 整块空闲,"进入隔离"与"全空退休"是同一时刻。若一个 slab 装多个
 * 对象,就得让隔离中的对象从空闲链里消失、同时保持 slab 其余部分
 * 可分配,状态组合会多出一整维。
 *
 * 锁:g_guard_lock 保护 guard bin 计数、隔离队列、回收池与头回收链。
 * 锁序 bins → guard → ext → threads → prof → base,与 fork prepare
 * 一致。事件锁是叶子:持 guard 锁写事件不会构成环。
 */

#include "wmm_internal.h"

#include <string.h>

/* ==================== 全局状态 ==================== */

wmm_bin_t       g_guard_bins[WMM_NBINS];   /* guard 专用池;lock 字段闲置,
                                              访问一律由 g_guard_lock 串行化 */
pthread_mutex_t g_guard_lock = PTHREAD_MUTEX_INITIALIZER;

/* 隔离队列:FIFO。链接借用 slab 的 full_next/full_prev——进入隔离
   的 slab 已从 bin 摘除,满链指针没有别的用途。 */
static wmm_slab_t *g_q_head;
static wmm_slab_t *g_q_tail;
static unsigned    g_q_n;

/* 类别回收池:LIFO,同样借 full_next 串链。池里的 slab 页仍在映射
   里,复用不需要任何系统调用;代价是峰值内存保留到 guard.disable。 */
static wmm_slab_t *g_pool[WMM_NBINS];

/* 已交还物理层、只剩头的 slab 壳。guard.disable 之后再次 enable 时
   从这里取头,避免 base 区随开关次数单调增长。 */
static wmm_slab_t *g_spare_headers;

/* ==================== 尺寸与哨兵 ==================== */

/* guard slab 需要多少页:前哨兵/对齐垫层 + 数据区 + 尾部哨兵。 */
static size_t guard_npages(szind_t i) {
    size_t bytes = (size_t)wmm_guard_data_off(wmm_bin_info[i].reg_size)
                 + wmm_bin_info[i].reg_size + WMM_GUARD_RZ;
    return (bytes + WMM_PAGE - 1) >> WMM_LG_PAGE;
}

/* 返回区间内第一个不等于 want 的字节;全相等返回 NULL。检查本身
   就是一次线性扫描:只在 free/tick/复用三个低频点跑,不碰分配
   快路径;判据"一个字节不对就算破坏",不做任何启发式。 */
static const uint8_t *first_bad(const void *p, size_t n, uint8_t want) {
    const uint8_t *q = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++)
        if (q[i] != want)
            return q + i;
    return NULL;
}

/* 校验两段 redzone。at_free 为真时事件的两帧是"释放点 + 分配点";
   否则(隔离到期/池复用)只有分配点可报——破坏发生的时刻已经过去,
   本库无法回溯是谁写的。 */
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

/* ==================== 隔离队列与回收池 ==================== */

static void pool_push_locked(wmm_slab_t *sl) {
    sl->full_prev = NULL;
    sl->full_next = g_pool[sl->szind];
    g_pool[sl->szind] = sl;
}

/* 隔离到期:校验隔离期间的写入(0xBF),然后转入回收池。不重铺
   哨兵也不清 poison——池里再被写,复用点的检查还要报 0xBC。 */
static void quarantine_reap_locked(wmm_slab_t *sl) {
    guard_check_redzones(sl, 0, false);

    const uint8_t *bad = first_bad((char *)sl->addr + sl->data_off,
                                   sl->reg_size, 0xBC);
    if (bad != NULL)
        wmm_event_log_site(0xBF, (void *)bad, sl->site_pc, 0);

    pool_push_locked(sl);
}

static void quarantine_push_locked(wmm_slab_t *sl) {
    sl->full_prev = g_q_tail;
    sl->full_next = NULL;
    if (g_q_tail != NULL)
        g_q_tail->full_next = sl;
    else
        g_q_head = sl;
    g_q_tail = sl;
    g_q_n++;

    /* 队满:最旧的一项提前转池。释放路径因此有界,不会因为应用
       突然大批释放而无限延期归还。 */
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

/* 把 slab 的页还给物理层,头留作下次 enable 的壳。调用方持 guard
   锁;phys_free_slab 内部拿 ext 锁,锁序 guard → ext 合法。 */
static void release_slab_locked(wmm_slab_t *sl) {
    wmm_phys_free_slab(sl);
    sl->ext       = NULL;
    sl->full_prev = NULL;
    sl->full_next = g_spare_headers;
    g_spare_headers = sl;
}

/* 取一块可用的 guard slab(已持锁):优先回收池(零系统调用),
   否则复用头壳 + 向物理层要页。池里的对象在上一次释放时已填
   poison;这里的检查覆盖"进池之后又被写"的窗口,事件类型用
   0xBC(释放后内存被改)。 */
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
            wmm_event_log(0xDB, NULL);   /* OOM:slab 头元数据耗尽 */
            return NULL;
        }
    }

    if (wmm_phys_alloc_slab_pages(sl, i, guard_npages(i), true) == NULL) {
        /* 页没拿到:头不能丢,放回壳链等下次 */
        sl->full_prev = NULL;
        sl->full_next = g_spare_headers;
        g_spare_headers = sl;
        return NULL;
    }
    return sl;
}

/* ==================== 分配/释放 ==================== */

void *wmm_guard_alloc(szind_t i, uintptr_t site_pc) {
    pthread_mutex_lock(&g_guard_lock);

    wmm_bin_t *b = &g_guard_bins[i];
    void      *p = wmm_bin_alloc(b);
    if (p == NULL) {
        wmm_slab_t *sl = guard_slab_get_locked(i);
        if (sl != NULL) {
            wmm_bin_slab_install(b, sl);
            p = wmm_bin_alloc(b);   /* 必成功:全空 slab 至少弹出一个 */
        }
    }

    /* 成功路径的 slab 一定是 b->slabcur:guard 从不产生非满链成员,
       而刚安装的 slab 就在热位。把用户分配点记在 slab 上,泄漏全量
       dump 与后续检查事件都靠它归因。 */
    if (p != NULL)
        b->slabcur->site_pc = site_pc;

    pthread_mutex_unlock(&g_guard_lock);
    return p;
}

bool wmm_guard_free(wmm_slab_t *sl, void *p, uintptr_t free_pc) {
    pthread_mutex_lock(&g_guard_lock);

    char *data = (char *)sl->addr + sl->data_off;

    /* 有效对象地址只有一个:guard slab 的唯一数据区。其余地址
       (内部指针、redzone 里的指针、邻块指针)都按野指针记事件。 */
    if (p != data) {
        wmm_event_log_site(0xBE, p, free_pc, sl->site_pc);
        pthread_mutex_unlock(&g_guard_lock);
        return false;
    }

    /* 对象在应用手里时 nfree 恒为 0。非 0 说明它已经在隔离区或
       回收池——同一指针第二次进来就是双重释放。页表在隔离/池期间
       保持有效,所以这里还能走到判据;真正归还物理层之后,页表
       清零,双重释放由 wmm_free 的 raw==0 分支接住。 */
    if (sl->nfree != 0) {
        wmm_event_log_site(0xBE, p, free_pc, sl->site_pc);
        pthread_mutex_unlock(&g_guard_lock);
        return false;
    }

    /* 先查 redzone 再写 poison:顺序其实不影响判据(两段不相交),
       但先查能把事件的两帧填成"释放点 + 分配点",归因更直接。 */
    guard_check_redzones(sl, free_pc, true);

    wmm_bin_t *b   = &g_guard_bins[sl->szind];
    bool       ret = wmm_bin_dalloc(b, sl, p);   /* 数据区在此填 0xBC */

    if (ret) {
        if (g_opt.guard)
            quarantine_push_locked(sl);
        else
            release_slab_locked(sl);   /* 检测已关:不再保留隔离期 */
    }

    pthread_mutex_unlock(&g_guard_lock);
    return true;
}

/* ==================== 周期维护 ==================== */

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

void wmm_guard_disable(void) {
    pthread_mutex_lock(&g_guard_lock);
    g_opt.guard = false;   /* 先关入口:之后的新分配回常规路径 */

    /* 隔离区里的对象按"仍在隔离期"记 0xBF;回收池里的按"已释放"
       记 0xBC。两者都在归还物理页之前做最后一次校验——页一旦还掉,
       这些对象地址也就无从检查了。 */
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

/* ==================== 泄漏 A 档:全量 dump ==================== */

/* 把一个存活对象计入按调用点聚合的数组。同点合并;表满则丢弃——
   dump 的容量由调用方定,不能为了多记一条去分配内存。 */
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

        /* nfree==0 = 对象在应用手里。slabcur 允许是空闲的(刚装好
           还没分配),不能无条件算存活。 */
        if (b->slabcur != NULL && b->slabcur->nfree == 0)
            dump_add(dst, &n, k, b->slabcur->site_pc, size);

        for (wmm_slab_t *sl = b->full_head; sl != NULL; sl = sl->full_next)
            dump_add(dst, &n, k, sl->site_pc, size);

        /* nonfull_root 对 guard 池恒为空:单对象 slab 从不满转非满 */
    }

    pthread_mutex_unlock(&g_guard_lock);
    return n;
}

/* 把 TOP 表按 pc[0] 合并。prof.top 的桶键是前 4 帧,同一个直接
   调用点经不同调用栈进来会产生多行;增量对比关心的是"用户调用点
   在涨",按 pc[0] 归并后基线查找才不会有歧义。合并只会让某行字节
   变大,可能破坏降序,但下面的插入排序不依赖输入有序。 */
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

/* ==================== 泄漏 B 档:TOP N 增量 ==================== */

/* 上一次调用时的累计表(按 pc 索引的 TOP N)。表是累计量,增量靠
   两次调用做差;基线随每次调用前移。TOP N 截断意味着掉出榜的调用
   点会从基线消失——它的历史增量不再参与比较,这是"TOP N 对比"的
   固有口径,不是计数错误。 */
static wmm_prof_entry_t g_leak_base[WMM_PROF_TOP_MAX];
static unsigned         g_leak_base_n;
static bool             g_leak_base_valid;

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
        /* 首次调用只建立基线:没有上一次的读数,任何"增量"都是
           把进程启动以来的全部历史算进来,必然误报。 */
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

        /* 按增量字节降序、增量次数降序、pc 升序插入:与 prof.top
           同一排序口径,监控端做两次差分时顺序稳定。 */
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

/* ==================== 泄漏 C 档:持续上涨告警 ==================== */

/* 窗口状态全部由 g_guard_lock 保护;后台线程每秒推进一步,测试也
   可以直接调 wmm_leak_tick。 */
static uint64_t g_leak_last;          /* 上一个窗口的 live_bytes */
static uint64_t g_leak_streak_start;  /* 本轮连续上涨的起点值 */
static unsigned g_leak_rise;          /* 连续上涨的窗口数 */
static bool     g_leak_alerted;       /* 本轮上涨已告警,不再重复 */
static bool     g_leak_primed;        /* 是否已有第一个窗口 */

uint64_t wmm_live_bytes_now(void) {
    uint64_t bytes = 0;

    /* 逐类持锁读,不一次性持有全部 bin 锁:这是后台每秒一次的
       采样,读数跨几个瞬间可以接受;若同时持 36 把锁,任何一次
       分配慢路径都会被它拖住。锁序仍是 bins → guard → ext,只是
       三段串行、不嵌套。 */
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
        /* 不涨(持平或回落)就打断连续上涨。回落还重新武装告警:
           一次泄漏被释放之后再次泄漏,应当能再次报出来。 */
        if (now < g_leak_last)
            g_leak_alerted = false;
        g_leak_rise = 0;
    }
    g_leak_last = now;

    pthread_mutex_unlock(&g_guard_lock);

    if (alert) {
        /* 告警本身只有"在涨"这一个信息,定位靠自动打开的调用点
           采样:没有采样就没有归因,告警等于只喊不指路。先开采样
           再写事件:读到 0xDA 的消费者可以放心认为采样已经打开,
           不会撞上"事件可见但开关尚未落下"的瞬间。0xDA 的 ptr
           为空——泄漏不是一个地址,而是一段趋势。 */
        (void)wmm_ctl("sampling.enable");
        wmm_event_log(0xDA, NULL);
    }
}

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
