/* wmm_phys.c —— 物理层:页从哪来、还到哪去、如何合并
 *
 * 对主路径的交接单位:slab(小对象,pgs 页)与 extent(大对象,
 * ceil(size/页) 页)。两条路径最终都汇到同一套 extent 记账上:
 *   取页 extent_get:first-fit 沿地址序链找第一个够大的空闲块切出
 *         (复用零系统调用);没有才从窗口 bump 处 mmap 新页;
 *   还页 extent_free:页表立即清零(raw=0),extent 转 FREE_DIRTY,
 *         物理页留在映射里(purge(M10)才真正还内核),然后沿地址
 *         序链向两侧合并——相邻空闲区间永远融成一块,这就是对抗
 *         外部碎片的全部机关。
 *
 * 两个关键设计(理由同时写在 wmm_internal.h 的 extent 段):
 *   页表语义 = "当前归属":释放即清零。不清的话,复用窗口期内对
 *   已释放地址的 double-free 会反查到还在空闲链上的 extent,把它
 *   再 free 一遍,直接毁链;清零后 double-free 反查 raw==0,与
 *   "从未分配"统一为同一条诊断路径。
 *   物理页不立即还:FREE_DIRTY 复用是零系统调用的;立即还(每次
 *   mmap PROT_NONE / madvise)会让"释放→再分配"的循环反复吃缺页,
 *   这正是 jemalloc 攒 dirty 再批量 purge 的动机。M10 在 dirty_pages
 *   超过 g_opt.dirty_max 时统一 madvise。
 *
 * 锁:g_ext_lock 包住全部共享结构(地址序链、bump 指针、回收链、
 * dirty 计数、g_large 统计)。锁内会调 base 锁(叶子),锁序
 * bin → ext → base,与 fork prepare 一致。
 *
 * extent 结点壳复用、slab 头不复用:大对象生灭频繁,结点(64B)
 * 逐块新拿会吃穿 base 区,走回收链;slab 头的生命通常与该类流量
 * 同长,退休时页已入空闲,72B 头部留存不是泄漏主项——若统计显示
 * 不然,同款回收链照加。
 */

#include "wmm_internal.h"

#include <stdlib.h>
#include <sys/mman.h>

static wmm_extent_t *g_ar_tail;        /* 地址序链尾(最高地址)。bump 出的
                                          新页地址恒最大,尾插 O(1),故
                                          缓存尾指针而不再扫链 */
static wmm_extent_t *g_ext_free_nodes; /* 空闲 extent 结点壳的回收链(LIFO) */

/* ==================== 结点壳:回收链 ==================== */

/* 以下内部函数全部假定调用方已持 g_ext_lock(对外接口统一拿锁,
   避免自嵌套死锁——pthread_mutex 非递归)。 */

static wmm_extent_t *ext_node_get(void) {
    wmm_extent_t *e = g_ext_free_nodes;
    if (e != NULL)
        g_ext_free_nodes = e->next;
    else
        e = (wmm_extent_t *)wmm_base_alloc(sizeof(wmm_extent_t));
    /* base 耗尽返回 NULL:与窗口耗尽同为分配器不可用,上抛 */
    return e;
}

static void ext_node_put(wmm_extent_t *e) {
    e->next = g_ext_free_nodes;
    g_ext_free_nodes = e;
}

/* ==================== 地址序双向链 ==================== */

/* 在 pos 之后插入 e(pos==NULL 表示链头)。新 bump 的页尾插,
   分裂出的余段插在原位之后——两种场景都是 O(1),链从不需要
   按地址扫描找插入点。 */
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

/* ==================== 取页/还页核心 ==================== */

/* 窗口内 bump 出 npages 可写页(已持 ext 锁)。
   窗口耗尽 = 撞人为上限,不是机器没内存:dprintf 把两个数都打
   出来供区分;统计事件(0xDB)落地后在此追加。abort_oom 为真时
   快速失败。 */
static void *window_map(size_t npages) {
    size_t bytes = npages * WMM_PAGE;
    if ((char *)g_heap_cur + bytes > (char *)g_heap_end) {
        size_t used = (size_t)((char *)g_heap_cur - (char *)g_heap_base);
        size_t win  = (size_t)((char *)g_heap_end - (char *)g_heap_base);
        wmm_dprintf("OOM: heap window exhausted, %zu MB in use (window %zu MB)\n",
                    used >> 20, win >> 20);
        if (g_opt.abort_oom)
            abort();
        return NULL;
    }

    void *p = mmap(g_heap_cur, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        /* 窗口是自己的 VA,MAP_FIXED 失败基本只剩"内核整体内存
           耗尽"一种解释;同样按 OOM 上抛。 */
        wmm_dprintf("OOM: mmap %zu pages failed, errno %d\n", npages, errno);
        if (g_opt.abort_oom)
            abort();
        return NULL;
    }
    g_heap_cur = (char *)p + bytes;
    return p;
}

/* 取 npages 页的 extent(已持 ext 锁)。first-fit 沿地址序链扫:
   从最低地址起找第一个够大的空闲块,切出低段交付,余段(若够
   一页)留在链上。没有命中才 bump 新页。返回 INUSE 的 extent。
   偏好低地址的副作用正好是期望行为:低地址先被复用、先腾空,
   高地址的空闲块更容易保持大块连续。 */
static wmm_extent_t *extent_get(size_t npages) {
    for (wmm_extent_t *e = g_ar_head; e != NULL; e = e->ar_next) {
        if (e->state == WMM_EXT_INUSE || e->npages < npages)
            continue;

        bool was_dirty = (e->state == WMM_EXT_FREE_DIRTY);
        size_t old_npages = e->npages;
        bool  split_done  = false;

        if (e->npages > npages) {
            /* 分裂:低段给调用方,高段余段留在原链位。余段继承
               原状态(同为 FREE;dirty 计数里它从未离开,无须改)。 */
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
            /* rem 耗尽(元数据枯竭):不分裂,整块交付——多给的页
               无主但记账在 e 里,free 时整体归还,只浪费不损坏。 */
        }

        e->state = WMM_EXT_INUSE;
        e->slab  = NULL;
        if (was_dirty) {
            /* 计数只减"离开空闲态"的页:分裂成功 = 交付段 npages,
               余段仍是 FREE_DIRTY、仍要计数;整块交付(恰合或分裂
               失败)才减 old_npages。两者在恰合时相等。 */
            g_dirty_pages -= split_done ? npages : old_npages;
        }
        return e;
    }

    /* 无可复用:窗口 bump 新页,尾插地址序链 */
    void *addr = window_map(npages);
    if (addr == NULL)
        return NULL;

    wmm_extent_t *e = ext_node_get();
    if (e == NULL)
        return NULL;   /* 页已花掉但不追回:留在 bump 指针之后,
                          下次 bump 会越过它;纯浪费不损坏,极端场景 */

    e->addr   = addr;
    e->npages = npages;
    e->state  = WMM_EXT_INUSE;
    e->slab   = NULL;
    e->next   = NULL;
    ar_insert_after(g_ar_tail, e);
    return e;
}

/* 释放 extent:页表清零、转 FREE_DIRTY、向两侧合并(已持 ext 锁)。
   合并只看"地址相接且都空闲",方向两侧各推到头;被吞的结点壳
   进回收链。dirty 计数页数守恒(空闲段之间腾挪),合并不改它。 */
static void extent_release(wmm_extent_t *e) {
    /* 页表清零:raw=0 即"当前无归属"。物理页留在映射里不动——
       FREE_DIRTY 复用零系统调用,真正还物理页是 purge(M10)的事。 */
    wmm_pte_t zero;
    zero.raw = 0;
    for (size_t pg = 0; pg < e->npages; pg++)
        wmm_pte_set((char *)e->addr + pg * WMM_PAGE, zero);

    e->state = WMM_EXT_FREE_DIRTY;
    e->slab  = NULL;
    g_dirty_pages += e->npages;

    /* 向低地址吞并 */
    wmm_extent_t *prev = e->ar_prev;
    while (prev != NULL && prev->state != WMM_EXT_INUSE &&
           (char *)prev->addr + prev->npages * WMM_PAGE == (char *)e->addr) {
        wmm_extent_t *p2 = prev->ar_prev;
        prev->npages += e->npages;
        ar_unlink(e);
        ext_node_put(e);
        e = prev;
        prev = p2;
    }

    /* 向高地址吞并 */
    wmm_extent_t *next = e->ar_next;
    while (next != NULL && next->state != WMM_EXT_INUSE &&
           (char *)e->addr + e->npages * WMM_PAGE == (char *)next->addr) {
        wmm_extent_t *n2 = next->ar_next;
        e->npages += next->npages;
        ar_unlink(next);
        ext_node_put(next);
        next = n2;
    }
}

/* ==================== 对外接口 ==================== */

wmm_slab_t *wmm_phys_alloc_slab(szind_t i) {
    const wmm_bin_info_t *bi = &wmm_bin_info[i];

    wmm_slab_t *sl = (wmm_slab_t *)wmm_base_alloc(sizeof(wmm_slab_t));
    if (sl == NULL)
        return NULL;

    pthread_mutex_lock(&g_ext_lock);
    wmm_extent_t *e = extent_get(bi->slab_size >> WMM_LG_PAGE);
    pthread_mutex_unlock(&g_ext_lock);
    if (e == NULL)
        return NULL;   /* 头已花掉但不追回:base 只进不出,不构成泄漏 */

    wmm_slab_init(sl, e->addr, i);   /* 注意:init 会把 ext 置 NULL */
    sl->ext = e;                     /* 双向接线:退休时由此找回 extent */
    e->slab = sl;
    return sl;
}

void wmm_phys_free_slab(wmm_slab_t *sl) {
    wmm_extent_t *e = (wmm_extent_t *)sl->ext;
    /* NULL 防御:slab 未走物理层(测试自备页建的),无页可还。
       产品路径不会出现——phys_alloc_slab 必然接线。 */
    if (e == NULL)
        return;

    pthread_mutex_lock(&g_ext_lock);
    extent_release(e);
    pthread_mutex_unlock(&g_ext_lock);
    /* slab 头留存不复用(见文件头);页已在空闲链,可被任何路径复用 */
}

wmm_extent_t *wmm_phys_alloc_extent(size_t size) {
    /* 防回绕挡板:npages 的向上取整在 size 接近 SIZE_MAX 时会溢出
       成小值,把天文数字的请求伪装成小分配。窗口编译期上限以内
       绝不会溢出;挡在上限之外,真把窗口用满的走正常 OOM 路径。 */
    if (size > WMM_HEAP_VA_SIZE) {
        wmm_dprintf("OOM: request %zu MB exceeds heap window\n", size >> 20);
        if (g_opt.abort_oom)
            abort();
        return NULL;
    }

    size_t npages = (size + WMM_PAGE - 1) >> WMM_LG_PAGE;

    pthread_mutex_lock(&g_ext_lock);
    wmm_extent_t *e = extent_get(npages);
    if (e != NULL) {
        /* 页表登记:tag=1 = extent(最低位置 1;结点来自 base、
           8 字节对齐,高位干净)。extent 每一页都登记同一结点,
           free 从任意页进入都能反查到整体。 */
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

void wmm_phys_free_extent(wmm_extent_t *ext) {
    pthread_mutex_lock(&g_ext_lock);
    g_large.live--;
    g_large.live_bytes -= ext->npages * WMM_PAGE;
    g_large.ndalloc++;
    extent_release(ext);   /* 含页表清零 + 两侧合并 */
    pthread_mutex_unlock(&g_ext_lock);
}
