/* tests/main.c —— 测试入口:汇总各 test_*.c 的用例函数,依次运行。
 *
 * 约定:每个用例文件导出 `int test_xxx(void)`,返回失败断言数,
 * 0 = 全过。新增用例:写 tests/test_xxx.c,并在下面的表里登记。
 * 用例是白盒的:允许直接包含 wmm_internal.h 检查内部状态;
 * 链接的是 debug 库(ASan/UBSan),内存错误会被 sanitizer 拦下。
 */
#include <stdio.h>
#include <stddef.h>

int test_boot(void);   /* 由 tests/test_boot.c 提供 */
int test_slab(void);   /* 由 tests/test_slab.c 提供 */
int test_api(void);    /* 由 tests/test_api.c 提供 */
int test_phys(void);   /* 由 tests/test_phys.c 提供 */

typedef int (*wmm_test_fn)(void);

static const struct {
    const char  *name;
    wmm_test_fn  fn;
} g_tests[] = {
    { "boot", test_boot },
    { "slab", test_slab },
    { "api",  test_api },
    { "phys", test_phys },
};

int main(void) {
    int groups_failed = 0;

    for (size_t i = 0; i < sizeof(g_tests) / sizeof(g_tests[0]); i++) {
        printf("---- %s ----\n", g_tests[i].name);
        int failures = g_tests[i].fn();
        printf("---- %s: %s(%d 项断言失败) ----\n\n",
               g_tests[i].name, failures ? "FAIL" : "PASS", failures);
        groups_failed += (failures != 0);
    }

    if (groups_failed != 0) {
        printf("RESULT: FAIL(%d 组)\n", groups_failed);
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
