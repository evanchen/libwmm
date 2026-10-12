/* wmm_wrap.c —— 链接期接管 malloc
 *
 * 职责:提供 __wrap_malloc/free/calloc/realloc/posix_memalign/
 * aligned_alloc,全部转调本库的同义 API。服务以
 * -Wl,--wrap=malloc,... 链接后,自身代码的 malloc 流量全部
 * 改道本库,而第三方动态库不受影响——对"自控代码 + 少量第三方"
 * 的后端服务是最省事的集成方式。
 */
