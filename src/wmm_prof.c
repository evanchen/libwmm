/* wmm_prof.c —— 调用点采样
 *
 * 回答"内存是谁吃的":分配路径按间隔采样,沿帧指针链回溯用户调用栈,
 * 记进线程自己的环形缓冲;后台线程把样本按"前 4 帧折叠成的调用点键"
 * 合并成估算的字节数与次数,经 wmm_ctl("prof.top") 与共享监控区输出
 * TOP N。默认关闭,关闭时分配路径只多一次全局标志测试。
 *
 * 为什么采样而不是逐次记账:逐次记录调用栈要把每一次分配都变成一次
 * 栈遍历,热路径直接慢一个数量级。按间隔 N 抽样、再按 N 放大,是
 * Horvitz-Thompson 式的无偏估计(jemalloc 的 prof 同一思路):采到的
 * 每个样本代表约 N 次同调用点分配,输出的是估计值。误差随样本数
 * 收敛,千级样本已足够回答"谁在吃内存"。
 *
 * 为什么触发点覆盖全部成功分配而不是只挂 bin 慢路径:估计器无偏的
 * 前提是"对全体分配均匀抽样"。只抽慢路径会系统性低估缓存命中的热
 * 调用点(热调用点恰恰最值得看),也完全漏掉大对象——大对象根本不
 * 经过 bin,却是 OBS2 里最典型的"吃内存函数"。所以三个成功返回点
 * (大对象、缓存命中、bin)共用同一个采样入口;关闭时各点只多一条
 * 可预测的全局标志测试。字面上的"零指令"要运行期改写代码,代价与
 * 收益不成比例,jemalloc 同样保留这个测试。
 *
 * 应用侧前提:本库两套构建都强制 -fno-omit-frame-pointer,且
 * wmm_alloc 在入口主动取自己的帧地址,强制编译器为它建立规范的
 * 帧指针链,所以第一帧(直接调用点)恒准确;更深的帧要应用也带
 * 帧指针编译,否则链可能提前断开或指向垃圾。遍历带单调性与跨度
 * 检查,坏链只会让样本少几帧,不会把它当有效调用链收下——与
 * jemalloc 的帧指针回溯同一前提。
 *
 * 同步:写端只有被采样线程自己;每个槽一把微型 seqlock 供后台合并器
 * 并发读取。聚合表由 g_prof_lock 保护,它是叶子锁(见 wmm_internal.h
 * 的锁序说明)。
 */

#include "wmm_internal.h"

#include <stdatomic.h>
#include <string.h>

/* ==================== 栈遍历 ==================== */

/* 单步允许的最大帧跨度。正常栈帧相隔几十到几百字节;16MB 远大于
   任何线程栈上的两帧间距,又足以把野帧指针跳到堆/映射区的多数情形
   挡在解引用之前。 */
#define WMM_PROF_MAX_FRAME_BYTES ((uintptr_t)16 << 20)

/* 从分配函数的帧开始向上走帧指针链,把 return address 依次写入 buf。
 * frame 必须是 wmm_alloc 自己的帧地址:由它在函数入口用
 * __builtin_frame_address(0) 捕获后经采样入口传进来。
 *
 * 为什么由分配函数捕获、而不是在采样器或本函数里取:
 * __builtin_frame_address(0) 描述的是求值它的那个函数。本函数在
 * debug 构建(-O0)里是独立函数,在它里面取只会拿到它自己的帧。在
 * 采样器里取也不够:它的 push rbp 保存的是 wmm_alloc 的 rbp,而
 * 只要 wmm_alloc 自己没取过帧地址,编译器就可能把 rbp 挪作通用
 * 寄存器,保存下来的并不是帧指针——跳过采样器帧后照样读到垃圾。
 * 真正需要规范帧指针链的是 wmm_alloc,所以帧地址必须由它取,再由
 * 它显式传进来。取帧地址这一事实会强制编译器为 wmm_alloc 建立
 * 规范的帧指针链——只给 -fno-omit-frame-pointer 不够,-O2 或 ASan
 * 构建仍可能把 rbp 挪作通用寄存器。
 *
 * 为什么从这一帧开始收:fp[1] 就是 wmm_alloc 的 return address,即
 * 用户对分配 API 的调用点;fp[0] 是用户的帧指针,继续走得到用户的
 * 调用者,与 jemalloc 的 prof 语义一致。若把分配器的帧也排掉,TOP
 * 表会把所有分配归到内部的同一个地址,毫无用处。
 *
 * 帧指针链的布局:fp[0] = 上一帧的 fp,fp[1] = 本帧的 return
 * address。栈向低地址增长,所以上一帧的地址必须严格更大、按指针
 * 大小对齐、且跨度合理——任何一步不满足就停,宁少帧不读坏内存。 */
static unsigned wmm_backtrace_from(void *frame, void **buf, unsigned max) {
    void **fp = (void **)frame;

    if (fp == NULL)
        return 0;

    unsigned n = 0;
    while (n < max) {
        void *pc = fp[1];
        if (pc == NULL)
            break;
        buf[n++] = pc;

        uintptr_t parent = (uintptr_t)fp[0];
        if (parent <= (uintptr_t)fp ||
            parent - (uintptr_t)fp > WMM_PROF_MAX_FRAME_BYTES ||
            (parent & (sizeof(void *) - 1u)) != 0)
            break;
        fp = (void **)parent;
    }
    return n;
}

/* ==================== 写线程环形缓冲 ==================== */

/* 把一条样本压进本线程的环。写者独占 head;槽位可能正被合并器读,
 * 所以用 per-slot seqlock:置奇 → 写字段 → release 栅栏 → 置偶,
 * head 最后以 release 发布。合并器看到 head 的新值时,槽内容必已
 * 可见;若它恰好在我们写的过程中来读,首尾世代号不等,它会放弃
 * 这一个样本(下一个合并周期也不会重读——样本宁可丢,不让分配
 * 路径等待)。 */
static void prof_ring_push(wmm_prof_ring_t *r, void *const *pc,
                           unsigned depth, size_t usize, uint32_t weight) {
    uint64_t idx = atomic_load_explicit(&r->head, memory_order_relaxed);
    wmm_prof_slot_t *s = &r->slot[idx % WMM_PROF_RING];

    uint32_t seq = atomic_load_explicit(&s->seq, memory_order_relaxed);
    atomic_store_explicit(&s->seq, seq + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_acq_rel);

    s->depth  = depth;
    s->index  = idx;
    s->usize  = usize;
    s->weight = weight;
    memcpy(s->pc, pc, (size_t)depth * sizeof pc[0]);

    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&s->seq, seq + 2, memory_order_relaxed);
    atomic_store_explicit(&r->head, idx + 1, memory_order_release);
}

/* 采样入口。调用方已判 g_opt.sample_n != 0;这里再读一次,是为了
 * 覆盖"判定之后采样被并发关闭"的窗口——少采一个样本无害,不能让
 * 已关闭的采样继续走栈。 */
__attribute__((noinline))
void wmm_prof_sample(wmm_tcache_t *t, size_t usize, void *alloc_frame) {
    if (t == NULL)
        return;
    unsigned n = g_opt.sample_n;
    if (n == 0)
        return;

    /* 节拍按"成功分配次数"计,与输出里的 count 同一口径:样本
       weight = N 才代表 N 次分配 */
    if (++t->prof_tick < n)
        return;
    t->prof_tick = 0;

    void *pc[WMM_PROF_DEPTH];
    unsigned depth = wmm_backtrace_from(alloc_frame, pc, WMM_PROF_DEPTH);
    if (depth == 0)
        return;   /* 链都走不起来:放弃本样本,不写空记录 */

    prof_ring_push(&t->prof, pc, depth, usize, n);
}

/* ==================== 全局聚合表 ==================== */

/* 一个桶 = 一个调用点键。键存前 4 帧的原始 PC,哈希只用来选探测
 * 起点,命中后再逐帧比较:哈希碰撞若直接合并,会把两个无关调用点
 * 算成一行、给出错误归因;多存 32 字节就能完全避免。depth==0 表示
 * 空桶(有效样本的 depth 至少为 1)。表满时丢弃新样本——采样路径
 * 不能向分配器申请内存(自举),而调用点超过 1024 个本身说明键折叠
 * 的粒度需要重新考虑。 */
typedef struct {
    uintptr_t pc[WMM_PROF_KEY_FRAMES];   /* 键:前 4 帧原始 PC */
    uint32_t  depth;                     /* 键的有效帧数 1..4;0 = 空 */
    uint32_t  pad;                       /* 显式占位,保持 8 字节对齐 */
    uint64_t  bytes;                     /* 估算累计分配字节 */
    uint64_t  count;                     /* 估算累计分配次数 */
} wmm_prof_bucket_t;

static wmm_prof_bucket_t g_prof_tab[WMM_PROF_TAB_N];

pthread_mutex_t g_prof_lock = PTHREAD_MUTEX_INITIALIZER;

/* FNV-1a:逐字节混合前 n 帧的地址。调用点地址的低位随机性足够,
 * 不需要更强的雪崩性质;桶数取 2 的幂,探测用 mask 而非取模。 */
static uint64_t prof_hash(void *const *pc, unsigned n) {
    uint64_t h = 1469598103934665603ull;   /* FNV-1a offset basis */
    for (unsigned i = 0; i < n; i++) {
        uint64_t v = (uint64_t)(uintptr_t)pc[i];
        for (unsigned b = 0; b < 8; b++) {
            h ^= (v >> (b * 8)) & 0xffu;
            h *= 1099511628211ull;         /* FNV prime */
        }
    }
    return h;
}

/* 把一个样本并按权重放大进聚合表。调用方须持 g_prof_lock。
 * n 取 depth 与键帧数的较小者:更深的帧不参与键,但仍保留在样本里
 * (将来若要展示完整栈,不必重新采样)。 */
static void prof_agg_add(void *const *pc, unsigned depth,
                         size_t usize, uint32_t weight) {
    unsigned n = depth < WMM_PROF_KEY_FRAMES ? depth : WMM_PROF_KEY_FRAMES;
    if (n == 0 || weight == 0)
        return;

    uint64_t h = prof_hash(pc, n);
    size_t   mask = (size_t)WMM_PROF_TAB_N - 1;

    for (size_t probe = 0; probe < WMM_PROF_TAB_N; probe++) {
        wmm_prof_bucket_t *b = &g_prof_tab[(size_t)(h + probe) & mask];

        if (b->depth == 0) {   /* 空桶:落键后继续走下面的累加 */
            for (unsigned i = 0; i < n; i++)
                b->pc[i] = (uintptr_t)pc[i];
            for (unsigned i = n; i < WMM_PROF_KEY_FRAMES; i++)
                b->pc[i] = 0;
            b->depth = n;
            b->pad   = 0;
            b->bytes = 0;
            b->count = 0;
        }
        if (b->depth != n)
            continue;

        bool same = true;
        for (unsigned i = 0; i < n; i++)
            if (b->pc[i] != (uintptr_t)pc[i]) {
                same = false;
                break;
            }
        if (!same)
            continue;

        b->bytes += (uint64_t)usize * weight;
        b->count += weight;
        return;
    }
    /* 探测完仍无空位:表满,丢弃本样本 */
}

/* ==================== 合并 ==================== */

/* 把 t 的环里尚未合并的样本并入聚合表,返回本次收下的样本数。
 * 调用方须持 g_prof_lock。
 *
 * 环绕圈(写者比合并器快)时,最旧的 merged..head-RING 段已被覆盖,
 * 再也读不回来;把游标直接推到 head-RING。不收下它们也不阻塞,
 * 合并器只处理仍然活着的那 256 个绝对序号。
 *
 * 每个槽的接收条件:世代号首尾相等且为偶,且槽里的 index 正是所求
 * 的绝对序号。后者同时挡住两种误收——槽已被更新的一代覆盖(index
 * 更大),或合并器读到的正是写者刚写完的下一代(index 已前进)。 */
static uint64_t prof_ring_merge_locked(wmm_tcache_t *t) {
    wmm_prof_ring_t *r = &t->prof;
    uint64_t head   = atomic_load_explicit(&r->head, memory_order_acquire);
    uint64_t merged = r->merged;

    if (head - merged > WMM_PROF_RING)
        merged = head - WMM_PROF_RING;

    uint64_t n = 0;
    for (; merged < head; merged++) {
        wmm_prof_slot_t *s = &r->slot[merged % WMM_PROF_RING];

        uint32_t s1 = atomic_load_explicit(&s->seq, memory_order_acquire);
        if ((s1 & 1u) != 0)
            continue;                 /* 写者正在覆盖:本代丢失 */
        if (s->index != merged)
            continue;                 /* 槽里已是另一代:本代丢失 */

        unsigned depth = s->depth;
        if (depth > WMM_PROF_DEPTH)
            depth = WMM_PROF_DEPTH;
        size_t   usize  = s->usize;
        uint32_t weight = s->weight;
        void    *pc[WMM_PROF_DEPTH];
        if (depth != 0)
            memcpy(pc, s->pc, (size_t)depth * sizeof pc[0]);

        atomic_thread_fence(memory_order_acquire);
        uint32_t s2 = atomic_load_explicit(&s->seq, memory_order_relaxed);
        if (s1 != s2)
            continue;                 /* 读到一半写者介入:整条丢弃 */

        prof_agg_add(pc, depth, usize, weight);
        n++;
    }
    r->merged = merged;
    return n;
}

void wmm_prof_merge(void) {
    /* 先锁注册表再锁聚合表:线程退出只单独拿聚合表,不会反向持锁,
       与 fork prepare 的 threads → prof 顺序一致 */
    pthread_mutex_lock(&g_threads_lock);
    pthread_mutex_lock(&g_prof_lock);
    for (wmm_tcache_t *t = g_threads; t != NULL; t = t->next)
        (void)prof_ring_merge_locked(t);
    pthread_mutex_unlock(&g_prof_lock);
    pthread_mutex_unlock(&g_threads_lock);
}

void wmm_prof_thread_drain(wmm_tcache_t *t) {
    if (t == NULL)
        return;
    pthread_mutex_lock(&g_prof_lock);
    (void)prof_ring_merge_locked(t);
    pthread_mutex_unlock(&g_prof_lock);
}

/* ==================== TOP N ==================== */

/* 取前 k 大:扫描聚合表,把每个桶插入一个按"估算字节降序、次数
 * 降序、PC 升序"排好的定长数组。k 最大 256,插入排序的移动量为
 * O(表大小 × k) 的最坏情形搬移,但只在控制面调用,而且不分配
 * 内存、不做全表排序——TOP N 的 N 很小,全排序是浪费。
 *
 * 并列键的 PC 升序只为输出稳定:同一张聚合表任意时刻取两次,字节
 * 与次数都相同时顺序也一致,监控端做差分对比不会看到无意义的抖动。 */
unsigned wmm_prof_top(wmm_prof_entry_t *dst, unsigned k) {
    if (dst == NULL || k == 0)
        return 0;
    if (k > WMM_PROF_TOP_MAX)
        k = WMM_PROF_TOP_MAX;

    wmm_prof_merge();   /* 控制面取数:先把各线程尚未聚合的样本收进来 */

    pthread_mutex_lock(&g_prof_lock);
    memset(dst, 0, (size_t)k * sizeof dst[0]);

    unsigned n = 0;
    for (size_t i = 0; i < WMM_PROF_TAB_N; i++) {
        const wmm_prof_bucket_t *b = &g_prof_tab[i];
        if (b->depth == 0)
            continue;

        unsigned pos = 0;
        while (pos < n &&
               (dst[pos].bytes > b->bytes ||
                (dst[pos].bytes == b->bytes &&
                 (dst[pos].count > b->count ||
                  (dst[pos].count == b->count &&
                   dst[pos].pc <= b->pc[0])))))
            pos++;
        if (pos >= k)
            continue;   /* 排不进 TOP k */

        unsigned last = n < k ? n : k - 1;
        for (unsigned j = last; j > pos; j--)
            dst[j] = dst[j - 1];
        dst[pos].pc       = b->pc[0];
        dst[pos].bytes    = b->bytes;
        dst[pos].count    = b->count;
        dst[pos].reserved = 0;
        if (n < k)
            n++;
    }
    pthread_mutex_unlock(&g_prof_lock);
    return n;
}
