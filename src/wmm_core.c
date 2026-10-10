/* wmm_core.c —— slab/freelist/bin 状态机/页表登记/主路径/对齐分配
 * (M3/M4/M5,见 doc/tasks.md §5~§7)
 *
 * 手写起点。按序完成:M3 slab 建立与 region 分配/释放、bin 状态机;
 * M4 size2index + 主路径(alloc/free/realloc/usable_size)+ free 反查;
 * M5 对齐分配三档。
 */
