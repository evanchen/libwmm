/* wmm_phys.c —— 物理层:页从哪来、还给谁
 *
 * 当前是最小集(对应 jemalloc extent 子系统的骨架),主路径的
 * 交接单位:slab(小对象)与 extent(大对象)。
 *   取页:从 VA 窗口 bump 前进(g_heap_cur),MAP_FIXED 把 PROT_NONE
 *         预留改成可写页。没有空闲链、没有合并——地址只向前推进。
 *   还页:mmap(PROT_NONE|MAP_FIXED|MAP_NORESERVE) 覆盖原区间,等价
 *         munmap 再重新预留:物理页立即归还内核,VA 仍归窗口管。
 *         页表项同步清零(raw==0),此后对这些地址的任何释放都会
 *         被反查识别为"未分配"——这就是双释放检测在物理层的落点。
 *
 * M6 在此升级:空闲 extent 挂地址序链、相邻区间合并、攒到
 * dirty_max 阈值再批量 madvise——"立即还"换成"攒着批量还",
 * 减少反复缺页的抖动。本文件的对外接口签名不变,主路径无感。
 *
 * 锁:g_ext_lock 保护 g_heap_cur(bump)、结点回收链与 g_large 统计;
 * 结点切 base 走 base 锁(叶子锁)。锁序 bin 锁 → g_ext_lock →
 * (base 锁),与 fork prepare 的加锁顺序一致,无互锁。
 * free 侧的退休发生在 bin 锁释放之后,进本层只拿 ext 锁一把。
 *
 * extent 结点复用、slab 头不复用的理由:大对象生灭频繁,结点
 * (24 字节)逐块新拿会把 base 区吃穿,所以走回收链;slab 的生命
 * 通常与该类流量同长,退休时页已整块还给内核,72 字节的头部留存
 * 不构成泄漏主项——M6 若统计显示不然,同款回收链照加即可。
 */

#include "wmm_internal.h"

#include <stdlib.h>
#include <sys/mman.h>

static wmm_extent_t *g_ext_free_nodes;   /* 空闲 extent 结点回收链 */

/* 窗口内取 npages 页(可写)。窗口耗尽 = OOM:这是"撞人为上限",
 * 不是"机器没内存",dprintf 把两个数都打出来,运维一眼可分;
 * 统计事件(0xDB)落地后在此追加。abort_oom 配置为真时快速失败。 */
static void *window_map(size_t npages) {
    pthread_mutex_lock(&g_ext_lock);

    size_t bytes = npages * WMM_PAGE;
    if ((char *)g_heap_cur + bytes > (char *)g_heap_end) {
        size_t used = (size_t)((char *)g_heap_cur - (char *)g_heap_base);
        size_t win  = (size_t)((char *)g_heap_end - (char *)g_heap_base);
        pthread_mutex_unlock(&g_ext_lock);
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
        pthread_mutex_unlock(&g_ext_lock);
        wmm_dprintf("OOM: mmap %zu pages failed, errno %d\n", npages, errno);
        if (g_opt.abort_oom)
            abort();
        return NULL;
    }
    g_heap_cur = (char *)p + bytes;

    pthread_mutex_unlock(&g_ext_lock);
    return p;
}

/* 还页:覆盖回 PROT_NONE 预留 + 页表清零。
 * 页表清零必须做:不清零的话,这块地址上的登记还指向已归还的
 * slab/extent,将来任何误释放都会被当成合法指针走下去。
 * mmap 失败(内核崩溃边缘)时保守放弃:此时映射未变、登记与
 * 映射仍一致,只是物理页没还成。 */
static void window_unmap(void *addr, size_t npages) {
    size_t bytes = npages * WMM_PAGE;
    void *m = mmap(addr, bytes, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
                   -1, 0);
    if (m == MAP_FAILED) {
        wmm_dprintf("warning: unmap %p (%zu pages) failed, errno %d\n",
                    addr, npages, errno);
        return;
    }

    wmm_pte_t zero;
    zero.raw = 0;
    for (size_t pg = 0; pg < npages; pg++)
        wmm_pte_set((char *)addr + pg * WMM_PAGE, zero);
}

/* extent 结点:优先回收链,空了再切 base */
static wmm_extent_t *ext_node_get(void) {
    pthread_mutex_lock(&g_ext_lock);
    wmm_extent_t *e = g_ext_free_nodes;
    if (e != NULL)
        g_ext_free_nodes = e->next;
    pthread_mutex_unlock(&g_ext_lock);

    if (e == NULL)
        e = (wmm_extent_t *)wmm_base_alloc(sizeof(wmm_extent_t));
    /* base 耗尽返回 NULL:与窗口耗尽同为分配器不可用,上抛 */
    return e;
}

static void ext_node_put(wmm_extent_t *e) {
    pthread_mutex_lock(&g_ext_lock);
    e->next = g_ext_free_nodes;
    g_ext_free_nodes = e;
    pthread_mutex_unlock(&g_ext_lock);
}

/* ==================== 对外接口 ==================== */

wmm_slab_t *wmm_phys_alloc_slab(szind_t i) {
    const wmm_bin_info_t *bi = &wmm_bin_info[i];

    wmm_slab_t *sl = (wmm_slab_t *)wmm_base_alloc(sizeof(wmm_slab_t));
    if (sl == NULL)
        return NULL;

    void *addr = window_map(bi->slab_size >> WMM_LG_PAGE);
    if (addr == NULL)
        return NULL;   /* 头已花掉但不追回:base 只进不出,不构成泄漏 */

    wmm_slab_init(sl, addr, i);
    return sl;
}

void wmm_phys_free_slab(wmm_slab_t *sl) {
    const wmm_bin_info_t *bi = &wmm_bin_info[sl->szind];
    window_unmap(sl->addr, bi->slab_size >> WMM_LG_PAGE);
    /* slab 头留存不复用(见文件头);M6 给 slab 挂 extent 后,此处
       顺带把 extent 结点交还回收链。 */
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

    wmm_extent_t *ext = ext_node_get();
    if (ext == NULL)
        return NULL;

    void *addr = window_map(npages);
    if (addr == NULL) {
        ext_node_put(ext);
        return NULL;
    }

    ext->addr   = addr;
    ext->npages = npages;

    /* 页表登记:tag=1 = extent(最低位置 1;extent 结点来自 base、
       8 字节对齐,高位干净) */
    wmm_pte_t e;
    e.raw = (uintptr_t)ext | 1u;
    for (size_t pg = 0; pg < npages; pg++)
        wmm_pte_set((char *)addr + pg * WMM_PAGE, e);

    pthread_mutex_lock(&g_ext_lock);
    g_large.live++;
    g_large.live_bytes += npages * WMM_PAGE;
    pthread_mutex_unlock(&g_ext_lock);
    return ext;
}

void wmm_phys_free_extent(wmm_extent_t *ext) {
    window_unmap(ext->addr, ext->npages);

    pthread_mutex_lock(&g_ext_lock);
    g_large.live--;
    g_large.live_bytes -= ext->npages * WMM_PAGE;
    g_large.ndalloc++;
    pthread_mutex_unlock(&g_ext_lock);

    ext_node_put(ext);
}
