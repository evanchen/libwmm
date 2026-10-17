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
#include <string.h>

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
    b->ndalloc++;   /* 累计释放次数:一切归还对象的路径都经此,单点计数 */

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

/* ==================== 主路径:wmm_alloc 家族 ====================
 *
 * 当前是单线程、无 tcache 的版本。tcache 落地后,这里变成
 * "缓存耗尽/刷出"才走的慢路径,快路径在 wmm_tcache.c;本段
 * 结构不变:查类 → bin 锁 → bin_alloc,NULL 则物理层供 slab
 * 后重试(至多一次)。
 *
 * 大小对象在此分野:小对象走 bin(共享 slab),大对象走物理层
 * (整页 extent)。分界线 14336/16384 之间的缝隙(14337..16383)
 * 也走大对象路径——请求超过任何类的规格时,近似到页比近似到
 * 7 页 slab 更省(见 wmm_internal.h 的 WMM_SMALL_MAXCLASS 注释)。 */

void *wmm_alloc(size_t size) {
    if (size == 0)
        size = 1;   /* glibc 语义:size==0 当 1 处理,返回可释放指针 */

    if (size > WMM_SMALL_MAXCLASS) {
        wmm_extent_t *ext = wmm_phys_alloc_extent(size);
        return ext != NULL ? ext->addr : NULL;
    }

    szind_t     i = wmm_size2index(size);
    wmm_bin_t *b = &g_bins[i];

    /* 慢路径全程持 bin 锁,包括物理层取页(mmap 在锁内)。v1 用
       简单换正确:两个线程同时发现无 slab,不会各自造一个(那需要
       "放锁取页、装回时发现别人已装"的让路逻辑)。代价是造 slab
       期间本类其他分配排队;造 slab 每类低频,tcache 落地后更稀。 */
    pthread_mutex_lock(&b->lock);
    void *p = wmm_bin_alloc(b);
    if (p == NULL) {
        wmm_slab_t *sl = wmm_phys_alloc_slab(i);
        if (sl != NULL) {
            wmm_bin_slab_install(b, sl);
            p = wmm_bin_alloc(b);   /* 必成功:全空 slab 至少弹出一个 */
        }
    }
    pthread_mutex_unlock(&b->lock);
    return p;
}

void wmm_free(void *p) {
    if (p == NULL)
        return;

    /* 先验界再查表:窗口外的指针不属本分配器。混用 libc 分配器在
       契约上是 UB,但 UB 不等于活该崩溃——给出诊断信息再返回,
       服务还能带病跑完这一单(guard 检测落地后这里改写 0xBE 事件)。 */
    if ((uintptr_t)p < (uintptr_t)g_heap_base ||
        (uintptr_t)p >= (uintptr_t)g_heap_end) {
        wmm_dprintf("free: %p out of heap window (not from wmm?)\n", p);
        return;
    }

    wmm_slab_t *sl;
    void       *ext;

    if (wmm_pte_is_slab(p, &sl)) {
        wmm_bin_t *b = &g_bins[sl->szind];
        pthread_mutex_lock(&b->lock);
        bool retire = wmm_bin_dalloc(b, sl, p);
        pthread_mutex_unlock(&b->lock);

        /* 退休在 bin 锁外执行:物理层只拿自己的锁,不与 bin 锁
           嵌套。当前单线程无虞;多线程时代"bin 摘除 → munmap"之间
           的窗口由 tcache 刷出路径的锁纪律闭合(tcache 落地时处理)。 */
        if (retire)
            wmm_phys_free_slab(sl);
        return;
    }

    if (wmm_pte_is_extent(p, &ext)) {
        wmm_phys_free_extent((wmm_extent_t *)ext);
        return;
    }

    /* raw==0:从未分配(或整块已退休)的地址——双释放/野指针。
       什么也不做地返回:M14 起这里写 0xBE 事件并可能按配置隔离,
       现在 dprintf 至少把问题打在现场。注意顺序:反查先于任何
       对 p 的解引用,被回收过的页是 PROT_NONE,碰了就是段错误。 */
    wmm_dprintf("free: %p not allocated (double free or wild pointer?)\n", p);
}

void *wmm_calloc(size_t n, size_t size) {
    /* 乘法溢出即失败:glibc 同款语义,回绕出来的大小毫无意义 */
    if (size != 0 && n > SIZE_MAX / size)
        return NULL;

    size_t bytes = n * size;
    void *p = wmm_alloc(bytes);
    if (p == NULL)
        return NULL;

    /* 为什么无条件 memset:新 slab 的页来自内核零页天然是零,但从
       空闲链弹出的复用对象带着上一任的全部内容,不 memset 就是
       信息泄漏。jemalloc 有"fresh slab 免清"的优化,代价是逐 slab
       追踪"是否全新"的状态;小而精先要 correctness。 */
    memset(p, 0, bytes);
    return p;
}

void *wmm_realloc(void *p, size_t size) {
    if (p == NULL)
        return wmm_alloc(size);
    if (size == 0) {
        wmm_free(p);
        return NULL;
    }

    size_t usable = wmm_usable_size(p);
    if (size <= usable)
        return p;   /* 原地:小对象本无尾巴可割;大对象不割页尾
                       (jemalloc 的 extent split 属 M6,单独做收益小) */

    void *q = wmm_alloc(size);
    if (q == NULL)
        return NULL;   /* 旧块保持有效:realloc 失败不丢数据 */

    memcpy(q, p, usable < size ? usable : size);   /* 契约:min(旧可用, 新) */
    wmm_free(p);
    return q;
}

size_t wmm_usable_size(const void *p) {
    if (p == NULL)
        return 0;
    if ((uintptr_t)p < (uintptr_t)g_heap_base ||
        (uintptr_t)p >= (uintptr_t)g_heap_end)
        return 0;   /* 不属本分配器:契约外输入,给 0 不给崩溃 */

    wmm_slab_t *sl;
    void       *ext;
    if (wmm_pte_is_slab(p, &sl))
        return wmm_bin_info[sl->szind].reg_size;
    if (wmm_pte_is_extent(p, &ext)) {
        wmm_extent_t *e = (wmm_extent_t *)ext;
        /* 减去头部零头:对齐分配的 p 不在 extent 基址上,可用大小 =
           映射末尾 − p(普通大对象 p 即基址,零头为 0,同一公式)。
           不减会把头部零头虚报进去,realloc 的原地扩容分支就会
           让应用写穿映射末端。 */
        return (size_t)((char *)e->addr + e->npages * WMM_PAGE - (char *)p);
    }
    return 0;
}

/* ==================== 对齐分配 ====================
 *
 * 三档(指南的"免费对齐"思想:能白拿的对齐绝不付过分配的代价):
 *   档 1 alignment ≤ 8:白拿。类 0 的对象 8B 对齐、类 1 起全部
 *       16B 对齐,任何 ≤ 8 的 2 的幂天然满足。
 *       边界为什么是 8 不是 16:alignment=16、size≤8 时若直接
 *       wmm_alloc(size) 会落进 8B 类,奇数号对象只有 8B 对齐,
 *       违反契约——16 这档必须走档 2 取整到 16B 类;
 *   档 2 8 < alignment ≤ 4096(页大小):取整白拿。size 向上取整到
 *       alignment 的倍数再走普通分配:
 *       小对象路径的依据:类规格形如 2^p×m(m 为奇数,最大 7),
 *       故不是 alignment(=2^k)倍数的类最大为 7×2^(k−1)=3.5×2^k;
 *       而 2^k、2·2^k、3·2^k 三者本身都是类(组内成员恰有此形状,
 *       16..8192 的全体 2 的幂亦在表)。于是 alignment 的倍数 x 的
 *       最小上界类:x ≤ 3.5×2^k 时 x 只能是上述三者之一,上界即
 *       自身;x 更大时一切非倍数类都 ≤ 3.5×2^k < x,上界类必为
 *       alignment 的倍数。slab 基址页对齐(4096 是任何 ≤4096 的
 *       2 的幂的倍数),对象地址 = 基址 + 编号×规格 → 对齐成立;
 *       大对象路径(size 取整后 > 14336):返回页对齐地址,页对齐
 *       ⊇ 任何 ≤4096 的 2 的幂对齐。且 alignment 整除页大小,先取
 *       整到 alignment 再取整到页,页数与不取整时相同——零浪费;
 *   档 3 alignment > 4096:唯一真付代价的档——过分配
 *       size+alignment 字节,返回地址向上对齐。头部零头(基址到
 *       对齐点)与页尾零头一并浪费,上限约 2×alignment;free 与
 *       usable_size 走页表反查 extent(usable 已扣头部零头,见上),
 *       与普通大对象同一条路,无须特殊记账。
 */

/* 对齐分配核心。调用方负责校验 alignment 是 2 的幂(两个公开 API
   的校验条款不同:posix_memalign 额外要求 ≥ sizeof(void*))。 */
static void *alloc_aligned(size_t alignment, size_t size) {
    if (alignment <= 8)
        return wmm_alloc(size);            /* 档 1:8B 对齐是全库下限 */

    if (alignment <= WMM_PAGE) {           /* 档 2:取整白拿 */
        size_t rounded = (size + alignment - 1) & ~(alignment - 1);
        return wmm_alloc(rounded);
    }

    /* 档 3:过分配 + 基址对齐。加法先挡回绕——size 接近 SIZE_MAX 时
       size+alignment 绕回小值会骗过页数取整;alignment 超过整个
       VA 窗口的也没得谈。两者都按 OOM 上抛。 */
    if (size > SIZE_MAX - alignment || alignment > WMM_HEAP_VA_SIZE)
        return NULL;

    wmm_extent_t *ext = wmm_phys_alloc_extent(size + alignment);
    if (ext == NULL)
        return NULL;

    uintptr_t base = (uintptr_t)ext->addr;
    return (void *)((base + alignment - 1) & ~(uintptr_t)(alignment - 1));
}

int wmm_posix_memalign(void **out, size_t alignment, size_t size) {
    if (out == NULL)
        return EINVAL;   /* 输出参数本身为空:调用方错误,无值可写 */
    *out = NULL;         /* 失败路径统一置空:调用方无须先初始化 */

    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
        return EINVAL;

    void *p = alloc_aligned(alignment, size);
    if (p == NULL)
        return ENOMEM;
    *out = p;
    return 0;
}

void *wmm_aligned_alloc(size_t alignment, size_t size) {
    /* alignment==0 要单列:0 & (0−1) == 0,幂检查对它失效 */
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
