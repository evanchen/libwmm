/* wmm_tcache.c —— cache_bin 快路径/批量填充/自适应 GC/线程退出
 * (M8/M9,见 doc/tasks.md §10/§11)
 *
 * 手写起点。采用 head 索引版 cache_bin(wmm_internal.h 已定);
 * M8 快路径 + fill/flush + arena 批量填充;M9 GC 公式 + wmm_thread_exit(G5)。
 */
