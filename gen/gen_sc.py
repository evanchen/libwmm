#!/usr/bin/env python3
# gen_sc.py —— 尺寸类生成器
#
# 忠实移植 jemalloc sc.c 的 size_classes() 与 slab 页数算法,生成
# src/wmm_sc.h(尺寸类表 + 查表数组)。用生成器而不是手抄常量:
# 36 个类 × 4 个参数,手抄必错;算法 + 断言一次写对,表永远自洽。
# 脚本自带断言,生成的表须与 jemalloc 默认值逐行一致才算成功。
