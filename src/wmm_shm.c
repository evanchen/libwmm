/* wmm_shm.c —— 共享内存监控协议(M12,见 doc/tasks.md §14)
 *
 * 手写起点。固定 ABI 布局(magic/version、seqlock、快照、TOP N、事件环),
 * 后台线程每 1s 发布;监控进程只读映射 + seqlock 读端。
 */
