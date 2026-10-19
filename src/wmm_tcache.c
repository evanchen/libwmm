/* wmm_tcache.c —— 线程缓存:无锁快路径 + 批量填充/刷出 + 生命周期
 *
 * 定位:小对象分配/释放的绝大多数在这两条路径上完成——弹栈即分配、
 * 压栈即释放,不碰任何锁(对应 jemalloc tcache.c / cache_bin.c 的
 * 裁剪版;这里用 head 索引栈替代其"低 16 位指针水位"技巧,语义等价,
 * 快路径同样只有一次比较)。bin 锁只在三种批量时刻出现:缓存空时
 * 填充、缓存满时部分刷出、线程退出/收敛核对时全量刷出。
 *
 * 缓存块跟线程同生命周期:首次分配惰性创建,一次 mmap 整块
 * [wmm_tcache_t 结构 | 36 个栈数组],线程退出整块 munmap。
 * 为什么不用 base 区:base 只进不出,线程反复创建销毁会把元数据
 * 吃穿;为什么不用分配器自己来分:首次分配时它还没建好(自举死锁)。
 *
 * 计数纪律(全库不变量 2 的 tcache 侧):缓存中的对象对 bin 而言
 * 仍算存活——填充走 wmm_bin_alloc 逐个 live++,刷出走 wmm_bin_dalloc
 * 逐个 live--,快路径只搬指针不碰计数。于是任意时刻
 *     Σ bin live = 应用持有 + 全部线程缓存
 * 成立;oracle 的对账公式据此从 Σ live 里扣减 Σ 缓存 head。
 *
 * 刷出的锁纪律与 wmm_free 相同:持 bin 锁批量 dalloc,退休的 slab
 * 收集进本地数组,放锁后逐个交物理层——bin 摘除到 munmap 之间不
 * 持 bin 锁,物理层只拿自己的锁。同一批次里一个 slab 只可能在它的
 * 最后一个缓存对象上退休:nfree 要涨到 nregs,要求该 slab 的全部
 * 对象都在空闲链上,自然包括此刻批次里尚未归还的那些;等最后一个
 * 也归还时才退休,之后批次里不再有它的对象。
 */

#define _GNU_SOURCE   /* gettid 的 unistd.h 声明在 __USE_GNU 门下,而
                         Makefile 给的 _DEFAULT_SOURCE 不含它。该宏只
                         影响本编译单元的头文件可见性,不改任何 ABI */

#include "wmm_internal.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

/* ==================== 缓存块构建 ====================
 *
 * 容量公式 clamp(2*nregs, 20, 200):一 slab 装得多的类缓存多些,
 * 批量搬运的摊销更好;下限 20 保证小类一次填充也有几十个的粒度;
 * 上限 200 把单线程缓存的无界膨胀钉死。为什么大对象不缓存,见
 * wmm_internal.h 里 wmm_tcache_t.bins 的注释。
 *
 * 失败处理:整块 mmap 失败就直接返回——调用方落回 M7 的慢路径,
 * 功能不损失,只是这个线程享受不到快路径。不缓存失败结果,下次
 * 分配再试;mmap 失败到这个程度,系统已经在 OOM 边缘,重试的
 * 系统调用开销不是问题。 */

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

    /* 线程局部计数保持显式零值:mmap 本就零填充,写出来是给新增
       字段立规矩——漏赋会被这一行一行看见,而不是埋在零页里 */
    t->t_alloc_bytes = 0;
    t->t_free_bytes  = 0;
    t->t_alloc_cnt   = 0;
    t->t_free_cnt    = 0;

    /* 栈数组紧跟在结构体后。结构体 8 字节对齐且尺寸是 8 的倍数,
       所以每个栈数组都天然满足指针数组的对齐要求 */
    char *stacks = (char *)(t + 1);
    for (szind_t i = 0; i < WMM_NBINS; i++) {
        wmm_cache_bin_t *cb = &t->bins[i];
        cb->stack       = (void **)stacks;
        cb->ncached_max = ncm[i];
        cb->head        = 0;
        cb->low_water   = 0;
        cb->nfill       = (uint16_t)(ncm[i] >> 1);   /* 自适应初值 */
        cb->nrequests   = 0;
        stacks += (size_t)ncm[i] * sizeof(void *);
    }
    /* 大对象槽(下标 36):预留不启用。字段显式清零,免得将来启用
       时继承本次留下的垃圾约定 */
    t->bins[WMM_NBINS].stack       = NULL;
    t->bins[WMM_NBINS].ncached_max = 0;
    t->bins[WMM_NBINS].head        = 0;
    t->bins[WMM_NBINS].low_water   = 0;
    t->bins[WMM_NBINS].nfill       = 0;
    t->bins[WMM_NBINS].nrequests   = 0;

    /* 注册进线程表:统计合并、GC、fork 裁剪都靠这条链找人。
       链的增删持 g_threads_lock;块的其余字段只有本线程碰 */
    pthread_mutex_lock(&g_threads_lock);
    t->next = g_threads;
    if (g_threads != NULL)
        g_threads->prev = t;
    g_threads = t;
    pthread_mutex_unlock(&g_threads_lock);

    /* 发布放在注册之后:fork 若恰好截在两者之间,子进程会把已注册
       但未发布的块当死线程节点清掉(刷空是空操作,munmap 安全),
       不会留下悬空引用 */
    g_tcache = t;

    /* 键值非 NULL 才会在线程退出时触发析构;值本身不用(清理永远
       以 g_tcache 为准,显式 exit 之后它为 NULL,回调空转)。写入
       失败(ENOMEM)意味着本线程的块从此无人自动回收:对象既不在
       应用手里,也回不了 slab,live 账永久悬挂;线程已死之后,应用
       也没有替它调 wmm_thread_exit 的通道。没有降级形态,就地终止。
       哨兵 -1 只可能来自"没先 boot"或内存损坏,同样致命 */
    if (g_tcache_key == (pthread_key_t)-1 ||
        pthread_setspecific(g_tcache_key, t) != 0) {
        wmm_dprintf("fatal: cannot register thread cache of tid %u for "
                    "exit-time reclamation\n", t->tid);
        abort();
    }
    return true;
}

void wmm_thread_init(void) {
    if (g_tcache == NULL)
        (void)tcache_create();
}

void wmm_tcache_thread_destructor(void *val) {
    (void)val;   /* 键值只是"本线程建过缓存"的标记,真身是 g_tcache */
    wmm_thread_exit();
}

/* ==================== 刷出 ====================
 *
 * 从栈底(最冷端)取对象还回 bin:栈顶是最新压入的,大概率马上又被
 * 分配出去(LIFO 局部性),先刷它们等于白刷。n 个对象的 dalloc 在
 * 一次持锁内做完,把锁开销摊到整批上。
 *
 * 逐对象验证页表:正常路径恒通过(缓存里的对象必属本类 slab),失败
 * 只可能来自内存损坏——例如 fork 时另一线程正卡在压栈的中间态
 * (head++ 已发生、栈项未写),子进程拿到的半个栈项。坏项丢了计数
 * 就永久多 1,但总比把野指针挂进空闲链强。 */

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

    /* 退休 slab 在 bin 锁外交物理层:与 wmm_free 同一锁纪律。
       同批次的退休顺序已在文件头论证,不会出现"退休后再 dalloc
       同一 slab"的窗口 */
    for (unsigned k = 0; k < nret; k++)
        wmm_phys_free_slab(retired[k]);

    memmove(cb->stack, cb->stack + n,
            (size_t)(cb->head - n) * sizeof(void *));
    cb->head = (uint16_t)(cb->head - n);
    cb->low_water = cb->head;   /* 水位随刷出重置:之后的 GC 只该统计
                                   刷出以来真正被用掉的部分 */
}

void wmm_tcache_flush(szind_t i, unsigned n) {
    if (g_tcache == NULL)
        return;
    tcache_flush_bins(g_tcache, i, n);
}

void wmm_tcache_flush_all(void) {
    if (g_tcache == NULL)
        return;
    for (szind_t i = 0; i < WMM_NBINS; i++)
        if (g_tcache->bins[i].head > 0)
            tcache_flush_bins(g_tcache, i, g_tcache->bins[i].head);
}

/* ==================== 快路径 ====================
 *
 * 两个函数只被本线程调用,零锁零原子:head 是唯一游标,栈数组是
 * 本线程独占的普通内存。编译器视角它们就是两条 load/store。 */

void *wmm_tcache_alloc(szind_t i) {
    wmm_cache_bin_t *cb = &g_tcache->bins[i];

    if (cb->head == 0)
        return NULL;
    void *p = cb->stack[--cb->head];
    if (cb->head < cb->low_water)
        cb->low_water = cb->head;   /* 记下这段时间水位最低漂到哪 */
    return p;
}

void wmm_tcache_free(szind_t i, void *p) {
    wmm_cache_bin_t *cb = &g_tcache->bins[i];

    /* 满了先按 3/4 比例部分刷出(留 1/4 在栈上),再压新的对象。
       全刷会让下一次分配立刻回慢路径;留四分之一保住了热对象的
       LIFO 局部性,又给后续释放腾出整批的余量 */
    if (cb->head == cb->ncached_max)
        tcache_flush_bins(g_tcache, i,
                          (unsigned)(cb->ncached_max - cb->ncached_max / 4));
    cb->stack[cb->head++] = p;
}

/* ==================== 填充 ====================
 *
 * 只在缓存空时调用:一次向 arena 要 want 个,wmm_bin_fill 内部持
 * 一次 bin 锁搬完。为什么这比"分配一个、拿走、再慢慢补"好:
 * 补充路径的锁开销被 want 个请求摊薄,快路径才能继续无锁。
 * 填充后 low_water 抬到新水位——刚填进来的对象还没被用到,
 * 不该算进"这段时间用掉多少"的 GC 估计里。 */

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

/* ==================== 线程退出与注册表 ==================== */

void wmm_thread_exit(void) {
    wmm_tcache_t *t = g_tcache;

    if (t == NULL)
        return;

    /* 先把对象还给 bin,再拆壳:顺序反了就是在已 munmap 的块上刷栈 */
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
    (void)munmap(t, t->map_size);   /* 失败无法补救:块已从一切结构摘除 */
}

uint64_t wmm_tcache_cached_total(void) {
    uint64_t sum = 0;

    /* 锁保护的是注册表链;heads 是各线程的无锁写,严格说这是一次
       快照——只在全体线程静止的时刻(屏障汇合、单线程测试)调用,
       读数才是精确值。统计快照将来也按这个口径使用 */
    pthread_mutex_lock(&g_threads_lock);
    for (wmm_tcache_t *t = g_threads; t != NULL; t = t->next)
        for (szind_t i = 0; i < WMM_NBINS; i++)
            sum += t->bins[i].head;
    pthread_mutex_unlock(&g_threads_lock);
    return sum;
}

/* ==================== fork 子进程侧清理 ====================
 *
 * fork 只把调用线程复制进子进程,注册表里其他线程的节点成了死节点:
 * 它们缓存的对象若不一并归还,永远达不到"全空退休",子进程的 live
 * 计数就带着一笔无法回收的账。处理:死节点的缓存逐类刷回 bin,然后
 * 整块 munmap;调用线程自己的块原样保留。
 *
 * 为什么敢碰死线程的栈:prepare 钩子持住全部锁,快照里 bin 世界是
 * 静止的;但快路径无锁,pthread 无法被钩子冻结,快照可能截到半个
 * 栈项。tcache_flush_bins 的逐项页表验证正是为这个窗口准备的——
 * 好项归还,坏项丢弃(计数留一笔小泄漏,好过崩溃)。
 *
 * 调用时机:wmm_atfork_child 里、所有锁重建之后。子进程此刻单线程,
 * 独占执行,不存在并发。 */

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
            for (szind_t i = 0; i < WMM_NBINS; i++)
                if (t->bins[i].head > 0)
                    tcache_flush_bins(t, i, t->bins[i].head);
            (void)munmap(t, t->map_size);
        }
        t = next;
    }

    if (keep != NULL && !found) {
        /* 理论上不可达:发布发生在注册之后,fork 若截在两者之间,
           子进程看到的 g_tcache 是 NULL。真到了这里说明链表被外部
           动过,块本身仍完好,补回成唯一节点,避免 g_tcache 悬空 */
        keep->prev = NULL;
        keep->next = NULL;
        g_threads = keep;
    }
}
