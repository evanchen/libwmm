/* wmm_wrap.c —— ld --wrap 接管入口:七个分配调用转发到 wmm_* 公共 API */

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

void *__wrap_reallocarray(void *p, size_t n, size_t size)
{
    return wmm_reallocarray(p, n, size);
}

int __wrap_posix_memalign(void **memptr, size_t alignment, size_t size)
{
    return wmm_posix_memalign(memptr, alignment, size);
}

void *__wrap_aligned_alloc(size_t alignment, size_t size)
{
    return wmm_aligned_alloc(alignment, size);
}
