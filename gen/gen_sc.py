#!/usr/bin/env python3
# gen_sc.py —— 尺寸类生成器
#
# 忠实移植 jemalloc sc.c 的 size_classes() 与 slab 页数算法,生成
# src/wmm_sc.h(wmm_bin_info[36] + wmm_lookup_tab[513])。
# 为什么用生成器而不是手抄常量:36 类 × 4 参数手抄必错;算法加断言
# 一次写对,表永远自洽,生成器本身就是规格的可执行证明。
#
# 算法出处(与本项目所有 jemalloc 引用一致,指其发布源码):
#   size_classes():  sc.c   —— 36 个小对象类的推导
#   slab 页数 LCM:   sc.c 的 sc_data_update_slab —— pgs=LCM(PAGE,reg)/PAGE
#   lookup 表:       sz.c/sz.h —— 4096 以下 (size+7)>>3 直接查表
#
# 用法:python3 gen_sc.py  (在 gen/ 目录下运行,输出写到 ../src/wmm_sc.h)
# 生成器内置四重断言,任一失败即拒绝输出:
#   1. 36 类的 (reg_size, pgs) 与 jemalloc 默认表逐行一致;
#   2. pgs == LCM(PAGE, reg_size)/PAGE 且 1<=pgs<=7;
#   3. 查表结果对每个 size 都是"最小上界类"。

import math
from fractions import Fraction  # noqa: F401  (仅文档提及;LCM 用 math.lcm)

# ---- 参数:Linux x86-64 默认(与 jemalloc 默认配置一致) ----
LG_QUANTUM   = 4    # 16B 对齐量子
LG_TINY_MIN  = 3    # 最小对象 8B
LG_PAGE      = 12   # 4K 页
LG_NGROUP    = 2    # 每组 4 个类
LG_MAX_LOOKUP = 12  # 查表上限 4096
QUANTUM      = 1 << LG_QUANTUM
PAGE         = 1 << LG_PAGE
NGROUP       = 1 << LG_NGROUP
NBINS        = 36   # 小对象类数(裁剪决策:与 jemalloc 默认一致)


def size_classes():
    """推导 36 个小对象类,返回 [(reg_size, lg_delta), ...]。

    分三段,对应 jemalloc sc.c 的构造:
      tiny 段:   8B(仅 1 个,QUANTUM 以下)
      pseudo 段: 16/32/48/64(k*QUANTUM,k=1..4,即 NGROUP 个)
      regular 段: 每组 NGROUP 个,基数翻倍:
                  组基数 B: B+δ, B+2δ, B+3δ, B+4δ(=2B),δ = B/4
                  通用式 size = (1<<lg_base) + ndelta*(1<<lg_delta)
    pgs 不在此算(见 slab_pages()),但与 reg_size 一起最终成表。
    """
    classes = []

    # tiny 段:1<<LG_TINY_MIN .. QUANTUM/2,只有 8B 一个
    classes.append(1 << LG_TINY_MIN)

    # pseudo 段:k*QUANTUM,k=1..NGROUP(16,32,48,64)
    for k in range(1, NGROUP + 1):
        classes.append(k * QUANTUM)

    # regular 段:从 QUANTUM*NGROUP=64 开始,每组 4 个,基数翻倍。
    # 36 = 1(tiny) + 4(pseudo) + 31(regular),31 不是 NGROUP 的倍数,
    # 最后一组只填到 NBINS 为止(对照 jemalloc:其最后一个 regular 组
    # 同样只填到 NBINS 收尾,多出的类位置转为大对象起点)。
    lg_base = LG_QUANTUM + LG_NGROUP          # lg(64) = 6
    while len(classes) < NBINS:
        lg_delta = lg_base - LG_NGROUP        # δ = base/4
        for ndelta in range(1, NGROUP + 1):
            if len(classes) == NBINS:
                break
            classes.append((1 << lg_base) + ndelta * (1 << lg_delta))
        lg_base += 1                          # 组基数翻倍:64→128→256→…

    assert len(classes) == NBINS, f"期望 {NBINS} 类,得到 {len(classes)}"
    return classes[:NBINS]


def slab_pages(reg_size):
    """slab 页数 = LCM(PAGE, reg_size) / PAGE。

    为什么取 LCM:要让 pgs*PAGE == nregs*reg_size 恰好整除、页内无
    碎料。数学上 pgs = reg_size / gcd(PAGE, reg_size),且由于
    reg_size ≤ 4*PAGE 且 4|PAGE,gcd 只能是 2^k 形式,故 pgs ∈ {1,3,5,7}
    (reg 的奇数因子决定)或 1/2 页对齐的幂次情形;恒 1<=pgs<=7。
    """
    pgs = math.lcm(PAGE, reg_size) // PAGE
    assert 1 <= pgs <= 7, f"reg_size={reg_size} 的 pgs={pgs} 超出 1..7"
    return pgs


def gen_lookup_tab(classes):
    """wmm_lookup_tab[513]:tab[s] 给出 size=(s<<3) 的最小上界类编号。

    索引方式 (size+7)>>3:8B 粒度向上取整,与 jemalloc sz.h 一致——
    size 1..7 都归到 8B 类。表只覆盖到 4096(类 28),更大的 size
    走公式路径,不进表。
    """
    tab = [0] * 513
    for s in range(1, 513):
        size = s << 3
        idx = None
        for i, rs in enumerate(classes):
            if rs >= size:
                idx = i
                break
        assert idx is not None, f"size={size} 找不到覆盖类?"
        tab[s] = idx
    return tab


# ---- jemalloc 默认配置的权威值(抄自 sc.c 推导结果,断言用) ----
JEMALLOC_TABLE = [
    # (reg_size, pgs)
    (8,     1), (16,    1), (32,    1), (48,    3), (64,    1),
    (80,    5), (96,    3), (112,   7), (128,   1), (160,   5),
    (192,   3), (224,   7), (256,   1), (320,   5), (384,   3),
    (448,   7), (512,   1), (640,   5), (768,   3), (896,   7),
    (1024,  1), (1280,  5), (1536,  3), (1792,  7), (2048,  1),
    (2560,  5), (3072,  3), (3584,  7), (4096,  1), (5120,  5),
    (6144,  3), (7168,  7), (8192,  2), (10240, 5), (12288, 3),
    (14336, 7),
]


def main():
    classes = size_classes()

    # 断言 1:与 jemalloc 默认表逐行一致(值与页数)
    for i, (reg, pgs) in enumerate(JEMALLOC_TABLE):
        assert classes[i] == reg, \
            f"类 {i}: 期望 {reg},推导得 {classes[i]}"
        assert slab_pages(reg) == pgs, \
            f"类 {i}({reg}B): 期望 pgs={pgs},LCM 算得 {slab_pages(reg)}"

    # 断言 2:表内无重复、严格递增(二分与查表的前提)
    assert all(b > a for a, b in zip(classes, classes[1:])), "类大小必须严格递增"

    # 断言 3:查表结果是"最小上界"(每个类的上界恰好是下一个类减 reg)
    tab = gen_lookup_tab(classes)
    for i, rs in enumerate(classes):
        if rs > 4096:
            break
        # 大小恰好为 rs 的请求必须落在类 i
        s = rs >> 3
        assert tab[s] == i, f"size={rs} 查表得类 {tab[s]},期望 {i}"
        # 大小为 rs+1 的请求必须落在下一个类(最小上界)
        if rs < 4096:
            s2 = (rs + 1 + 7) >> 3
            assert tab[s2] == i + 1, \
                f"size={rs+1} 查表得类 {tab[s2]},期望 {i+1}(最小上界)"

    # 关键点抽查(文档勘误后):tab[1]=0(8B), tab[2]=1(16B),
    # tab[3]=2(17..24 归 32), tab[512]=28(4096)
    assert tab[1] == 0 and tab[2] == 1 and tab[3] == 2 and tab[512] == 28

    # 断言 4:属性抽样(步进 7 打散对齐边界)——对每个抽样 size:
    #   最小上界类的 reg_size ≥ size(不欠分配);
    #   size ≤ 4096 时,查表结果与全扫描结果一致(两条路径不矛盾)。
    # 这正是将来 C 侧 size2index 属性测试的生成期版本。
    for size in range(1, 14336 + 1, 7):
        want = next(i for i, rs in enumerate(classes) if rs >= size)
        if size <= 4096:
            assert tab[(size + 7) >> 3] == want, \
                f"size={size}: 查表类 {tab[(size + 7) >> 3]} != 全扫描类 {want}"

    # ---- 全部断言通过,生成头文件 ----
    lines = []
    lines.append("/* wmm_sc.h —— 尺寸类表 + 查表数组(由 gen/gen_sc.py 生成,勿手改)")
    lines.append(" *")
    lines.append(" * 内容:wmm_bin_info[36](每类 reg_size/pgs/slab_size/nregs)")
    lines.append(" * 与 wmm_lookup_tab[513](4096 以下请求一步查表定类)。")
    lines.append(" * 数值与 jemalloc 默认配置逐行一致;生成器内置四重断言")
    lines.append(" * (推导值 vs jemalloc 表、LCM 页数、查表最小上界)保证自洽。")
    lines.append(" */")
    lines.append("#ifndef WMM_SC_H_INCLUDED")
    lines.append("#define WMM_SC_H_INCLUDED")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("/* 防御:有人改了 wmm_bin_info_t 的字段类型时,本表与其")
    lines.append(" * 注释/假设全部失效——编译期拦下,而不是静默错位。 */")
    lines.append("_Static_assert(sizeof(wmm_bin_info_t) == 16,")
    lines.append("              \"bin_info 布局变了,重新审视生成器\");")
    lines.append("")
    lines.append("/* 每个尺寸类的只读参数。字段含义见 wmm_internal.h 的 wmm_bin_info_t;")
    lines.append(" * 类型与它一致(全 uint32_t,共 16B)。 */")
    lines.append("static const wmm_bin_info_t wmm_bin_info[WMM_NBINS] = {")
    for i, reg in enumerate(classes):
        pgs = slab_pages(reg)
        slab_size = pgs * PAGE
        nregs = slab_size // reg
        lines.append(f"    {{ {reg}, {pgs}, {slab_size}, {nregs} }},"
                     f"  /* [{i:2d}] {reg}B, {pgs}页 */")
    lines.append("};")
    lines.append("")
    lines.append("/* 4096 以下请求的查表数组:索引 (size+7)>>3(8B 粒度向上取整),")
    lines.append(" * 值 = 最小上界类编号。tab[0] 恒 0 但不会用到(size>=1)。 */")
    lines.append("static const uint8_t wmm_lookup_tab[513] = {")
    row = []
    for s in range(513):
        row.append(str(tab[s]))
        if len(row) == 16:
            lines.append("    " + ", ".join(row) + ",")
            row = []
    if row:
        lines.append("    " + ", ".join(row) + ",")
    lines.append("};")
    lines.append("")
    lines.append("#endif /* WMM_SC_H_INCLUDED */")

    import os
    out = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                        "..", "src", "wmm_sc.h"))
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"OK: {len(classes)} bins -> {out}")
    print(f"    lookup: tab[1]=0(8B) tab[2]=1(16B) tab[3]=2(17..24→32B) tab[512]=28(4096)")


if __name__ == "__main__":
    main()
