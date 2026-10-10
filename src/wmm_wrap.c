/* wmm_wrap.c —— 链接期接管(M15.2,见 doc/tasks.md §17)
 *
 * 手写起点。__wrap_malloc/free/calloc/realloc/posix_memalign/aligned_alloc,
 * 转调 wmm_* 显式 API;G3 归属审计。
 */
