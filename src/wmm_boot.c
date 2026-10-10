/* wmm_boot.c —— 启动:VA 堆区、页表、bin 初始化(M2,见 doc/tasks.md §4)
 *
 * 手写起点。只做三件事:固定 VA 堆区 mmap(PROT_NONE)、g_pte 页表、
 * g_bins[36] 初始化。boot 过程禁止调用自身分配函数(I4 自举检查)。
 */
