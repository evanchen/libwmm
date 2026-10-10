/* wmm_phys.c —— 物理层:extent 地址序链/carve/合并/retained/dirty/purge
 * (M6/M10,见 doc/tasks.md §8/§12)
 *
 * 手写起点。M6:extent 结构 + 全量地址序双向链 + carve(MAP_FIXED 于堆区内)
 * + 合并 + 大对象路径;M10:purge 后台线程 + MADV_DONTNEED/FREE。
 */
