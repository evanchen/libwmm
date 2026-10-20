/* test_boot.c —— M2 启动初始化用例(白盒:直接检查内部状态)
 *
 * 覆盖:
 *   1. 窗口预留:base/cur/end 记录正确,页表数组覆盖整个实际窗口;
 *   2. 页表零值来自内核零页:boot 前后 RSS 增量远小于页表数组总
 *      大小——若实现里手滑写了 memset,增量会顶到整表大小;
 *   3. bin 表零初始化:计数为 0、链指针为空;
 *   4. wmm_boot 幂等:重复调用不改变任何状态;
 *   5. base 分配器:8B 对齐、地址不重叠、内容为零(内核零页)、
 *      超过块大小的大请求能触发追加块。
 *
 * 手工用例(需外部条件,不进自动流程):
 *   预留失败减半重试——`bash -c 'ulimit -v 8000000; ./build/test/run'`,
 *   观察 stderr 的 "reservation failed, retry" 日志;
 *   verbose 日志——在 boot 前 g_opt.verbose = true 重跑即可。
 */
#include "wmm_internal.h"

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

static int g_fail;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("  FAIL @%d: %s\n", __LINE__, #cond);               \
            g_fail++;                                                  \
        }                                                              \
    } while (0)

/* /proc/self/statm 第二项 = 常驻页数;换算成 KB。读不到返回 0。 */
static size_t rss_kb(void) {
    FILE  *f = fopen("/proc/self/statm", "r");
    size_t total = 0, res = 0;

    if (f == NULL)
        return 0;
    if (fscanf(f, "%zu %zu", &total, &res) != 2)
        res = 0;
    fclose(f);
    return res * (size_t)sysconf(_SC_PAGESIZE) / 1024;
}

int test_boot(void) {
    g_fail = 0;

    /* ---- RSS 对照放在 boot 之前(测的就是 boot 自己碰了多少) ---- */
    size_t rss_before = rss_kb();

    wmm_boot();

    size_t rss_after = rss_kb();

    /* ---- 1) 窗口与页表记录 ---- */
    CHECK(g_heap_base != NULL);
    CHECK((uintptr_t)g_heap_base % WMM_PAGE == 0);      /* 页对齐 */
    CHECK((char *)g_heap_end > (char *)g_heap_base);
    CHECK(g_heap_cur == g_heap_base);                   /* bump 起步位置 */
    CHECK(g_pte != NULL);
    CHECK(sizeof(wmm_pte_t) == 8);                      /* 页表项 ABI */

    size_t win_bytes = (size_t)((char *)g_heap_end - (char *)g_heap_base);
    size_t nptes     = win_bytes / WMM_PAGE;
    size_t pte_bytes = nptes * sizeof(wmm_pte_t);
    printf("  window = %zu MB, page table = %zu MB, bins = %d\n",
           win_bytes >> 20, pte_bytes >> 20, WMM_NBINS);

    /* 页表覆盖整个窗口,且零值(抽样表头/表中/表尾) */
    CHECK(nptes >= 1);
    CHECK(g_pte[0].raw == 0);
    CHECK(g_pte[nptes / 2].raw == 0);
    CHECK(g_pte[nptes - 1].raw == 0);

    /* ---- 2) RSS 台阶:零值必须来自内核零页,不是 memset ---- */
    if (rss_before != 0 && rss_after != 0) {
        size_t delta = rss_after > rss_before ? rss_after - rss_before : 0;
        printf("  boot RSS delta = %zu KB (full memset would add %zu KB)\n",
               delta, pte_bytes >> 10);
        CHECK(delta < pte_bytes / 1024 / 4);   /* 允许零头,远小于整表 */
    }

    /* ---- 3) bin 表零初始化 ---- */
    for (size_t i = 0; i < WMM_NBINS; i++) {
        CHECK(g_bins[i].slabcur == NULL);
        CHECK(g_bins[i].full_head == NULL);
        CHECK(g_bins[i].nonfull_root == NULL);
        CHECK(g_bins[i].live == 0);
        CHECK(g_bins[i].ndalloc == 0);
        CHECK(g_bins[i].nslabs == 0 && g_bins[i].curslabs == 0);
    }

    /* ---- 4) 幂等:重复 boot 不改变任何状态 ---- */
    void       *base0 = g_heap_base, *cur0 = g_heap_cur, *end0 = g_heap_end;
    wmm_pte_t  *pte0  = g_pte;
    wmm_boot();
    CHECK(g_heap_base == base0 && g_heap_cur == cur0 && g_heap_end == end0);
    CHECK(g_pte == pte0);

    /* ---- 5) base 分配器 ---- */
    unsigned char *p1 = wmm_base_alloc(1);
    unsigned char *p2 = wmm_base_alloc(1000);
    unsigned char *p3 = wmm_base_alloc((size_t)8 << 20);   /* 8MB:超块大小,触发追加 */
    CHECK(p1 != NULL && p2 != NULL && p3 != NULL);
    CHECK(((uintptr_t)p1 & 7u) == 0);
    CHECK(((uintptr_t)p2 & 7u) == 0);
    CHECK(((uintptr_t)p3 & 7u) == 0);
    CHECK(p1 != p2 && p2 != p3 && p1 != p3);

    /* 新页内容为零(内核零页),且 p1 的写入不影响 p2(地址不重叠) */
    CHECK(p2[0] == 0 && p2[999] == 0 && p3[0] == 0);
    p1[0] = 0xAB;
    CHECK(p2[0] == 0);

    /* ---- 6) 运行期配置命令 ---- */
    /* 查询类命令的返回值就是答案,不是成功码 */
    CHECK(wmm_ctl("config.lg_page") == (int)WMM_LG_PAGE);
    /* verbose 是普通开关:开、关都要落值;测完恢复,不污染后续组 */
    bool saved = g_opt.verbose;
    CHECK(wmm_ctl("config.verbose", 1) == 0);
    CHECK(g_opt.verbose);
    CHECK(wmm_ctl("config.verbose", 0) == 0);
    CHECK(!g_opt.verbose);
    g_opt.verbose = saved;

    if (g_fail == 0)
        printf("  all boot checks passed\n");
    return g_fail;
}
