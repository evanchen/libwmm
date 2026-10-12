/* wmm_core.c —— 分配主路径
 *
 * 职责:尺寸类查表;slab 的建立与对象取/还(空闲链);bin 状态机
 * (热 slab 满了挂链、全空归还);页表登记与 free 时的反向查找;
 * wmm_alloc/free/realloc/calloc/usable_size 与对齐分配的实现。
 *
 * 层次关系:本文件只管"对象 ←→ slab/bin"的世界;页从哪来、还给
 * 谁、相邻块怎么合并是 wmm_phys.c 的事,两者以 extent 为交接单位。
 */
