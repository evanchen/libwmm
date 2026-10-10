#!/usr/bin/env python3
# gen_sc.py —— 尺寸类生成器(M1,见 doc/tasks.md §3)
#
# 手写起点:忠实移植 jemalloc sc.c 的 size_classes() 与 slab_size() 算法,
# 生成 src/wmm_sc.h(bin_info[36] + wmm_lookup_tab[513]),
# 输出必须与 doc/allocator_build_guide.md §4.2 逐行一致(SC1/SC3)。
