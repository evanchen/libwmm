/* wmm_prof.c —— 调用点采样(M13,见 doc/tasks.md §15)
 *
 * 手写起点。frame-pointer 栈遍历(-fno-omit-frame-pointer)、线程环形缓冲、
 * 慢路径计数触发、后台聚合 TOP N。
 */
