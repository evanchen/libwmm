/* wmm_wrap.c —— malloc 家族的链接期接管(-Wl,--wrap)
 *
 * 应用照常写 malloc/free/calloc/realloc/posix_memalign/aligned_alloc,
 * 链接时加 -Wl,--wrap=<名字>:GNU ld 把本可执行文件里对这些符号的
 * 未定义引用改写成 __wrap_<名字>,于是全部落到 libwmm。这对
 * 已经写好的程序是零源码改动——不需要宏替换,也不需要重新声明。
 *
 * 函数体只做转发:size==0、free(NULL)、溢出检查、对齐与错误码等
 * 语义全由 wmm_* 负责,这里不重复判断,也不吞错误。包装层越薄,
 * "经 wrap"与"直接调 wmm_*"的行为差异就越不可能出现。
 *
 * 为什么单独成文件而不是把 wmm_alloc 直接改名为 malloc:库自身需要
 * 稳定的 wmm_* 符号;--wrap 是应用侧可选的链接方式,不该改变库的
 * 符号布局。单独一个文件也让"哪些入口被接管"一眼可见,将来补
 * strdup 一类入口时改动集中。
 *
 * 边界(必须让使用者知道):--wrap 只改写本可执行文件里的引用,
 * 不改写 glibc 或第三方动态库内部的 malloc——动态库已经链接完成,
 * 它们的调用不经过本程序的链接期改写。strdup/getline/asprintf 这类"libc
 * 内部申请、把指针交给应用"的函数仍由 glibc 分配,应用若用被接管
 * 的 free 去释放,就是跨分配器释放——本库按野指针拒绝,glibc 侧
 * 则会损坏自己的堆。使用 --wrap 的程序要避开这类函数,或改用
 * wmm_alloc 自行复制。
 *
 * 编译期约束:Makefile 的 -fno-builtin-malloc 不能去掉。编译器若把
 * malloc/free 识别成内建函数,可能直接对折或改成自己的调用,链接期
 * 改写就落空了。
 */
#include "wmm.h"

void *__wrap_malloc(size_t size)
{
    return wmm_alloc(size);
}

void __wrap_free(void *p)
{
    wmm_free(p);
}

void *__wrap_calloc(size_t n, size_t size)
{
    return wmm_calloc(n, size);
}

void *__wrap_realloc(void *p, size_t size)
{
    return wmm_realloc(p, size);
}

int __wrap_posix_memalign(void **memptr, size_t alignment, size_t size)
{
    return wmm_posix_memalign(memptr, alignment, size);
}

void *__wrap_aligned_alloc(size_t alignment, size_t size)
{
    return wmm_aligned_alloc(alignment, size);
}
