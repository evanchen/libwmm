/* test_slab.c —— slab 空闲链、页表登记、bin 状态机用例(白盒)
 *
 * 覆盖:
 *   建链后逐对象弹出:地址 = addr + r*reg_size,按编号升序、
 *         互不重叠、恰好覆盖全 slab;
 *   页表:slab 每一页的页表项都指向该 slab;从未映射的页 raw==0
 *         (这就是 free 反查"未分配地址"的挂点);
 *   随机 alloc/free 序列中周期性沿链遍历:长度恰等于 nfree、
 *         无环;排空后链恰好覆盖全部对象;
 *   bin 状态机:满→full 链、由满转非满(地址序)、取最低地址
 *         转正、全空退休的三条路径(热位/nonfull/nregs==1 的 full)。
 *
 * 页与 slab 头由测试自备:本组只验证 slab/bin 状态机。若走物理层,
 * extent 的复用、合并与地址序会让 slab 地址不可预知,断言就钉不住。
 * map_pages 在预留窗口内 MAP_FIXED 并推进 g_heap_cur,与物理层取页
 * 的动作一致;slab 头由 new_slab 直接向 base 区申请。
 */
#include "wmm_internal.h"

#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>

static int g_fail;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("  FAIL @%d: %s\n", __LINE__, #cond);               \
            g_fail++;                                                  \
        }                                                              \
    } while (0)

/* 固定种子(π 的十六进制前缀):失败可复现,不用 rand() 免得引入
   libc 全局状态与可移植性问题。xorshift64 输出够散,做压力序列足够。 */
static uint64_t g_rng = 0x243F6A8885A308D3ULL;

static uint32_t rng(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 32);
}

/* 在窗口内把 g_heap_cur 起始的 bytes 改成可写页,推进 bump 指针。
   MAP_FIXED 会覆盖目标处的既有映射——目标区是 boot 预留的 PROT_NONE
   空白,覆盖即安全;与物理层取页的动作一致。 */
static void *map_pages(size_t bytes) {
    bytes = (bytes + WMM_PAGE - 1) & ~(WMM_PAGE - 1);
    void *p = mmap(g_heap_cur, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    g_heap_cur = (char *)g_heap_cur + bytes;
    return p;
}

/* 沿空闲链遍历,返回走到的对象数。任何损坏模式都会让返回值
   偏离 nfree:链提前断(少)、成环(走到的超过对象总数,循环内截断)、
   编号越界(提前断)。调用方 CHECK(返回值 == sl->nfree) 即完成校验。 */
static uint32_t walk_freelist(const wmm_slab_t *sl) {
    const wmm_bin_info_t *bi = &wmm_bin_info[sl->szind];
    uint32_t seen = 0;
    uint32_t r    = sl->free_head;

    while (r != UINT32_MAX) {
        if (r >= bi->nregs)
            break;                    /* 编号越界:链已损坏 */
        seen++;
        if (seen > bi->nregs)
            break;                    /* 走到的比对象总数还多:必成环 */
        r = *(const uint32_t *)((const char *)sl->addr + (size_t)r * bi->reg_size);
    }
    return seen;
}

static wmm_slab_t *new_slab(void) {
    return (wmm_slab_t *)wmm_base_alloc(sizeof(wmm_slab_t));
}

/* ---- 单个尺寸类的全链路 ---- */
static void check_class(szind_t i) {
    const wmm_bin_info_t *bi = &wmm_bin_info[i];
    void       *addr = map_pages(bi->slab_size);
    wmm_slab_t *sl   = new_slab();

    CHECK(addr != NULL && sl != NULL);
    if (addr == NULL || sl == NULL)
        return;

    wmm_slab_init(sl, addr, i);
    CHECK(sl->nfree == bi->nregs);
    CHECK(sl->free_head == 0);

    /* 每一页都指向本 slab */
    size_t npages = bi->slab_size >> WMM_LG_PAGE;
    for (size_t pg = 0; pg < npages; pg++) {
        wmm_slab_t *out = NULL;
        CHECK(wmm_pte_is_slab((char *)addr + pg * WMM_PAGE, &out));
        CHECK(out == sl);
    }

    /* 建链序即弹出序,第 r 次弹出恰是 addr + r*reg_size */
    void *held[512];
    CHECK(bi->nregs <= 512);
    for (uint32_t r = 0; r < bi->nregs; r++) {
        void *p = wmm_slab_alloc(sl);
        CHECK(p == (char *)addr + (size_t)r * bi->reg_size);
        held[r] = p;
    }
    CHECK(sl->nfree == 0);
    CHECK(walk_freelist(sl) == 0);

    /* 逆序全压回:链应恢复成完整升序(0→1→…) */
    for (uint32_t r = bi->nregs; r-- > 0; )
        wmm_slab_free(sl, held[r]);
    CHECK(sl->nfree == bi->nregs);
    CHECK(walk_freelist(sl) == bi->nregs);

    /* 随机 alloc/free,周期校验链长 == nfree */
    uint32_t nheld = 0;
    for (int op = 0; op < 3000; op++) {
        if (sl->nfree > 0 && (rng() & 1) && nheld < bi->nregs) {
            held[nheld++] = wmm_slab_alloc(sl);
        } else if (nheld > 0) {
            uint32_t k = rng() % nheld;
            wmm_slab_free(sl, held[k]);
            held[k] = held[--nheld];   /* 尾部换位删,保持 held 稠密 */
        }
        if (op % 256 == 0)
            CHECK(walk_freelist(sl) == sl->nfree);
    }
    for (uint32_t k = 0; k < nheld; k++)
        wmm_slab_free(sl, held[k]);
    CHECK(sl->nfree == bi->nregs);
    CHECK(walk_freelist(sl) == bi->nregs);   /* 排空后链覆盖全部对象 */
}

/* ---- bin 状态机全场景(16B 类,nregs=256) ---- */
static void check_bin_statemachine(void) {
    wmm_bin_t *b = &g_bins[1];
    const wmm_bin_info_t *bi = &wmm_bin_info[1];

    void *pa = map_pages(bi->slab_size);
    void *pb = map_pages(bi->slab_size);
    wmm_slab_t *A = new_slab(), *B = new_slab();
    CHECK(pa != NULL && pb != NULL && A != NULL && B != NULL);
    if (pa == NULL || pb == NULL || A == NULL || B == NULL)
        return;
    CHECK((uintptr_t)pa < (uintptr_t)pb);   /* map_pages 顺序 = 地址序 */

    wmm_slab_init(A, pa, 1);
    wmm_slab_init(B, pb, 1);
    CHECK(b->curslabs == 0 && b->live == 0 && b->nslabs == 0);

    void *heldA[256], *heldB[256];

    /* 填满 A:最后一次弹出后 A 仍在热位(满而不退,惰性挂 full) */
    wmm_bin_slab_install(b, A);
    CHECK(b->slabcur == A && b->curslabs == 1 && b->nslabs == 1);
    for (uint32_t r = 0; r < bi->nregs; r++) {
        heldA[r] = wmm_bin_alloc(b);
        CHECK((uintptr_t)heldA[r] >= (uintptr_t)pa &&
              (uintptr_t)heldA[r] < (uintptr_t)pb);
    }
    CHECK(b->slabcur == A && A->nfree == 0);
    CHECK(wmm_bin_alloc(b) == NULL);        /* 此刻 A 才挂 full */
    CHECK(b->slabcur == NULL && b->full_head == A);

    /* 填满 B:full 链头插,B 在头 */
    wmm_bin_slab_install(b, B);
    for (uint32_t r = 0; r < bi->nregs; r++) {
        heldB[r] = wmm_bin_alloc(b);
        CHECK((uintptr_t)heldB[r] >= (uintptr_t)pb &&
              (uintptr_t)heldB[r] < (uintptr_t)pb + bi->slab_size);
    }
    CHECK(wmm_bin_alloc(b) == NULL);
    CHECK(b->full_head == B && B->full_next == A);
    CHECK(b->curslabs == 2 && b->nslabs == 2);

    /* 由满转非满:A 先释放一个 → 链头;B 再释放一个 → 排在 A 后(地址序) */
    CHECK(wmm_bin_dalloc(b, A, heldA[0]) == false);
    CHECK(b->nonfull_root == A);
    CHECK(wmm_bin_dalloc(b, B, heldB[0]) == false);
    CHECK(b->nonfull_root == A && A->ph_next == B && B->ph_prev == A);

    /* 取最低地址:分配必须落在 A(转正为热位) */
    void *q = wmm_bin_alloc(b);
    CHECK((uintptr_t)q >= (uintptr_t)pa && (uintptr_t)q < (uintptr_t)pb);
    CHECK(b->slabcur == A && b->nonfull_root == B);

    /* 全空退休自 nonfull:B 还剩 255 个在应用手里,逐个释放,
       最后一次 nfree 到达 nregs → 退休返回真 */
    bool retired = false;
    for (uint32_t k = 1; k < bi->nregs; k++)
        retired = wmm_bin_dalloc(b, B, heldB[k]);
    CHECK(retired);
    CHECK(b->curslabs == 1 && b->nonfull_root == NULL);
    CHECK(b->full_head == NULL);      /* full 早已清空 */
    CHECK(b->live == bi->nregs);      /* 只剩 A 的 256 个存活 */

    /* 全空退休自热位:释放 A 的全部(heldA[1..255] + 弹出的 q) */
    CHECK(wmm_bin_dalloc(b, A, q) == false);
    retired = false;
    for (uint32_t k = 1; k < bi->nregs; k++)
        retired = wmm_bin_dalloc(b, A, heldA[k]);
    CHECK(retired);
    CHECK(b->curslabs == 0 && b->slabcur == NULL && b->live == 0);
    CHECK(b->nslabs == 2);            /* 累计值不随退休回退 */
}

/* ---- nregs==1 的类:分配即满、释放即全空,退休走 full 链 ---- */
static void check_nregs1(void) {
    wmm_bin_t *b = &g_bins[28];      /* 4096B:pgs=1,nregs=1 */
    void *pa = map_pages(WMM_PAGE);
    wmm_slab_t *S = new_slab();
    CHECK(pa != NULL && S != NULL);
    if (pa == NULL || S == NULL)
        return;

    wmm_slab_init(S, pa, 28);
    wmm_bin_slab_install(b, S);
    void *p = wmm_bin_alloc(b);
    CHECK(p == pa);
    CHECK(S->nfree == 0 && b->slabcur == S);
    CHECK(wmm_bin_alloc(b) == NULL);      /* S 挂 full */
    CHECK(b->full_head == S);

    /* 一次释放即全空:退休必须从 full 链摘(通用路径只摘 nonfull,
       那会把这个 slab 永久遗留在 full 链上) */
    CHECK(wmm_bin_dalloc(b, S, p) == true);
    CHECK(b->curslabs == 0 && b->full_head == NULL && b->slabcur == NULL);
    CHECK(b->nonfull_root == NULL && b->live == 0);
}

/* ---- nregs==2 的类:热位"满而不退"期间释放一个,不许误判成满→非满 ----
 * 这是状态机判据的回归用例:若 dalloc 按"nfree==1 即从 full 摘"来写,
 * 此刻 slabcur 会同时出现在热位与 nonfull 链、满链被清空。 */
static void check_nregs2_hotpath(void) {
    wmm_bin_t *b = &g_bins[33];      /* 10240B:pgs=5,nregs=2 */
    const wmm_bin_info_t *bi = &wmm_bin_info[33];
    void *pa = map_pages(bi->slab_size);
    wmm_slab_t *C = new_slab();
    CHECK(pa != NULL && C != NULL);
    if (pa == NULL || C == NULL)
        return;

    wmm_slab_init(C, pa, 33);
    wmm_bin_slab_install(b, C);
    void *p0 = wmm_bin_alloc(b);
    void *p1 = wmm_bin_alloc(b);
    CHECK(p0 == pa && p1 == (char *)pa + 10240);
    CHECK(b->slabcur == C && b->nonfull_root == NULL);

    /* 满(仍在热位)时释放 p1:留在热位,任何链都不许动 */
    CHECK(wmm_bin_dalloc(b, C, p1) == false);
    CHECK(b->slabcur == C);
    CHECK(b->nonfull_root == NULL && b->full_head == NULL);

    /* 热位还有空位:再分配应弹出同一个 p1 */
    CHECK(wmm_bin_alloc(b) == p1);

    /* 全空:两次释放,第二次退休自热位 */
    CHECK(wmm_bin_dalloc(b, C, p0) == false);
    CHECK(wmm_bin_dalloc(b, C, p1) == true);
    CHECK(b->curslabs == 0 && b->slabcur == NULL && b->live == 0);
    CHECK(walk_freelist(C) == 2);
}

int test_slab(void) {
    g_fail = 0;
    wmm_boot();   /* 幂等;test_boot 先跑过也无妨 */

    static const szind_t classes[] = { 0, 3, 7, 28, 33 };
    for (size_t k = 0; k < sizeof(classes) / sizeof(classes[0]); k++) {
        printf("  -- class %zu (%uB, nregs=%u) --\n", (size_t)classes[k],
               wmm_bin_info[classes[k]].reg_size,
               wmm_bin_info[classes[k]].nregs);
        check_class(classes[k]);
    }

    check_bin_statemachine();
    check_nregs1();
    check_nregs2_hotpath();

    /* 从未映射的页:页表 raw==0,反查两头都不认——free 的野指针挂点 */
    wmm_pte_t e = wmm_pte_get(g_heap_cur);
    CHECK(e.raw == 0);
    wmm_slab_t *sout = (wmm_slab_t *)0x1;
    void       *vout = (void *)0x1;
    CHECK(!wmm_pte_is_slab(g_heap_cur, &sout));
    CHECK(!wmm_pte_is_extent(g_heap_cur, &vout));

    if (g_fail == 0)
        printf("  all slab checks passed\n");
    return g_fail;
}
