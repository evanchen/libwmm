/* wmm_core.c —— 分配主路径
 *
 * 职责:slab 的建立与对象取/还(空闲链);bin 状态机(热 slab 满了
 * 挂链、全空归还);非满 slab 的地址序链;页表登记与反查。
 *
 * 层次关系:本文件只管"对象 ←→ slab/bin"的世界;页从哪来、还给谁、
 * 相邻块怎么合并是 wmm_phys.c 的事,两者以 slab(及其承载页)为交接
 * 单位。本文件不碰 g_ext_lock——所属 bin 的锁就是这里的全部同步。
 *
 * 并发纪律(全库不变量 2"计数单点"):bin 的 live 计数只在持该 bin 锁
 * 的路径里增减,因此"Σ live[i]*size[i] == live_bytes"在任何观测时刻
 * 精确成立;tcache 的批量填充/刷出路径今后也必须遵守同一条纪律。
 */

#include "wmm_internal.h"

#include <assert.h>

/* ==================== 页表辅助 ====================
 *
 * 页表是"地址 → 归属"的唯一桥梁。登记与反查全部经这四个函数走,
 * 不把 g_pte 数组访问散落各处,便于统一加断言与统计。
 *
 * 下标计算是"相对窗口基址"的页号:p 必须落在 [g_heap_base,
 * g_heap_end) 内——free 路径进来之前先判范围,这里的下标计算无条件
 * 信任这一点,越界指针会读写页表以外的内存。 */

wmm_pte_t wmm_pte_get(const void *p) {
    return g_pte[((uintptr_t)p - (uintptr_t)g_heap_base) >> WMM_LG_PAGE];
}

void wmm_pte_set(const void *p, wmm_pte_t v) {
    g_pte[((uintptr_t)p - (uintptr_t)g_heap_base) >> WMM_LG_PAGE] = v;
}

/* tag=0 且非零 → slab 指针;raw==0(未分配)与 tag=1 都返回假。 */
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

/* ==================== slab:把连续页初始化成等大对象 ====================
 *
 * 对象不带任何头部:空闲对象的首 4 字节复用为侵入式空闲链(存下一个
 * 空闲对象的编号),元数据集中在 slab 头(base 区分配,不占堆页)。
 * 编号与地址一次除法互推:regind = (p − addr)/reg_size。
 */

void wmm_slab_init(wmm_slab_t *sl, void *addr, szind_t i) {
    const wmm_bin_info_t *bi = &wmm_bin_info[i];

    /* 全字段显式赋值(等效 memset 后逐个覆盖,但不依赖"先清零"的
       记忆——新增字段时漏赋会被这里一眼看出)。 */
    sl->szind     = i;
    sl->nfree     = bi->nregs;
    sl->free_head = 0;
    sl->pad       = 0;
    sl->addr      = addr;
    sl->full_prev = NULL;
    sl->full_next = NULL;
    sl->ph_prev   = NULL;
    sl->ph_next   = NULL;
    sl->ph_child  = NULL;
    sl->ext       = NULL;   /* 物理层交接 extent 时填上 */

    /* 建空闲链:对象 r 的首 4 字节写 r+1,链尾对象写 UINT32_MAX。
       于是链为 0→1→2→…,分配按地址升序出对象,首批分配地址连续。
       这一步逐对象写入,是 slab 生命周期唯一一次全量触碰——页的
       物理内存在建链时全部驻留,不存在分配时二次缺页。 */
    for (uint32_t r = 0; r + 1 < bi->nregs; r++)
        *(uint32_t *)((char *)addr + (size_t)r * bi->reg_size) = r + 1;
    *(uint32_t *)((char *)addr + (size_t)(bi->nregs - 1) * bi->reg_size)
        = UINT32_MAX;

    /* 页表登记:slab 的每一页写同一个表项(tag=0 → slab 头;
       slab 头 8 字节对齐,最低位天然为 0,无须显式置 tag)。
       free 反查任意对象所在页都能拿到同一归属。 */
    wmm_pte_t e;
    e.raw = (uintptr_t)sl;
    size_t npages = bi->slab_size >> WMM_LG_PAGE;
    for (size_t pg = 0; pg < npages; pg++)
        wmm_pte_set((char *)addr + pg * WMM_PAGE, e);
}

void *wmm_slab_alloc(wmm_slab_t *sl) {
    const wmm_bin_info_t *bi = &wmm_bin_info[sl->szind];

    assert(sl->nfree > 0);   /* 内部契约:调用方状态机先判,违反即 bug */

    uint32_t r    = sl->free_head;
    char    *p    = (char *)sl->addr + (size_t)r * bi->reg_size;
    sl->free_head = *(uint32_t *)p;   /* 侵入式链:下一编号就在对象首字里 */
    sl->nfree--;
    return p;
}

/* 前提:p 属于本 slab 且当前不空闲。双重释放会破坏链——检测靠
   guard 模块的 poison/隔离,不在这条路径上花检查。 */
void wmm_slab_free(wmm_slab_t *sl, void *p) {
    const wmm_bin_info_t *bi = &wmm_bin_info[sl->szind];

    uint32_t r = (uint32_t)(((uintptr_t)p - (uintptr_t)sl->addr) / bi->reg_size);
    *(uint32_t *)p = sl->free_head;
    sl->free_head  = r;
    sl->nfree++;
}

/* ==================== 非满 slab 的地址序链 ====================
 *
 * 结构:ph_prev/ph_next 维护地址升序双向链,bin 的 nonfull_root 指向
 * 最低地址。分配永远取链头(最低地址):同批对象地址聚集,cache 局部
 * 性好;低地址 slab 更容易先腾空整块归还。
 * 插入是 O(n) 的顺序找位(n = 本类非满 slab 数,通常个位数);若压测
 * 证明它成为瓶颈,升级为 jemalloc edata_heap 式配对堆——slab 结构的
 * ph_child 字段已为此预留,不必改布局。 */

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

/* ==================== 满链(双向,O(1) 摘除) ==================== */

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

/* ==================== bin 状态机 ====================
 *
 * 一个 slab 的全部去向:slabcur(热位)/ full 链 / nonfull 链 / 物理层
 * (全空归还)。状态转移只发生在这两个函数里,且全程持 bin 锁。
 *
 * 关键事实(下面 dalloc 的分支判据全部由它推出):
 *   热 slab 即使 nfree==0 也仍占着 slabcur,直到下一次 bin_alloc 把
 *   它挂进 full(惰性退役)——所以"nfree==0"不能区分 full 链成员与
 *   热位;nonfull 成员的 nfree 恒 ≥1(到 0 之前必已转正为热 slab)。
 * nregs==1 的类(4096/8192/12288)分配即满、释放即空,天然永不进
 * nonfull 链,jemalloc 的 bin.c 同样不为它们写插入分支。 */

void wmm_bin_slab_install(wmm_bin_t *b, wmm_slab_t *sl) {
    /* 前提契约:bin_alloc 返回 NULL 时旧热 slab 已挂 full、nonfull 已
       取空,此刻 slabcur 必为 NULL;若不为空说明调用序错了,新 slab
       会顶掉一个仍在服役的旧 slab、把它从所有结构里泄漏掉。 */
    assert(b->slabcur == NULL);

    b->slabcur   = sl;
    b->nslabs++;
    b->curslabs++;
}

void *wmm_bin_alloc(wmm_bin_t *b) {
    /* 1) 热 slab 还有空位:直接弹 */
    if (b->slabcur != NULL && b->slabcur->nfree > 0) {
        b->live++;
        return wmm_slab_alloc(b->slabcur);
    }

    /* 2) 热 slab 满了:此刻才退役挂 full。它不再服务分配,直到某次
       释放让它由满转非满。 */
    if (b->slabcur != NULL) {
        bin_push_full(b, b->slabcur);
        b->slabcur = NULL;
    }

    /* 3) nonfull 链头(最低地址)转正为热 slab */
    if (b->nonfull_root != NULL) {
        wmm_slab_t *sl = b->nonfull_root;
        nonfull_remove(&b->nonfull_root, sl);
        b->slabcur = sl;
        b->live++;
        return wmm_slab_alloc(sl);
    }

    /* 4) 无可用 slab:调用方去物理层取页,init + install 后重试 */
    return NULL;
}

bool wmm_bin_dalloc(wmm_bin_t *b, wmm_slab_t *sl, void *p) {
    const wmm_bin_info_t *bi = &wmm_bin_info[sl->szind];

    wmm_slab_free(sl, p);
    b->live--;

    /* ---- 分支一:热位。怎么释放都留在热位,唯一例外是全空退休。
       注意不能按 nfree 推断它挂在哪——热 slab 满了(nfree==0)也仍在
       热位,若误走"由满转非满"会去 full 链摘一个不在链上的节点,
       直接清掉整条满链。 */
    if (sl == b->slabcur) {
        if (sl->nfree == bi->nregs) {
            b->slabcur = NULL;
            b->curslabs--;
            return true;
        }
        return false;
    }

    /* ---- 分支二:全空退休。来源由 nregs 唯一确定:nregs==1 的类
       上一状态必是 full(它从不进 nonfull,且释放一次即全空);
       其余类 nonfull 成员释放前 nfree≥1,能涨到 nregs 的只可能在
       nonfull 链上。 */
    if (sl->nfree == bi->nregs) {
        if (bi->nregs == 1)
            bin_unlink_full(b, sl);
        else
            nonfull_remove(&b->nonfull_root, sl);
        b->curslabs--;
        return true;
    }

    /* ---- 分支三:由满转非满。判据 nfree==1 无歧义——非 slabcur 的
       slab 释放前若在 full 链则 nfree==0,释放后恰为 1;若已在
       nonfull 链,释放前≥1、释放后≥2,到不了 1。 */
    if (sl->nfree == 1) {
        bin_unlink_full(b, sl);
        nonfull_insert(&b->nonfull_root, sl);
    }

    /* nfree>1 且非热位:已在 nonfull 链,原地变空一些,无需动作 */
    return false;
}

/* 对象编号除法:regind = (p − base)/reg_size。
 * 独立成函数便于将来集中换成 jemalloc 同款的魔术数乘法(预计算
 * magic/shift,除法变乘移,省 ~20 周期);普通除法只在慢路径,不急。 */
uint32_t wmm_div_regind(const void *p, const void *base, uint32_t reg_size) {
    return (uint32_t)(((uintptr_t)p - (uintptr_t)base) / reg_size);
}
