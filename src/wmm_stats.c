/* wmm_stats.c —— 计数/快照/事件日志(M11,见 doc/tasks.md §13)
 *
 * 手写起点。L1 线程计数(快路径,非原子)+ bin 锁内 live 单点(G2)+
 * 快照合并 + wmm_event_t 无锁事件环。
 */
