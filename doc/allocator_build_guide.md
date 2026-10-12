# 小而精的 Linux 内存管理库 —— 实现指南(精确规格版)

> **定位**:内嵌于后端服务的自研内存管理库。Linux-only、代码量目标 2~3k 行、
> 以 **实时可观测 + 问题定位** 为核心目标。
> 本版为**精确规格**:每个子系统给出数据结构定义、算法(伪代码)、不变量、
> 关键数值表,并标注对应 jemalloc 源码位置(`src/*.c` / `include/jemalloc/internal/*.h`,均为本仓库实测)。
> 学习对象是 jemalloc(见 `DESIGN.md` / `malloc_flow.html`),但**刻意裁剪**。

---

## 目录

1. [目标 → 需求与设计原则](#1-目标--需求与设计原则)
2. [范围裁剪:精确到模块与数值](#2-范围裁剪精确到模块与数值)
3. [架构总览与全局不变量](#3-架构总览与全局不变量)
4. [尺寸类系统(规格)](#4-尺寸类系统规格)
5. [slab 与 region(规格)](#5-slab-与-region规格)
6. [反向映射:页表(规格)](#6-反向映射页表规格)
7. [tcache(规格)](#7-tcache规格)
8. [arena 与 bin(规格)](#8-arena-与-bin规格)
9. [物理层:extent 合并与缓存(规格)](#9-物理层extent-合并与缓存规格)
10. [可观测性(规格,核心章节)](#10-可观测性规格核心章节)
11. [实时监控:共享内存协议(规格)](#11-实时监控共享内存协议规格)
12. [集成到后端服务(规格)](#12-集成到后端服务规格)
13. [问题定位 playbook](#13-问题定位-playbook)
14. [路线图:文件清单与验收](#14-路线图文件清单与验收)
15. [Linux/后端服务特化坑](#15-linux后端服务特化坑)
16. [验收标准](#16-验收标准)

---

## 1. 目标 → 需求与设计原则

| 目标 | 翻译成设计需求 | 落点章节 |
|---|---|---|
| 小而精 | 范围裁剪;单文件单子系统;代码量 2~3k 行 | §2 |
| 只在 Linux | `__thread`、`mmap/madvise`、`MADV_FREE/DONTNEED/HUGEPAGE`、`sched_getcpu`、`-Wl,--wrap` 放开用 | §9 §12 |
| 后端服务内嵌 | 稳定性/可控性优先;fork、线程退出、信号、dlopen 要处理 | §12 §15 |
| 定制内存管理 | 策略参数化:尺寸类表、缓存水位、GC 阈值、告警阈值 | §4 §7 §10 |
| **实时监控** | 统计子系统从第一天设计;埋点不阻塞分配路径 | §10 §11 |
| **定位问题** | 调用点采样、泄漏检测、guard/quarantine、事件日志 | §10 §13 |

**三条设计原则(本规格所有决策的判据)**:

1. **一个子系统一个文件**,砍不掉的才留下;
2. **快路径只做"读 + 一次非原子整数累加"**——无锁、无系统调用、无统计表查找;
3. **重功能(采样/guard/quarantine)默认关闭**,开启才有开销;所有监控数据都不阻塞分配路径。

---

## 2. 范围裁剪:精确到模块与数值

**先给出 jemalloc 的数量级,裁剪决策才有依据**(64 位 x86-64、4K 页、默认配置,全部实测):

| 量 | 值 | 出处 |
|---|---|---|
| 总尺寸类数 `SC_NSIZES` | **232** | `sc.h:198-200`(tiny 1 + pseudo 4 + regular 227) |
| 小对象(bin)数 `SC_NBINS` | **36** | `sc.h:237-248` |
| 最小大对象 `SC_LARGE_MINCLASS` | **16384** | `sc.h:278`(`1 << (LG_PAGE+SC_LG_NGROUP)`) |
| 最大小对象 `SC_SMALL_MAXCLASS` | **14336** | `sc.h:263-269` |
| lookup 查表上限 | **4096** | `sc.h:259-260` |
| 页面倍数尺寸类 `SC_NPSIZES` | **199** | `sc.h:226-231` |
| 对齐量子 `QUANTUM` | **16** | `quantum.h`(LG_QUANTUM=4) |
| 最小尺寸 `SC_LG_TINY_MIN` | **3 (8B)** | `sc.h:174` |
| 每组类数 `SC_NGROUP` | **4** | `sc.h:173,185` |

### ✅ 保留

| 子系统 | jemalloc 出处 | 你的实现 |
|---|---|---|
| 尺寸类表 | `sc.h` / `sc.c` | 生成器脚本 + 常量表(§4) |
| slab + region | `bin_info.h` / `bin.c` | freelist 版(§5) |
| 反向映射 | `emap.h`(rtree) | **页表**(§6),比 rtree 简单一个量级 |
| tcache | `cache_bin.h` / `tcache.c` | 无锁栈 + 批量 + 自适应 GC(§7) |
| arena + bin | `bin.h` / `arena.c` | 全局单 arena + bin 锁(§8) |
| extent 合并 + 缓存 | `extent.c` / `pac.c` | retained/dirty 两级(§9) |
| **统计/观测** | `stats.*` / `thread_event` | §10(核心) |

### ⏸️ 推迟(第一版不做,但接口预留)

| 子系统 | jemalloc 做法 | 替代 |
|---|---|---|
| decay 平滑 | backlog[64]+定点 | 后台线程阈值触发 purge(§9) |
| 多 arena / per-CPU | per-CPU 绑定 | 全局 arena + tcache |
| binshard | bin 按线程分片 | 预留 `shard` 字段 |
| HPA / 大页 | eden+psset | 不做;THP 靠 `madvise` 提示 |
| prof 家族 | 7+ 文件 | §10.3 精简调用点采样 |
| 复杂自适应 tcache | `tcache_ncached_target.h` 全套 | §7.5 保留核心公式 |

### ❌ 砍掉

跨平台层(os/*)、USDT/probes、符号重命名体系(rename/mangle)、mpsc/seq、edata 位域压缩(直接用结构体,换可读性)。

**裁剪验收**:某个子系统若解释不清"防什么、开销多大、砍了会怎样",就砍。

---

## 3. 架构总览与全局不变量

```
┌───────────────────────────────────────────────────────────┐
│ 应用: wmm_alloc/wmm_free/wmm_realloc 或 -Wl,--wrap(§12)      │
├───────────────────────────────────────────────────────────┤
│ 快路径: size→class(查表) → cache_bin 弹/压栈 → 返回        │
│         无锁无 syscall;仅 thread 计数 +=(一次)             │
├───────────────────────────────────────────────────────────┤
│ 慢路径: tcache 空/满 → bin 锁 → slabcur→nonfull→新 slab    │
│          批量填充/刷出(单次持锁)                           │
│ 大对象:  size≥16K → 物理层直接分配/释放                    │
├───────────────────────────────────────────────────────────┤
│ 物理层: 页表反向映射 + 地址序合并 + retained/dirty          │
│          mmap / madvise(MADV_DONTNEED|FREE|HUGEPAGE)       │
├───────────────────────────────────────────────────────────┤
│ 观测层: 线程计数 | per-class live | 调用点采样 | 泄漏检测   │
│          guard/quarantine | 事件日志 → 共享内存(§11)       │
└───────────────────────────────────────────────────────────┘
```

**全局不变量(设计正确性的判据,写进每层注释)**:

- **I1 对齐**:slab 基址页对齐;`reg_size % 16 == 0`(8 类除外)→ 所有返回值满足 `wmm_alloc` 的对齐契约(§4.4);
- **I2 单点**:per-class `live` 计数**只在 bin 锁内更新**(fill/flush/直接分配),tcache 从不碰它 → `live` 恒一致(§10.2);
- **I3 归属**:同一指针的分配与释放必须走同一分配器(`--wrap` 下同样成立,§12);
- **I4 自举**:页表、统计、采样缓冲均来自 `mmap` 专用区,**绝不用自分配器分配自己的元数据**;
- **I5 无泄漏**:线程退出时 tcache 全部归还 arena;fork 后子进程重建全部锁状态。

---

## 4. 尺寸类系统(规格)

### 4.1 生成算法(忠实移植 `sc.c:84-253`)

参数(Linux x86-64 默认):`LG_QUANTUM=4`、`SC_LG_TINY_MIN=3`、`SC_LG_NGROUP=2`(→NGROUP=4)、`LG_PAGE=12`、`SC_LG_MAX_LOOKUP=12`。

```
尺寸类集合分四段:
1. tiny(1 个):       8                      (1<<LG_TINY_MIN .. 1<<(LG_QUANTUM-1))
2. pseudo(4 个):     16, 32, 48, 64         (k*QUANTUM, k=1..4; 即 1<<LG_QUANTUM 起,步进 QUANTUM)
3. regular(每组 4 个,base 每次翻倍):
   组 base B(从 64 起):  B+1δ, B+2δ, B+3δ, B+4δ=2B,  其中 δ = B/4
4. 大对象:  ≥ 2^(LG_PAGE+NGROUP) = 16K,页面倍数量化(§4.5)

通用公式: size(class) == (1<<lg_base) + ndelta*(1<<lg_delta)
  - regular 组:  lg_delta = lg_base - SC_LG_NGROUP, ndelta ∈ [1,4]
  - tiny/pseudo 按 sc.h:148-160 的特例编码
```

### 4.2 完整尺寸类表(脚本按 `sc.c` 逐行移植生成,已验证)

| idx | size | slab页数 pgs | nregs/slab | psz | idx | size | pgs | nregs | psz |
|---|---|---|---|---|---|---|---|---|---|
| 0 | 8 | 1 | 512 | | 18 | 768 | 3 | 16 | |
| 1 | 16 | 1 | 256 | | 19 | 896 | 7 | 32 | |
| 2 | 32 | 1 | 128 | | 20 | 1024 | 1 | 4 | |
| 3 | 48 | 3 | 256 | | 21 | 1280 | 5 | 16 | |
| 4 | 64 | 1 | 64 | | 22 | 1536 | 3 | 8 | |
| 5 | 80 | 5 | 256 | | 23 | 1792 | 7 | 16 | |
| 6 | 96 | 3 | 128 | | 24 | 2048 | 1 | 2 | |
| 7 | 112 | 7 | 256 | | 25 | 2560 | 5 | 8 | |
| 8 | 128 | 1 | 32 | | 26 | 3072 | 3 | 4 | |
| 9 | 160 | 5 | 128 | | 27 | 3584 | 7 | 8 | |
| 10 | 192 | 3 | 64 | | 28 | 4096 | 1 | 1 | ★ |
| 11 | 224 | 7 | 128 | | 29 | 5120 | 5 | 4 | |
| 12 | 256 | 1 | 16 | | 30 | 6144 | 3 | 2 | |
| 13 | 320 | 5 | 64 | | 31 | 7168 | 7 | 4 | |
| 14 | 384 | 3 | 32 | | 32 | 8192 | 2 | 1 | ★ |
| 15 | 448 | 7 | 64 | | 33 | 10240 | 5 | 2 | |
| 16 | 512 | 1 | 8 | | 34 | 12288 | 3 | 1 | ★ |
| 17 | 640 | 5 | 32 | | 35 | 14336 | 7 | 2 | |

- **36 个 bin**,最大 14336;`psz ★` 表示该类是页面的整数倍(4096/8192/12288);
- **nregs=1 的类(4096/8192/12288)** 是特例:slab 永不进入 nonfull 堆(`bin.c:202`),满/空直接切换;
- **大对象类**:16K, 20K, 24K, 28K, 32K, 40K, 48K, 56K, 64K, 80K, …(页面倍数,步长随组翻倍),共 199 个页面倍数类(`SC_NPSIZES`)。

### 4.3 slab 页数公式(`sc.c:23-51`)

```
pgs = LCM(PAGE, reg_size) / PAGE
  等价循环: 从 1 页起累加,直到 页数*页大小 == 区域数*区域大小(完美分割)
  最多迭代 2*NGROUP-1 = 7 次(所以 pgs ≤ 7;唯一例外见下)
```

规律(表内可见):`reg_size` 含奇数因子 3 → 3 页、5 → 5 页、7 → 7 页;2 的幂 → 1 页;`8192` 是 LCM=2 页(2 的幂但大于一页)。
> 注:jemalloc 用 bitmap 管理 region,额外受 `BITMAP_BITS` 上限约束(`sc.c:276`);**你用 freelist 则无此约束,pgs 恒为 LCM 精确值**。

### 4.4 查表与对齐保证

**size → index**(`sz.h:164-225`):

```
wmm_size2index(size):
  if size <= 4096:  return tab[(size + 7) >> 3]     // 静态 uint8_t tab[513],512 项有效,idx 1..512
                                                    // 覆盖 8..4096(29 个 bin),一次访存
  if size <= 14336: return 公式/二分(36 项小表)       // 冷路径,怎么快怎么来
  else:            return 大对象路径(§4.5)
```

- lookup 表由生成脚本导出,`tab[1]=0(8B) … tab[512]=28(4096)`;尺寸 1..7 上取整到 8;
- **对齐保证(免费)**:slab 基址页对齐(§5),region 偏移 = `regind * reg_size`;除 8B 类外所有 `reg_size % 16 == 0` → 返回值 **16 字节对齐**(x86-64 `max_align_t`);8B 类天然 8 对齐。**这就是为什么尺寸类表本身就是对齐方案**——不需要任何对齐分配路径(特殊 `aligned_alloc` 请求见 §12)。

### 4.5 大对象量化(`sz.h:63-114` 页面倍数路径)

```
wmm_psz2u(size):  # 向上取整到页面倍数尺寸类
  x = lg_floor((size<<1)-1)
  lg_delta = (x < LG_PAGE+NGROUP+1) ? LG_PAGE : x - NGROUP - 1
  return (size + (1<<lg_delta) - 1) & ~((1<<lg_delta)-1)
```

大对象按页面倍数对齐、整页分配,`usize - size < 页面倍数步长 ≤ 8K`。

---

## 5. slab 与 region(规格)

### 5.1 常量表(仿 `bin_info_t`,`bin_info.h:27-45`)

```c
typedef struct {
    uint32_t reg_size;    /* 尺寸类大小(§4.2 表) */
    uint32_t pgs;         /* slab 页数(§4.3) */
    uint32_t slab_size;   /* pgs * PAGE */
    uint32_t nregs;       /* slab_size / reg_size */
} bin_info_t;             /* 只读,全局数组 bin_info[36],生成器导出 */
```

### 5.2 slab 布局(freelist 版)

```
page 0           region 0   region 1        ...     region nregs-1
┌──────────────┬─────────────────────────────────────────────┐
│ slab_header  │            regions(对象体,无任何头部)        │
│ (1 页内)     │                                             │
└──────────────┴─────────────────────────────────────────────┘
```

```c
typedef struct {
    uint32_t   szind;      /* 尺寸类索引 */
    uint32_t   nfree;      /* 空闲区域数;0=满,nregs=全空 */
    uint32_t   free_head;  /* 空闲链头(region 索引) */
    uint32_t   pad;
    void      *addr;       /* slab 基址(页对齐) */
    /* 链入 arena 的桶(bin): */
    struct slab_s *next_full, *prev_full;  /* full 双向链 */
    struct slab_s *left, *right;           /* nonfull 地址序堆(§8.3) */
} slab_header_t;
```

**空闲链**:空闲 region 的**首字存下一个空闲 region 的索引**(侵入式单链表,零额外元数据)。

```
pop:  r = free_head; free_head = *(uint32_t*)(addr + r*reg_size); nfree--; return addr + r*reg_size
push: *(uint32_t*)p = free_head; free_head = (p-addr)/reg_size; nfree++
```

> 与 jemalloc 差异:jemalloc 用 bitmap(分配/释放 O(1) 且批量友好,`bin_slab_reg_alloc_batch` `bin.c:89-134`)。freelist 同样 O(1),代码少 200 行;**批量填充时从链头连续 pop 即可**。将来若想升级,`bin_info_t` 换成 `bitmap_info_t` 即可,上层接口不变。

### 5.3 不变量

- `nfree == nregs` → 全空(可还物理层);`nfree == 0` → 满(进 full 链);
- slab 只在一个 bin 的链表中(或为 slabcur),`addr` 页对齐;
- 每个 slab 有唯一所有者 bin;页表项永久指向该 slab_header(§6),直到 slab 归还前取消映射。

---

## 6. 反向映射:页表(规格)

`free(p)` 必须回答:哪个类、哪个 slab。

### 6.1 结构

```c
/* 8 字节项;页表本身用 mmap 专用区,自举(I4) */
typedef union {
    struct { uintptr_t tag : 1, slab_or_ext : (sizeof(uintptr_t)*8 - 1); };
    uintptr_t raw;
} pte_t;   /* tag=0 → 指向 slab_header_t*; tag=1 → 指向 extent_t* */

static pte_t *g_page_table;   /* 大小 = 堆地址空间页数,mmap 分配 */
```

- **小对象**:slab 的**每一页**的页表项都指向同一个 `slab_header_t`(8 字节,代价可接受)——`free(p)` 不需要知道对象落在第几页;
- **大对象**:extent 的**每一页**都登记同一 `extent_t*`(tag=1)——与 jemalloc emap 全页登记语义一致,`free(p)` 从任意页进入都能反查,归还时以 `extent->addr` 为基址整体回收;
- 开销:1GB 堆 = 256K 页 × 8B = **2MB**;用二级结构按需分配可降到常驻少量页(Phase 4 优化)。

### 6.2 算法

```c
// 小对象释放(慢路径入口,§7 快路径拦截大部分):
wmm_free_slow(slab_header_t *s, void *p):
  regind = (uintptr_t)(p - s->addr) / bin_info[s->szind].reg_size;   // 除法定值指令(仿 div 模块)
  lock(bin[s->szind]); s->nfree++; push(p); live[s->szind]--;
  // 状态机:全空→还物理层;由 full→nonfull 等见 §8.4
  unlock

// 分配慢路径也要用页表?不需要——slab 链已含全部对象
```

> jemalloc 用多级 rtree + TSD 两级缓存做同样的事(`emap.c:159` 边界登记),是**为省页表内存 + 快路径零锁**。后端服务堆一般 ≤ 数十 GB,页表 2MB/GB 完全可接受;这是"小而精"最典型的一次取舍。

---

## 7. tcache(规格)

### 7.1 cache_bin 结构(`cache_bin.h:88-135`,忠实移植,去掉 stash 等扩展)

```
 (低地址) |------stashed/预留------|------cached------|   (高地址)
          ^                        ^                 ^
     low_bound(全满位)          stack_head       empty(栈顶之上)
          栈向下生长
```

```c
typedef struct {
    void    **stack_head;   /* 非空时指向一个有效对象;空时==empty_pos */
    uint16_t low_bits_low_water;
    uint16_t low_bits_full;    /* 满水位(低16位) */
    uint16_t low_bits_empty;   /* 空水位(低16位),初始化后不变 */
    uint16_t ncached_max;
    uint64_t nrequests;        /* 本 bin 累计请求数(观测用) */
} cache_bin_t;                 /* 热路径只碰 stack_head + low_water 两个字段 */

// 快路径只比较"低16位指针"判空/判水位,避免整指针比较(sc.h/cache_bin.h 注释)
```

### 7.2 快路径分配(`cache_bin_alloc_impl`,`cache_bin.h:381-443`)

```c
void *wmm_tcache_alloc(cache_bin_t *b) {
    void *ret = *b->stack_head;                       // 可能读到空位,但无妨
    uint16_t head = (uint16_t)(uintptr_t)b->stack_head;
    if (head != b->low_bits_low_water) {              // 热路径:仅 1 次比较
        b->stack_head++;  return ret;
    }
    if (head != b->low_bits_empty) {                  // 越过水位线:更新水位
        b->stack_head++;
        b->low_bits_low_water = (uint16_t)(uintptr_t)b->stack_head;
        return ret;
    }
    return NULL;                                      // 空 → 慢路径填充
}

void wmm_tcache_free(cache_bin_t *b, void *p) {
    if ((uint16_t)(uintptr_t)b->stack_head == b->low_bits_full) { flush(b); }  // 满→刷出
    b->stack_head--; *b->stack_head = p;
}
```

### 7.3 批量填充/刷出(`cache_bin.h:647-698` 的语义)

- **填充** `fill(b, n)`:bin 必须为空;目标位置在 `empty_pos - n`;从 arena 一次取 n 个(§8.5),`stack_head = empty_pos - n`(不足 n 时整体 memmove 靠拢);
- **刷出** `flush(b, n)`:取栈底 n 个还给 arena(先回收**最旧**的,保持 first-fit 与缓存局部性——`cache_bin.h:613-621` 注释明确这是可测的优化);剩余 memmove 到栈底;
- `nrequests` 在批量时合并,避免逐对象原子操作。

### 7.4 ncached_max 公式(`tcache.c:717-739`)

```
ncached_max(szind):                                    // 仅小对象(szind 0..35)
  // 本库第一版不缓存大对象:jemalloc 的 20 槽(TCACHE_NSLOTS_LARGE)属于
  // "每个大对象类各一个 cache bin"的语境,剥离语境塞进单一混合槽,弹出
  // 规格无法保证匹配请求;szind ≥ 36 一律直走物理层。
  candidate = nregs(szind) << 1                       // TCACHE_LG_NSLOTS_MUL=1
  if candidate 奇数: candidate++
  return clamp(candidate, 20, 200)                    // SMALL_MIN/MAX(tcache.h:65-66)

示例: 类0(8B, 512regs) → 200;  类8(128B, 32regs) → 64;
     类20(1K, 4regs) → 20;     类28(4K, 1reg) → 20
```

tcache 总占用(每线程)≈ Σ ncached_max[i]×size[i],默认配置约 **几十~百 KB/线程**;这是 §15 线程数×缓存上限坑的数值依据。

### 7.5 自适应 GC(保留 `tcache_ncached_target.h` 核心公式)

GC 在 flush 时执行:读 `low_water`(=上次 GC 后栈顶最低水位),计算"本轮实际用量"。

```
target_max = ncached_max >> 1                    // 填充目标上限:半容量(最少 2)
used_since_gc = ncached - low_water
retain = used_since_gc + (used_since_gc >> 2)    // 保留 25% 余量
        || (used_since_gc==0 → 2)                // 完全没用 → 缩到最小
flush 掉  ncached - retain  个                    // tcache_ncached_retain_after_gc, h:48-66
下次 fill 量 nfill:  填满后 nfill <<= 1(封顶 target_max);GC 触发后 nfill >>= 1(≥2)
```

- 每次 GC 后 `low_bits_low_water = stack_head`;
- 效果:持续用 → 缓存水位涨到半容量;偶尔用 → 收缩到最小,把对象还给 arena。**这就是"缓存自适应"的全部,~40 行**。

### 7.6 线程退出

`__thread` 析构回调(或 `pthread_key`):对每个非空 bin 全量 `flush`;再归还 cache_bin 栈内存。**不做,后端服务线程池反复创建线程 = 每线程缓存泄漏**。

---

## 8. arena 与 bin(规格)

### 8.1 结构(`bin.h:23-50` 精简)

```c
typedef struct {
    slab_header_t *slabcur;            /* 当前热 slab */
    /* nonfull: 地址序堆(最低地址优先,保 first-fit), 仿 edata_heap */
    slab_header_t *nonfull_root;
    slab_header_t *full_head;          /* full 双向链 */
    uint64_t live, ndalloc, nslabs, curslabs;   /* 观测(§10) */
} bin_t;   /* 每尺寸类一个,共 36 个,全局 */

typedef struct { bin_t bins[36]; } arena_t;   /* 第一版:全局单 arena */
```

### 8.2 小对象分配慢路径(`bin_malloc_no_fresh_slab`,`bin.c:304-316`)

```c
void *bin_alloc(bin_t *b, szind_t i) {
    lock(b);
    if (b->slabcur == NULL || b->slabcur->nfree == 0) {
        if (b->slabcur != NULL)          full 链 += slabcur;           // bin.c:293-295
        b->slabcur = pop_lowest_addr(nonfull 堆);                       // bin_slabs_nonfull_tryget
        if (b->slabcur == NULL) {
            slab = 物理层取新 slab(§9); init header; b->slabcur = slab;  // bin_refill_slabcur
        }
    }
    r = pop(b->slabcur);
    b->live++;                          // I2:live 唯一更新点之一
    unlock(b);
    return r;
}
```

### 8.3 nonfull 地址序堆(`bin_lower_slab`,`bin.c:211-234`)

- 释放使 slab 从 full → nonfull 时,插入地址序堆;**分配永远优先最低地址** → 缓存局部性与回收友好;
- `slabcur` 换人规则:新 slab 地址更低时,旧 slabcur 降级入堆,新的成为 slabcur(`bin.c:220-227`)。

### 8.4 释放回 bin(慢路径,补全 §6.2 状态机)

```c
void bin_dalloc(bin_t *b, slab_header_t *s, void *p) {
    lock(b);
    s->nfree++; push(s, p);
    if (s->nfree == nregs) {           // 全空 → 归还物理层
        if (s == b->slabcur) b->slabcur = NULL;
        else 从 nonfull 堆移除;         // bin_dissociate_slab, bin.c:189-208
        unlock(b); 物理层回收(s, §9); return;
    }
    if (s 在 full 链) { 移除; lower_slab(b, s); }   // 由满转非满,进地址序堆
    unlock(b);
}
```

### 8.5 批量填充(慢路径核心,`arena_ptr_array_fill_small` `arena.c:736`)

```c
unsigned bin_fill(bin_t *b, szind_t i, unsigned want, void **out) {
    lock(b);
    unsigned n = 0;
    while (n < want) {
        if (b->slabcur == NULL || b->slabcur->nfree == 0) refill_slabcur(b, i);  // §8.2
        if (b->slabcur == NULL) break;                    // OOM
        while (n < want && b->slabcur->nfree > 0)
            out[n++] = pop(b->slabcur);                    // 从链头连续 pop(freelist 批量天然高效)
    }
    b->live += n;                                          // I2(fill 即分配;原稿误写 ndalloc+=n,勘误)
    unlock(b);
    return n;
}
```

**每线程 95% 的分配/释放被 §7 快路径拦截**,bin 锁只在批量路径出现——这是"全局单 arena"能成立的原因。

---

## 9. 物理层:extent 合并与缓存(规格)

### 9.1 结构

```c
typedef struct {
    void          *addr;      /* 页对齐基址 */
    size_t         npages;
    szind_t        szind;     /* 大对象类(页面倍数),或 SLAB */
    bool           dirty;     /* 内容仍驻留(仅 dirty/retained 链表用) */
    /* 合并用: */
    struct extent_s *prev, *next;   /* 按地址有序双向链 */
    uint32_t       pte_tag;   /* 首尾页页表 tag(§6) */
} extent_t;                    /* 大对象:extent 头单独页;slab:slab_header 即 extent */

enum { EXT_FREE_RETAINED, EXT_FREE_DIRTY, EXT_INUSE };
static extent_t *g_ext_addr_root;   /* 全部 extent 按地址序 */
static list retained; static list dirty;   /* 空闲两级 */
```

### 9.2 分配优先序(仿 `extent_recycle` `extent.c:633`)

```
新 slab/大对象取 extent:   retained(上次回收,内容已废弃)
                        → dirty(可能仍在内存)
                        → mmap 新映射(pages_map, pages.c:77)
```

### 9.3 合并(`extent_coalesce` 语义)

释放 extent E 时,查地址相邻页的页表项:
- 邻居是空闲 extent → 取下合并(`npages` 相加,从链表移除);可连续向两侧合并;
- **不做合并 = 长期运行必然碎成页渣**;这是物理层最重要的功能,必须 Phase 1 就做。

### 9.4 简化回收策略(替代 decay 平滑算法)

```
后台线程每 T=1s 检查:
  if dirty 空闲总量 > dirty_max(如 8MB) 或 某个 extent 空闲超过 T_idle:
     对目标 extent: madvise(addr, size, MADV_DONTNEED)   // 内容废弃,立即返还能耗低
     并移入 retained
  后台线程同时做:观测聚合(§11)
```

Linux 特化:
- `MADV_DONTNEED`:立即释放物理页(回收及时,代价是再访问会缺页);
- `MADV_FREE`:懒回收(Linux≥4.5,回收延迟但无缺页代价)——**后端服务常驻、RSS 敏感时推荐 DONTNEED;延迟敏感时用 FREE**;
- `MADV_HUGEPAGE`:大 slab/大对象可提示 THP(§15 坑 9)。

### 9.5 大对象路径(`large_malloc` `large.c:17` → `pa_alloc` `pa.c:125` → `pac_alloc` `pac.c:155`)

```
wmm_alloc(≥16K):
  usize = wmm_psz2u(size); npages = usize/PAGE
  extent = recycle(§9.2); 页表登记首尾页(tag=1 → extent)
  return extent->addr
wmm_free(大对象):
  页表首项 → extent; 页表注销; 合并(§9.3); 入 dirty/retained
```

---

## 10. 可观测性(规格,核心章节)

### 10.1 计数器与更新点(回答"数据在哪写、谁保证一致")

| 层 | 字段 | 更新位置 | 成本 | 一致性 |
|---|---|---|---|---|
| L1 每线程 | `t_alloc_bytes, t_alloc_cnt, t_free_bytes, t_free_cnt` | **快路径每次**分配/释放 | 非原子累加 ~1ns | 快照时合并,允许瞬时偏差 |
| L2 per-class | `live[i], ndalloc[i], curslabs[i]` | **bin 锁内**(§8.2/§8.4/§8.5) | 每批一次 | **恒一致**(I2,单写者) |
| L2 大对象 | `live_large, live_large_bytes` | 大对象慢路径 | 原子或锁内 | 恒一致 |
| L3 汇总 | `active_bytes, rss_est, high_water` | 物理层每次 mmap/madvise/munmap | 低频 | 恒一致 |

**I2 的完整陈述(勘误版)**:`live[i]` 只在 bin 锁内、且只在 slab pop/push 处修改:fill 批量 pop 加 n、flush 批量 push 减 n、慢路径直接分配/释放 ±1。因此 **live = 应用持有 + tcache 缓存中**(jemalloc 同语义:fill 计入 nmalloc,flush 计入 ndalloc)。**只要这条成立,`Σ live[i]*size[i] == live_bytes` 恒等**(live_bytes 同口径),监控数据自身可信;属性测试断言为 `Σ live[i] − Σ tcache head[i] == 应用活指针数`,线程退出 flush 后 `Σ live` 回到基线(G5)。

### 10.2 快照结构

```c
typedef struct {
    uint64_t epoch;                  /* 递增序号(§11) */
    /* L3 汇总: */
    uint64_t allocated_bytes, deallocated_bytes;   /* 累计 */
    uint64_t live_bytes,   live_objects;
    uint64_t active_bytes;           /* 已映射未归还 = retained+dirty+live(页对齐) */
    uint64_t frag_bytes;             /* active - live(碎片+缓存) */
    uint64_t high_water_live;        /* live_bytes 历史峰值 */
    /* per-class(36 + 大对象): */
    struct { uint64_t live, ndalloc, size; } cls[37];
    /* per-thread(≤ 上限个): */
    struct { uint64_t tid, alloc_bytes, free_bytes, alloc_cnt; } threads[128];  /* 超员合并进 threads[127] */
    /* 事件环(§10.6)+ 采样 TOP N(§10.3) */
} wmm_stats_t;
```

`active - live` 是"碎片 + 缓存"的度量——**RSS 比实际使用高的那把钥匙**。

### 10.3 调用点采样(精简 prof)

```
触发:每 N 次慢路径分配(默认 N=1000)采样一次
采集:frame-pointer 手工遍历栈(需 -fno-omit-frame-pointer;编译期开关)
     每帧记 return address,最深 32 帧,存到线程环缓冲(256 项)
聚合:后台线程每 1s 把环缓冲合并进"调用点 → {bytes, count}"哈希表(按采样权重放大)
     偏置校正:bytes × 本次 usize,再 × N(Horvitz-Thompson,与 fork 的 otel_memory 同思路)
输出:TOP N 调用点(函数名 + 偏移,可 addr2line)+ 占比
默认:关闭(wmm_ctl("sampling.enable", 1) 开启),采样率 1/1000 时开销可忽略
```

**这是"内存去哪了"的直接答案**,后端服务定位问题的头号工具。

### 10.4 泄漏检测(三档)

| 档 | 做法 | 开销 | 用途 |
|---|---|---|---|
| C(默认) | 后台线程监测 `live_bytes`:连续 W 个窗口单调上升且分配-释放失衡 → 告警 + 自动开启采样 dump | ~0 | 7×24 |
| B(按需) | 采样持续开启,泄漏期对比 TOP N 增量 | 采样成本 | 定位到函数 |
| A(调试) | `wmm_ctl("leak.dump")` 全量 dump 每个 live 对象的调用点(需开启调用点记账:slab 元数据加 4B 哈希) | 高 | 精确复现 |

### 10.5 越界/双释放/UAF(默认关闭)

- **poison**:释放填 `0xBC`;下次分配若内容被改 → UAF 已发生;
- **redzone**:对象前后 8B guard 模式 `0xBD`;在 free 慢路径和 GC 时抽查对比,命中记事件(含对象类与调用点);
- **quarantine**:释放先入隔离区(每类 32 项),延迟复用,期间写 = UAF 可捕获;
- 三者都经 `wmm_ctl` 运行期开关;开启会使内存放大与慢路径变慢——只在压测/排障时开。

### 10.6 事件日志

```c
typedef struct { uint64_t ts; uint32_t type; uint32_t tid;
                 uintptr_t ptr; uint32_t cls; uintptr_t pc0, pc1; } wmm_event_t;
/* 固定 1024 项环形缓冲,无锁(写者=触发线程,单生产者),映射进共享区(§11) */
/* type: 0xBC poison 命中 | 0xBD redzone | 0xBE 双释放 | 0xBF quarantine
         0xDA 泄漏告警 | 0xDB OOM | 0xDC purge 执行 ... */
```

**线上问题定位的最快路径**:崩溃前看事件日志,类型+调用点直接缩小范围。

---

## 11. 实时监控:共享内存协议(规格)

### 11.1 布局(内存映射文件,固定 ABI)

```
偏移    字段                    说明
0x000   magic "WMMST" + version  协议版本
0x010   seqlock                 奇数=写入中(§11.2)
0x020   wmm_stats_t 主快照       (sizeof==0x13B8,static_assert 锁死)
0x1800  采样 TOP N 表(256 项 × 32B: pc, bytes, count, reserved;名字在监控端 addr2line)
0x4000  wmm_event_t 事件环(1024 × 48B)   /* 映射总大小 0x10000;两个结构尺寸由 wmm.h static_assert 锁死 */
```

- 分配器持写端:后台聚合线程每 1s 更新一次(§10.3/§10.4 同线程);
- 监控进程持读端:只读 `mmap`,轮询 seqlock → 取快照 → 画曲线/告警。**零侵入服务主逻辑、ms 级延迟、无 IPC 开销**。

### 11.2 seqlock 读写(30 行)

```c
/* 写 */ seq_lock(); memcpy(snap, &g_stats); seq_unlock();   // 奇数标记写入中
/* 读 */ do { s1 = seq; memcpy(local, snap); s2 = seq; } while (s1 & 1 || s1 != s2);
```

### 11.3 与内核数据交叉验证

- `/proc/self/statm` / `smaps` 的 RSS 与 `active_bytes` 对照;
- `RSS >> active` → 查 THP、页表页、采样缓冲等"隐藏驻留"(§15)。

---

## 12. 集成到后端服务(规格)

### 方案 A:显式 API(起点)

```c
#include "wmm.h"
void *p = wmm_alloc(1024);
wmm_free(p);
wmm_ctl("stats.snapshot", &st); wmm_ctl("sampling.enable", 1);
/* 过渡宏: #define malloc wmm_alloc ... */
```

### 方案 B:`-Wl,--wrap`(推荐主路径)

```
gcc -O2 -fno-omit-frame-pointer app.c -Wl,--wrap=malloc,--wrap=free,\
    --wrap=realloc,--wrap=calloc,--wrap=posix_memalign,--wrap=aligned_alloc \
    -L. -lwmm -lpthread
```

```c
void *__wrap_malloc(size_t n) { return wmm_alloc(n); }
void  __wrap_free(void *p)    { if (p) wmm_free(p); }
/* realloc/calloc 同理;__real_malloc 保留给需要 libc 内存的第三方场景 */
```

- 本二进制内所有 `malloc/free` 调用被链接器重定向到 `__wrap_*`;**第三方 .so 内部仍走 libc**——对"自控代码 + 少量第三方库"是理想平衡;
- 验证:`nm app | grep -w malloc` 应只出现 `__wrap_malloc`;单元测试覆盖 realloc(NULL)/size 0 等边界。

### 方案 C:`LD_PRELOAD`(最后考虑)

全进程接管(含第三方库),但需处理 libc 内部 malloc 依赖、`dlopen` 顺序、fork/exec、信号安全。**仅在必须统计第三方库分配时做,先灰度**。

### 集成纪律(三种方案通用)

1. **I3 禁止混用**:同一指针绝不跨分配器释放;CI 加审计;
2. fork 后:提供 `wmm_atfork()` 重建锁与 `__thread` 状态(§15);
3. 信号 handler 内禁止调用分配器(或提供 async-signal-safe 子集);
4. 每种集成方式配冒烟测试:跑通 → 统计正确 → 退出零泄漏。

---

## 13. 问题定位 playbook

| 线上症状 | 打开什么 | 判定 |
|---|---|---|
| 内存持续上涨 | C 级泄漏监测 + 采样 | 告警自动开启采样;TOP N 增量 = 嫌疑函数 |
| RSS 高但业务正常 | `active vs live vs RSS` + per-class 直方图 | 小类堆积→缓存过大;`active-live` 高→碎片(查合并/GC) |
| 偶发崩溃 | redzone + 事件日志 | 日志给出类型/偏移/调用点 |
| 释放后使用 | quarantine + 事件日志 | 命中 ts + 释放调用点 |
| 双释放 | 状态机 + 事件日志 | 重复释放调用点 |
| 峰值抖动 | 高水位 + per-thread | 定位线程与类 |
| 天级缓慢泄漏 | epoch 快照对比 | 两次快照的 TOP 增量 |

**通用流程**:全局曲线定位"何时涨" → per-class/per-thread 收敛 → 采样锁定函数 → A 档精确复现。每步都有明确数据出口(§11)。

---

## 14. 路线图:文件清单与验收

| 阶段 | 文件 | 内容 | 规模(约) | 验收 |
|---|---|---|---|---|
| P0 | `gen_sc.py` `bench/bench.c` | 尺寸类生成器(§4)+ 基准(与 glibc 对照) | 150 | 生成表与 §4.2 一致;基准可复跑 |
| P1 | `wmm_core.c` `wmm_core.h` | `wmm_alloc/free/realloc` 单线程正确:尺寸类+slab+freelist+页表+合并 | 800 | 单元+压力测试;`live` 恒等(I2) |
| P2 | `wmm_tcache.c` | cache_bin+批量+自适应 GC+线程退出 | 450 | 多线程吞吐 ≥ 单线程 90%;线程退出零泄漏 |
| P3 | `wmm_phys.c` | retained/dirty+合并+周期 purge+大对象 | 400 | 长期压力 RSS 平稳;无碎片爆炸 |
| P4 | `wmm_stats.c` `wmm_shm.c` | §10 计数+§11 共享内存+事件日志+后台聚合 | 400 | 监控进程实时看到全部指标 |
| P5 | `wmm_prof.c` `wmm_guard.c` | 采样+泄漏三档+redzone/poison/quarantine | 400 | 注入三类 bug 均能定位 |
| P6 | `wmm_wrap.c` `wmm_ctl.c` | `--wrap` 集成+运行期开关+监控面板 | 250 | 真实流量跑通;无混用 |

**合计 ≈ 2.8k 行**。每阶段独立可运行、可验收。可观测(P4/P5)提前是因为它是产品核心,不是收尾。

---

## 15. Linux/后端服务特化坑

1. **自举(I4)**:页表、统计、采样缓冲都从 `mmap` 专用区拿;内部分配器一 malloc 就递归;
2. **`__thread` 析构与 dlclose**:服务会 dlclose 时用 `pthread_key` 或显式 `wmm_thread_exit()`;
3. **fork**:子进程单线程,所有锁重置、`__thread` 重建;注册 `wmm_atfork`;
4. **信号安全**:handler 内禁止分配;文档化边界;
5. **混用灾难(I3)**:libc ↔ 你的库指针互 free = UB(§12 纪律);
6. **线程数 × 缓存上限**:tcache 每线程几十~百 KB(§7.4),1000 线程 = 100MB 级,要设全局上限与告警;
7. **编译选项**:`-fno-omit-frame-pointer`(采样依赖);`-fno-builtin-malloc`(wrap 时防编译器内联绕过);
8. **`--wrap` 边界**:静态链接、第三方库、`dlopen` 分别验证;
9. **THP 双刃剑**:`MADV_HUGEPAGE` 降 TLB miss 但放大驻留页 → 监控 active/RSS;
10. **别用 printf 打日志**:内部日志/告警用 `write(2)` 或事件日志,避免回调 libc malloc;
11. **页表自身的页**:堆很大时页表本身占内存且可能被换出 → 用 `mlock` 或二级页表按需分配;
12. **`MADV_DONTNEED` vs `FREE`**:RSS 敏感选 DONTNEED,延迟敏感选 FREE,二者做成运行期配置。

---

## 16. 验收标准

以"能否回答下列问题"作为完成的定义:

| 问题 | 验收方式 |
|---|---|
| 内存去哪了? | 人为制造吃内存函数 → 采样 TOP N 看到它 |
| 谁在涨? | per-thread / per-class 曲线定位到线程与类 |
| 有没有越界/UAF/双释放? | 注入三类 bug → guard/quarantine/状态机报出调用点 |
| 碎片多严重? | `active−live`、`RSS−active` 两个数可解释 |
| 性能? | 快路径与 glibc 同量级;慢路径可解释 |
| 稳定性? | 多线程 stress + 长期运行曲线平稳;valgrind/ASan 对照零新发现 |
| 数据可信? | `Σ live[i]·size[i] == live_bytes` 恒等(I2),监控自身可信 |

**核心哲学**:这不是"又一个 malloc 替代品",而是**带仪表盘的 malloc**——分配只是功能,监控与定位才是产品。

---

*配套阅读:`DESIGN.md`(jemalloc 设计框架)、`malloc_flow.html`(调用链)、`jemalloc_example.c`(API 示例)。本规格中所有 jemalloc 引用均来自本仓库实测源码。*
