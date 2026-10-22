/* test_api.c —— 主路径用例(黑盒为主,白盒核对计数)
 *
 * 覆盖:
 *   基础契约:size==0 当 1、free(NULL) 无操作、realloc 三特例
 *         (NULL/原地收缩/搬移拷贝)、usable_size ≥ 请求;
 *   对齐契约:16B 对齐(仅 8B 类允许 8B);
 *   usable 精确值:查表边界(17→32、49→64)、公式段(4097→5120)、
 *         小/大分界(14336/14337)、大对象页对齐取整(100000→102400);
 *   calloc:溢出返回 NULL、复用对象必须重新清零(防信息泄漏);
 *   防御性释放:nregs==1 类与大对象的双释放(页表已清零,走
 *         raw==0 分支)、窗口外野指针、窗口内未映射页——都不许崩溃。
 *         双释放检测依赖"对象已回 slab",而 tcache 快路径只把对象
 *         压栈、页表登记保持,第二次 free 会被静默再收一次——这是
 *         所有 tcache 实现共同的盲区(glibc 同款),检测归 guard 模块。
 *         本组负路径因此临时关缓存,专测慢路径的诊断;
 *   页表与计数零点:万次随机分配/释放风暴后全部归还,36 个 bin 的
 *         live/curslabs/链指针全零、大对象计数全零、抽 witness 指针
 *         查页表已清登记。风暴的归还是两段:应用先全量 free(对象
 *         落进本线程缓存),再 wmm_thread_exit 把缓存刷回 arena——
 *         零点只在"应用持有与缓存都空"之后才有意义。
 */
#include "wmm_internal.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

static int g_fail;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("  FAIL @%d: %s\n", __LINE__, #cond);               \
            g_fail++;                                                  \
        }                                                              \
    } while (0)

/* 固定种子(根号 2 的十六进制前缀),失败可复现 */
static uint64_t g_rng = 0x1A6A09E667F3BCCDULL;

static uint32_t rng(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 32);
}

/* ---- 基础契约 ---- */
static void check_contract_basics(void) {
    /* size==0 按 1 处理:可分配、可释放、至少 8B 对齐 */
    void *z = wmm_alloc(0);
    CHECK(z != NULL);
    CHECK(wmm_usable_size(z) >= 1);
    CHECK(((uintptr_t)z & 7u) == 0);
    wmm_free(z);

    /* free(NULL) 无操作;usable(NULL) == 0 */
    wmm_free(NULL);
    CHECK(wmm_usable_size(NULL) == 0);

    /* 对齐:类 1 起规格全是 16 的倍数、slab 基址页对齐,
       所以一切 ≥9 字节的请求都拿到 16B 对齐指针 */
    for (size_t s = 9; s <= 256; s++) {
        void *p = wmm_alloc(s);
        CHECK(p != NULL);
        CHECK(((uintptr_t)p & 15u) == 0);
        CHECK(wmm_usable_size(p) >= s);
        wmm_free(p);
    }
}

/* ---- usable 精确值:查表段、线性扫段、小/大分界 ---- */
static void check_usable_exact(void) {
    static const struct { size_t req, want; } t[] = {
        {     8,     8 },   /* 最小类 */
        {    16,    16 },
        {    17,    32 },   /* 查表边界:17..24 归 32B 类 */
        {    48,    48 },
        {    49,    64 },
        {   100,   112 },   /* 100 → 104 粒度 → 112B 类 */
        {   128,   128 },
        {  4096,  4096 },   /* 查表上限,恰为自身 */
        {  4097,  5120 },   /* 进入线性扫段 */
        {  5120,  5120 },
        {  8192,  8192 },
        {  8193, 10240 },
        { 14336, 14336 },   /* 最大小对象 */
        { 14337, 16384 },   /* 最小大对象路径:4 页 */
        { 65536, 65536 },   /* 大对象,恰 16 页 */
        { 100000, 102400 }, /* 大对象,向上取整到 25 页 */
    };

    for (size_t k = 0; k < sizeof(t) / sizeof(t[0]); k++) {
        unsigned char *p = wmm_alloc(t[k].req);
        CHECK(p != NULL);
        if (p == NULL)
            continue;
        CHECK(wmm_usable_size(p) == t[k].want);
        /* 对齐契约:16B;唯一例外是 8B 最小类(请求 ≤ 8,只保证 8B,
           wmm.h 明文允许)——奇数号对象天然只有 8B 对齐 */
        if (t[k].req > 8)
            CHECK(((uintptr_t)p & 15u) == 0);
        else
            CHECK(((uintptr_t)p & 7u) == 0);
        p[t[k].req - 1] = 0x5A;   /* 请求的最后一个字节必须可写 */
        p[0] = 0xA5;
        wmm_free(p);
    }
}

/* ---- calloc:溢出与清零(含复用对象的再清零) ---- */
static void check_calloc(void) {
    /* 乘法溢出 → NULL,不回绕;errno 与 glibc 一致为 ENOMEM */
    errno = 0;
    CHECK(wmm_calloc(SIZE_MAX / 2, 3) == NULL);
    CHECK(errno == ENOMEM);
    errno = 0;
    CHECK(wmm_calloc((size_t)1 << 40, (size_t)1 << 30) == NULL);
    CHECK(errno == ENOMEM);

    /* 先造脏块再释放;同线程同类的下一次分配按 LIFO 必弹出它——
       calloc 若跳过 memset 就把 0xAB 泄漏给应用了 */
    unsigned char *dirty = wmm_alloc(64);
    CHECK(dirty != NULL);
    memset(dirty, 0xAB, 64);
    wmm_free(dirty);

    unsigned char *c = wmm_calloc(4, 16);
    CHECK(c != NULL);
    bool clean = true;
    for (int k = 0; k < 64; k++)
        clean = clean && (c[k] == 0);
    CHECK(clean);
    wmm_free(c);
}

/* ---- realloc:三特例 + 搬移内容保持 ---- */
static void check_realloc(void) {
    /* p==NULL 等价 alloc */
    unsigned char *p = wmm_realloc(NULL, 100);
    CHECK(p != NULL && wmm_usable_size(p) >= 100);
    for (int k = 0; k < 100; k++)
        p[k] = (unsigned char)k;

    /* 收缩原地:指针不变 */
    void *q = wmm_realloc(p, 50);
    CHECK(q == p);

    /* 扩容搬移:内容前 100 字节原样到达 */
    q = wmm_realloc(p, 5000);
    CHECK(q != NULL);
    CHECK(q != p);   /* 100(112B 类)→ 5000(5120B 类),必搬移 */
    bool same = true;
    for (int k = 0; k < 100; k++)
        same = same && (((unsigned char *)q)[k] == (unsigned char)k);
    CHECK(same);
    wmm_free(q);

    /* n==0 等价 free 并返回 NULL */
    p = wmm_alloc(100);
    CHECK(p != NULL);
    CHECK(wmm_realloc(p, 0) == NULL);

    /* reallocarray:乘积溢出必须拒绝,且旧块原样有效;正常乘积
       与 realloc 同语义 */
    p = wmm_alloc(32);
    CHECK(p != NULL);
    errno = 0;
    CHECK(wmm_reallocarray(p, SIZE_MAX / 2, 3) == NULL);
    CHECK(errno == ENOMEM);
    CHECK(wmm_usable_size(p) >= 32);
    q = wmm_reallocarray(p, 4, 100);
    CHECK(q != NULL && wmm_usable_size(q) >= 400);
    wmm_free(q);
}

/* ---- 防御性释放:三种非法指针都不许把进程带走 ---- */
static void check_defensive_free(void) {
    /* 双释放的诊断依赖"对象已经回到 slab、页表登记已清"(慢路径上
       nregs==1 的类释放即整块退休,再 free 走 raw==0 分支)。tcache
       快路径把对象压在栈上、页表保持登记,第二次 free 会被它静默
       再收一次——glibc tcache 同款盲区,检测归 guard 模块。本组临时
       关缓存,让负路径落到慢路径:测的是诊断契约,不是缓存 */
    wmm_thread_init();
    bool saved = g_tcache->enabled;
    g_tcache->enabled = false;

    /* nregs==1 的类:释放即整块退休 + 页表清零,再释放走 raw==0 */
    void *p = wmm_alloc(4096);
    CHECK(p != NULL);
    wmm_free(p);
    wmm_free(p);          /* guard 落地后写 0xBE 事件;现在 dprintf 后返回 */

    /* 大对象同理:enabled 关时大对象缓存一并旁路 */
    void *l = wmm_alloc((size_t)1 << 20);
    CHECK(l != NULL);
    wmm_free(l);
    wmm_free(l);

    /* 窗口外(栈变量)与窗口内从未映射的页 */
    int on_stack = 0;
    wmm_free(&on_stack);
    wmm_free(g_heap_cur);   /* bump 当前位置:在窗口内、从未映射 */

    g_tcache->enabled = saved;
}

/* ---- 对齐分配(三档 + 错误码 + 档 2 不变量穷举) ---- */
static void check_aligned(void) {
    /* posix_memalign 契约错误码:非 2 的幂、小于 sizeof(void*)、
       输出参数为空;失败时 *out 必须是 NULL */
    void *m = (void *)0x1;
    CHECK(wmm_posix_memalign(&m, 0, 100) == EINVAL && m == NULL);
    CHECK(wmm_posix_memalign(&m, 3, 100) == EINVAL && m == NULL);
    CHECK(wmm_posix_memalign(&m, 4, 100) == EINVAL && m == NULL);
    CHECK(wmm_posix_memalign(&m, 24, 100) == EINVAL && m == NULL);
    CHECK(wmm_posix_memalign(NULL, 16, 100) == EINVAL);

    /* aligned_alloc 契约错误码:非 2 的幂(含 0——它骗过幂检查,
       0 & (0-1) == 0),errno 语义 */
    errno = 0;
    CHECK(wmm_aligned_alloc(0, 100) == NULL && errno == EINVAL);
    errno = 0;
    CHECK(wmm_aligned_alloc(12, 100) == NULL && errno == EINVAL);
    errno = 0;
    CHECK(wmm_aligned_alloc(24, 100) == NULL && errno == EINVAL);

    /* 档 3 的回绕挡板:size 接近 SIZE_MAX 时 size+alignment 会绕回
       小值;挡不住就按小分配成功,挡住了应报 ENOMEM */
    CHECK(wmm_posix_memalign(&m, (size_t)1 << 62, SIZE_MAX - 100) == ENOMEM);

    /* 档 2 的取整同样会回绕,必须在加法前挡住;size 为 0 时不能
       落到 8B 类,否则返回的指针不满足请求的对齐 */
    errno = 0;
    CHECK(wmm_aligned_alloc(4096, SIZE_MAX) == NULL && errno == ENOMEM);
    m = (void *)0x1;
    CHECK(wmm_posix_memalign(&m, 4096, SIZE_MAX) == ENOMEM && m == NULL);

    unsigned char *z = wmm_aligned_alloc(4096, 0);
    CHECK(z != NULL && ((uintptr_t)z & 4095u) == 0);
    wmm_free(z);

    m = (void *)0x1;
    CHECK(wmm_posix_memalign(&m, 4096, 0) == 0);
    CHECK(m != NULL && ((uintptr_t)m & 4095u) == 0);
    wmm_free(m);

    /* 档 2 依据的穷举证明:16..4096 全部 2 的幂 alignment 的全部
       倍数,最小上界类的规格必是 alignment 的倍数。类表一旦改坏
       (或有人动了取整逻辑),这里立刻炸——比抽测可靠。 */
    for (size_t a = 16; a <= WMM_PAGE; a <<= 1)
        for (size_t x = a; x <= WMM_SMALL_MAXCLASS; x += a)
            CHECK(wmm_bin_info[wmm_size2index(x)].reg_size % a == 0);

    /* 全档位 × 尺寸段:对齐成立、可用量够、首尾字节可写、释放干净;
       posix_memalign 同参数再走一遍,两个 API 行为必须一致 */
    static const size_t aligns[] = { 1, 2, 4, 8, 16, 32, 64, 128, 256,
                                     512, 1024, 2048, 4096, 8192,
                                     65536, (size_t)1 << 20 };
    static const size_t sizes[]  = { 1, 5, 8, 9, 17, 100, 4095, 4096,
                                     4097, 14336, 14337, 65536, 300000 };

    for (size_t ia = 0; ia < sizeof(aligns) / sizeof(aligns[0]); ia++) {
        size_t a = aligns[ia];
        for (size_t is = 0; is < sizeof(sizes) / sizeof(sizes[0]); is++) {
            size_t s = sizes[is];

            unsigned char *p = wmm_aligned_alloc(a, s);
            CHECK(p != NULL);
            CHECK(((uintptr_t)p & (a - 1)) == 0);
            size_t usable = wmm_usable_size(p);
            CHECK(usable >= s);
            p[0] = 0x5A;
            p[s - 1] = 0xA5;   /* 请求的首尾字节必须可写 */

            /* 档 3 白盒:usable 必须精确等于"映射末尾 − p"(头部
               零头已扣)。虚报头部的话 realloc 原地扩容会写穿末端 */
            if (a > WMM_PAGE) {
                void *ext;
                CHECK(wmm_pte_is_extent(p, &ext));
                wmm_extent_t *e = (wmm_extent_t *)ext;
                CHECK((char *)p + usable ==
                      (char *)e->addr + e->npages * WMM_PAGE);
            }
            wmm_free(p);

            /* posix_memalign:< 8 的对齐按契约拒绝,其余与
               aligned_alloc 同参数等价 */
            if (a < 8) {
                CHECK(wmm_posix_memalign(&m, a, s) == EINVAL && m == NULL);
            } else {
                CHECK(wmm_posix_memalign(&m, a, s) == 0);
                CHECK(m != NULL && ((uintptr_t)m & (a - 1)) == 0);
                CHECK(wmm_usable_size(m) >= s);
                wmm_free(m);
            }
        }
    }

    /* 档 3 直接从物理层过分配,绕开了 wmm_alloc 的大对象分支。
       线程计数必须在这里补记,且分配/释放两侧都按可用大小
       (映射末尾 − 对象地址)记,否则线程表里两边永远对不平。
       断言取增量:测试组共享进程,绝对计数带着前面用例的历史 */
    wmm_thread_init();
    CHECK(g_tcache != NULL);
    if (g_tcache != NULL) {
        uint64_t ab = g_tcache->t_alloc_bytes;
        uint64_t fb = g_tcache->t_free_bytes;
        uint64_t ac = g_tcache->t_alloc_cnt;
        uint64_t fc = g_tcache->t_free_cnt;

        void *q = wmm_aligned_alloc(8192, 20000);   /* 8K 对齐 > 页大小 */
        CHECK(q != NULL);
        if (q != NULL) {
            CHECK(((uintptr_t)q & 8191u) == 0);
            size_t usable = wmm_usable_size(q);

            CHECK(g_tcache->t_alloc_bytes - ab == usable);
            CHECK(g_tcache->t_alloc_cnt - ac == 1u);
            CHECK(g_tcache->t_free_bytes - fb == 0);
            CHECK(g_tcache->t_free_cnt - fc == 0);

            wmm_free(q);
            CHECK(g_tcache->t_free_bytes - fb == usable);
            CHECK(g_tcache->t_free_cnt - fc == 1u);
            CHECK(g_tcache->t_alloc_bytes - ab ==
                  g_tcache->t_free_bytes - fb);
        }
    }
}

/* ---- 页表与计数零点:万次随机风暴 + 全量归还 ---- */
static void check_random_storm(void) {
    enum { SLOTS = 4096, WITNESS = 16 };
    static void *slot[SLOTS];
    static void *witness[WITNESS];
    int n_witness = 0;

    for (int op = 0; op < 10000; op++) {
        uint32_t s = rng() % SLOTS;
        if (slot[s] != NULL && (rng() & 1) == 0) {
            wmm_free(slot[s]);
            slot[s] = NULL;
        } else if (slot[s] == NULL) {
            size_t sz;
            if ((rng() & 15) == 0)
                sz = 16384 + (rng() % (1u << 18));   /* 每 16 次掺一个大对象 */
            else
                sz = 1 + (rng() % 8192);
            void *p = wmm_alloc(sz);
            CHECK(p != NULL);          /* 64GB 窗口下风暴不可能 OOM */
            size_t usable = wmm_usable_size(p);
            CHECK(usable >= sz);
            /* 同上:8B 类只保证 8B 对齐,其余一律 16B */
            CHECK(((uintptr_t)p & 15u) == 0 || usable == 8);
            slot[s] = p;
            if (n_witness < WITNESS)
                witness[n_witness++] = p;
        }
    }

    /* 全部释放分两段:应用 free 的对象先落进本线程缓存,再由线程
       退出把缓存刷回 arena——两段都做完,slab 才会全空退休、extent
       才会归还 */
    for (int s = 0; s < SLOTS; s++)
        if (slot[s] != NULL)
            wmm_free(slot[s]);
    wmm_thread_exit();   /* 刷空缓存 + 从注册表摘除 + munmap 缓存块 */

    /* 全零特例:live 恒等式此刻两边都是 0(应用持有与缓存皆空) */
    for (szind_t i = 0; i < WMM_NBINS; i++) {
        CHECK(g_bins[i].live == 0);
        CHECK(g_bins[i].curslabs == 0);
        CHECK(g_bins[i].slabcur == NULL);
        CHECK(g_bins[i].full_head == NULL);
        CHECK(g_bins[i].nonfull_root == NULL);
        CHECK(g_bins[i].ndalloc > 0 || g_bins[i].nslabs == 0);
    }
    CHECK(g_large.live == 0);
    CHECK(g_large.live_bytes == 0);
    CHECK(g_large.ndalloc > 0);

    /* witness 指针的页表登记已清零(退休/释放时同步清的) */
    for (int k = 0; k < n_witness; k++)
        CHECK(wmm_pte_get(witness[k]).raw == 0);
}

int test_api(void) {
    g_fail = 0;
    wmm_boot();

    check_contract_basics();
    check_usable_exact();
    check_calloc();
    check_realloc();
    check_aligned();
    check_defensive_free();
    check_random_storm();

    if (g_fail == 0)
        printf("  all api checks passed\n");
    return g_fail;
}
