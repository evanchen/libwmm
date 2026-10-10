# tasks.md —— libwmm 手写实现任务分解(人肉可执行版)

> **使用者**:你本人(手写代码)。本文把 `plan.md` 的规格拆成"敲哪个文件、写哪些函数、
> 按什么顺序、写完怎么自测"的具体任务。**按顺序执行,每步自测通过再进下一步**;
> 与本文矛盾时先停下来问,不要自行改规格。
>
> 配套:规格正文 = `allocator_build_guide.md`(指南);验收/DoD = `plan.md`(SPEC 编号)。

---

## 0. 使用方式与编码约定

### 0.1 顺序与节奏
- 每模块 = 一个文件 + 一个自测清单;清单全绿才算完成;
- 每阶段结束有一个"里程碑验证"(§9),通过才进入下一阶段;
- 每条 SPEC 编号(SC2/TC1/…)是验收锚点,自测时对照 `plan.md` §3。

### 0.2 编码约定(全库统一)
- C11;**库内禁止 `printf`/`malloc`**——日志用 `wmm_dprintf`(直接 `write(2)`,§M1);
- 断言用 `assert`;所有内部函数返回错误用 `bool`/`NULL`,不抛异常;
- 每文件头注释写:职责、依赖、不变量(指南 §3 的 I1~I5);
- 页大小常量 `WMM_LG_PAGE=12 / WMM_PAGE=4096`;
- 编译(每模块完成后都要能过):
  ```
  gcc -std=c11 -O2 -fno-omit-frame-pointer -fno-builtin-malloc -pthread \
      -Wall -Wextra -g -fsanitize=address,undefined  ...
  ```

### 0.3 代码根目录
建议新建独立目录(不要混入 jemalloc 仓库),下文以 `<root>/` 表示,例如 `E:\github\libwmm\`。

```
<root>/
├── Makefile
├── include/wmm.h              # 公共 API(plan.md §2 冻结)
├── src/
│   ├── wmm_internal.h         # 内部共享结构(本文件定义全部)
│   ├── wmm_sc.h               # 由 gen/gen_sc.py 生成:bin_info[36] + lookup 表
│   ├── wmm_boot.c             # 启动:VA 堆区、页表、初始化
│   ├── wmm_core.c             # slab/freelist/bin/页表登记/主路径/对齐
│   ├── wmm_phys.c             # extent 层:carve/合并/retained/dirty/purge
│   ├── wmm_tcache.c           # cache_bin 快路径/批量/GC/线程退出
│   ├── wmm_stats.c            # 计数/快照/事件日志
│   ├── wmm_shm.c              # 共享内存 seqlock 协议
│   ├── wmm_prof.c             # 调用点采样
│   ├── wmm_guard.c            # redzone/poison/quarantine + 泄漏三档
│   ├── wmm_ctl.c              # wmm_ctl 命令分发
│   └── wmm_wrap.c             # __wrap_* 链接期接管
├── gen/gen_sc.py              # 尺寸类生成器(手写)
├── tests/                     # 每个模块对应一个 test_*.c
├── monitor/                   # 独立监控进程(读共享区)
└── bench.c                    # 基准
```

---

## 1. 任务总览(依赖序)

| 阶段 | 模块 | 内容 | 自测锚点 |
|---|---|---|---|
| A 骨架 | M0 | Makefile + wmm.h + wmm_internal.h + wmm_ctl 分发骨架 | 可编译 |
| A 核心 | M1 | gen_sc.py + 生成 wmm_sc.h | SC1/SC3 |
| | M2 | wmm_boot:VA 堆区 + 页表 + 初始化 | G4 |
| | M3 | slab + freelist + bin 状态机 | SLAB1/2, AR1 |
| | M4 | 页表登记/反查 + 主路径 alloc/free/realloc/usable_size | PTE1/2, API1 |
| | M5 | 对齐分配 aligned/posix_memalign | API1 |
| | M6 | extent 层:carve/合并/retained/dirty + 大对象 | PHY1/PHY3 |
| | M7 | **里程碑**:oracle 属性测试 + 单线程压力 | G1/G2/G5 |
| B tcache | M8 | cache_bin + 快路径 + 批量 | TC1/TC2 |
| | M9 | 自适应 GC + 线程退出 | TC3/TC4/G5 |
| C 物理 | M10 | purge 后台线程 + MADV | PHY2 |
| D 可观测 | M11 | 统计计数 + 快照 + 事件日志 | OBS1 |
| | M12 | 共享内存 seqlock + 聚合线程 | SHM1 |
| E 定位 | M13 | 调用点采样 | OBS2 |
| | M14 | guard/quarantine + 泄漏三档 | OBS3/OBS4 |
| F 集成 | M15 | wmm_ctl 全部命令 + --wrap 集成 | API1/G3 |
| | M16 | 监控进程 + 基准 + playbook 演练 | §6 |

> 注:M4 完成即可跑第一个"能用的 `wmm_alloc/free`"(单线程、无 tcache)。

---

## 2. M0 工程骨架

**文件**:`Makefile`、`include/wmm.h`、`src/wmm_internal.h`、`src/wmm_ctl.c`(空壳)

**职责**:编译体系 + 冻结公共 API + 内部结构"总装图"。

### M0.1 写 `include/wmm.h`
逐字抄 `plan.md` §2 的契约(已冻结)。**不得加删改签名**。

### M0.2 写 `src/wmm_internal.h`(本库的"总装图",先定结构再写实现)

```c
#define WMM_LG_PAGE 12
#define WMM_PAGE    ((size_t)1 << WMM_LG_PAGE)
#define WMM_NBINS   36
#define WMM_ALIGN   16                 /* QUANTUM */

/* ---- 尺寸类(生成,只读) ---- */
typedef struct { uint32_t reg_size, pgs, slab_size, nregs; } wmm_bin_info_t;
extern const wmm_bin_info_t wmm_bin_info[WMM_NBINS];
extern const uint8_t wmm_lookup_tab[513];      /* (size+7)>>3 → bin index, size≤4096 */

/* ---- 页表项 ---- */
/* 8B 项,低 1 位 tag: 0=指向 slab_header_t*(小对象); 1=指向 wmm_extent_t*(大对象);
   raw==0 表示该页未分配。 */
typedef union { struct { uintptr_t tag : 1, ptr : 63; } s; uintptr_t raw; } wmm_pte_t;
extern wmm_pte_t *g_pte;             /* 大小 = HEAP_VA_SIZE/PAGE,M2 boot 时 mmap */

/* ---- slab ---- */
typedef struct wmm_slab_s wmm_slab_t;
struct wmm_slab_s {
    uint32_t     szind;               /* 尺寸类 */
    uint32_t     nfree;
    uint32_t     free_head;           /* 空闲链头(region 索引),见 M3 */
    void        *addr;                /* slab 基址(页对齐) */
    wmm_slab_t  *full_prev, *full_next;    /* full 双向链 */
    wmm_slab_t  *np_prev,  *np_next;       /* nonfull 地址序双向链 */
    void        *ext;                 /* 所属 extent(M6 之后才用到) */
};

/* ---- bin ---- */
typedef struct {
    wmm_slab_t *slabcur;
    wmm_slab_t *full_head;
    wmm_slab_t *nonfull_head;         /* 地址序,最低地址在头 */
    uint64_t live, ndalloc, nslabs, curslabs;   /* M11 才用,先占位 */
} wmm_bin_t;
extern wmm_bin_t g_bins[WMM_NBINS];   /* 全局单 arena */

/* ---- tcache(head 索引版,可手写优先) ---- */
typedef struct {
    void    **stack;                  /* 容量 ncached_max 的数组 */
    uint16_t ncached_max;
    uint16_t head;                    /* 已缓存数;栈顶=stack[head-1] */
    uint16_t low_water;               /* 上次 GC 后 head 的最小值 */
    uint16_t pad;
    uint64_t nrequests;
} wmm_cache_bin_t;

/* ---- 线程数据 ---- */
typedef struct {
    wmm_cache_bin_t bins[WMM_NBINS + 2];   /* 36 小 + 大对象缓存 + 1 预留 */
    bool enabled;
} wmm_tcache_t;
extern _Thread_local wmm_tcache_t *g_tcache;   /* 惰性创建,或 __thread 内嵌 */

/* ---- 小工具(全库通用) ---- */
void wmm_dprintf(const char *fmt, ...);   /* write(2) 直出,禁止 printf */
uint32_t wmm_div_regind(const void *p, const void *base, uint32_t reg_size); /* M4 */
```

### M0.3 写 `src/wmm_ctl.c`(分发空壳)
```c
int wmm_ctl(const char *cmd, ...) {
    /* 命令表静态数组,当前全部返回 -1/ENOSYS;
       后续模块落地后逐条填充(M11 stats / M13 sampling / M14 guard / M15 purge) */
}
```

### M0.4 写 `Makefile`
- 目标:`debug`(`-O0 -g -fsanitize=address,undefined`)、`release`(`-O2`)、`test`(编译全部 tests/)、`bench`;
- 统一 flags(§0.2);`libwmm.a` + `libwmm.so`。

### 自测清单(M0)
- [ ] `make debug` 空库编译链接零告警(-Wall -Wextra);
- [ ] `nm libwmm.a | grep wmm_` 能看到 `wmm_ctl`;`nm` 无 FP(帧指针)相关告警;
- [ ] 编译带 `-fno-omit-frame-pointer` 已确认生效。

---

## 3. M1 尺寸类生成器 `gen/gen_sc.py` → 生成 `src/wmm_sc.h`

**职责**:把尺寸类表、lookup 表、slab 参数从"算法"变成"常量",杜绝手抄错误。

### M1.1 实现 `size_classes()`(忠实移植)
- 输入:`LG_QUANTUM=4, SC_LG_TINY_MIN=3, SC_LG_NGROUP=2(→4/组), LG_PAGE=12, LOOKUP=12`;
- 输出:36 行 `(reg_size, pgs, slab_size, nregs)`,**与指南 §4.2 表逐行一致**;
- `pgs` 用 LCM 循环求(`指南 §4.3`):从 1 页起,`页数*PAGE == 区域数*reg_size` 即停;
- 大对象部分:仅生成 `large_min=16384` 常量即可(M6 用公式,不生成 199 项表)。

### M1.2 生成 `wmm_lookup_tab[513]`
```python
for s in range(1, 513):
    size = s << 3                       # 8..4096,步进 8
    tab[s] = size2index(size)           # 用 size2index 主函数回填
```
- 手工核对:`tab[1]=0(8B)`, `tab[2]=1(16B)`, `tab[3]=1(17B 归 16)`, `tab[512]=28(4096)`。

### M1.3 生成头文件骨架
- 头部注释:生成时间、算法出处(sc.c)、**校验和断言**(`static_assert(sizeof(wmm_bin_info)==16)` 等);
- 附一个 `wmm_sc_selfcheck()`(调试用):遍历 1..14336 随机取样,断言 `size2index` 结果 ≥ 请求且上界最小。

### 自测清单(M1)
- [ ] 生成表与指南 §4.2 逐行 diff 一致(可写进脚本断言);
- [ ] `tab` 关键点核对通过;
- [ ] 属性抽样:`for size in 1..14336 步进 7:` `usize=bin_info[size2index(size)].reg_size ≥ size` 且 `size≤4096 时与查表结果一致`(SC2)。

---

## 4. M2 启动 `src/wmm_boot.c`

**职责**:在 `wmm_boot()` 里完成三件事:堆区 VA、页表、bin 初始化。**全部单线程、最早调用**。

### M2.1 堆区 VA(为可手写而简化的关键决策)
```
HEAP_VA_SIZE = 16GB(可配置)
base = mmap(NULL, HEAP_VA_SIZE, PROT_NONE,
            MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)   /* 只占 VA,不占物理 */
```
- **为什么**:页表大小 = VA/PAGE×8 = 32MB,**固定一次分配**;不需要动态增长页表(手写最容易错的地方);
- 32MB 一次性 memset 可接受;后续优化点:二级页表按需(M16 之后)。

### M2.2 页表
```
g_pte = mmap(NULL, HEAP_VA_SIZE/PAGE * 8, PROT_READ|WRITE, MAP_PRIVATE|MAP_ANONYMOUS)
memset(g_pte, 0, 大小)                 /* 0=未用 */
```

### M2.3 初始化
- `g_bins[36]` 全零;
- 记录 `g_heap_base / g_heap_cur`(bump 指针,M6 用);
- 注册 `pthread_atfork` 三个钩子(先空实现,§15 指南坑 3 补全);
- **I4 自举检查**:boot 过程中只允许 `mmap/munmap/madvise/memset`,禁止调用自身分配函数。

### 自测清单(M2)
- [ ] `wmm_boot` 后 `g_pte` 32MB 可读写、`g_bins` 全零;
- [ ] 重复调用 `wmm_boot` 返回错误(幂等保护)。

---

## 5. M3 slab + freelist + bin 状态机 `src/wmm_core.c`(第一部分)

**职责**:一页(或 pgs 页)如何变成"可分配 region 的 slab",以及 bin 如何管理 slab 集合。
**本阶段只做"把已 mmap 的页初始化为 slab",物理层(M6)才负责拿页。**

### M3.1 slab 建立
```c
/* 把 [addr, addr+pgs*PAGE) 初始化为 slab。addr 必须页对齐。 */
void wmm_slab_init(wmm_slab_t *sl, void *addr, szind_t i);
/* 步骤: 1) memset 头部字段(szind/nfree=pgs*PAGE/reg_size... 由 bin_info 算);
         2) 建立空闲链: free_head=0; 对 r=0..nregs-2: *(uint32_t*)(addr+r*reg_size) = r+1;
            最后一个 region 首字 = UINT32_MAX(链尾哨兵);
         3) 登记页表: 该 slab 的每一页 → {tag=0, ptr=sl}(PTE1)。 */
```

### M3.2 region 分配/释放(纯逻辑,先不碰 bin)
```c
void *wmm_slab_alloc(wmm_slab_t *sl);   /* pop 空闲链, nfree--, 返回 addr+r*reg_size */
void  wmm_slab_free(wmm_slab_t *sl, void *p);
/* free 步骤: regind=(p-sl->addr)/reg_size(先普通除法);
              *(uint32_t*)p = sl->free_head; sl->free_head = regind; sl->nfree++; */
```
- **边界**:`nfree==0` 再 alloc → assert 失败(调用方必须先判);
- **不变量(SLAB2)**:从 free_head 沿链走,应恰好访问 nfree 个 region 且无环;自测遍历。

### M3.3 bin 状态机
```c
void *wmm_bin_alloc(wmm_bin_t *b, szind_t i);    /* AR1 */
void  wmm_bin_dalloc(wmm_bin_t *b, wmm_slab_t *sl, void *p);
void  wmm_bin_slab_install(wmm_bin_t *b, wmm_slab_t *sl);  /* 新 slab 上任 */
```
**bin_alloc 流程**(对照指南 §8.2):
```
1. slabcur 非空且 nfree>0 → wmm_slab_alloc(slabcur) 返回
2. 否则: slabcur 满 → 挂 full 链
3. nonfull 头(最低地址)取一个 → 成为 slabcur
4. 都没有 → 返回 NULL(调用方去 M6 取新页再 install)
5. b->live++(G2)
```
**bin_dalloc 流程**(对照指南 §8.4):
```
1. sl->nfree++ 并压链
2. 若 nfree==nregs(全空): 从所在结构摘除(slabcur 置 NULL / 从 nonfull 摘),
   返回"可归还"标志 → 调用方交给 M6
3. 若原在 full 链: 摘除, 按地址插入 nonfull(最低在前, O(n) 插入即可)
4. b->live--(G2)
```

### 自测清单(M3)
- [ ] SLAB1:单测建 slab 后遍历全部 region,地址 = addr+r*reg_size、互不重叠、覆盖全区间;
- [ ] SLAB2:随机 alloc/free 序列后,nfree 与链长一致;
- [ ] AR1:状态转移单测(全满→full;全空→归还;slabcur 切换取最低地址);
- [ ] PTE1:slab 每页页表项都指向该 slab。

---

## 6. M4 页表登记/反查 + 主路径 `src/wmm_core.c`(第二部分)

**职责**:把 M3 的"slab 世界"接到 `wmm_alloc/wmm_free` 上(单线程、无 tcache,先跑通)。

### M4.1 页表辅助
```c
wmm_pte_t wmm_pte_get(const void *p);          /* p>>12 查表 */
void      wmm_pte_set(const void *p, wmm_pte_t v);
bool      wmm_pte_is_slab(const void *p, wmm_slab_t **out);
bool      wmm_pte_is_extent(const void *p, void **out_ext);
```
**free 反查流程(PTE2)**:
```
pte = g_pte[p>>12]
if raw==0        → 未分配地址,记事件(双释放/野指针,M14),返回
if tag==1        → 大对象:交给 M6 wmm_extent_free
else             → slab: wmm_slab_free(sl, p) → 视 nfree 决定是否 bin 归还(M3.3)
```

### M4.2 size2index(主路径第一行)
```c
static inline uint32_t wmm_size2index(size_t size) {
    if (size <= 4096)  return wmm_lookup_tab[(size + 7) >> 3];
    if (size <= 14336) return wmm_sc_lookup_large_small(size);   /* 36 项二分 */
    return WMM_INDEX_LARGE;
}
```

### M4.3 主路径
```c
void *wmm_alloc(size_t size) {
    if (size == 0) size = 1;                  /* API1: glibc 语义 */
    if (size <= 14336) {  /* 小对象 */
        i = wmm_size2index(size);
        p = wmm_bin_alloc(&g_bins[i], i);     /* M3.3;NULL → M6 取新页后重试 */
        /* 取到页后: wmm_slab_init + wmm_bin_slab_install + 再 alloc(至多一次) */
    } else {
        p = wmm_large_alloc(size);            /* M6 */
    }
    return p;
}
void  wmm_free(void *p)  { if (!p) return; wmm_pte 反查 → 小/大分派 }
void *wmm_realloc(void *p, size_t n) {
    if (!p) return wmm_alloc(n);
    usize = wmm_usable_size(p);
    if (n <= usize) return p;                 /* 原地收缩,直接返回 */
    q = wmm_alloc(n); memcpy(q, p, usize); wmm_free(p); return q;
}
size_t wmm_usable_size(const void *p) { pte 反查 → slab: bin_info[szind].reg_size; extent: npages*PAGE }
```

### M4.4 调试
- `wmm_dprintf` 用于 boot/首错时打印;`MM_DEBUG` 宏包裹。

### 自测清单(M4)
- [ ] API1 全过(§2 错误契约 4 条):size0、NULL、realloc 特例、usable_size≥请求;
- [ ] PTE2:随机分配后 `wmm_free` 全部正确归还(配合 M3 的 nfree 断言);
- [ ] 简单循环:`for 10000: alloc(随机≤4K) / 一半 free` 无崩溃、无 assert。

---

## 7. M5 对齐分配 `src/wmm_core.c`(第三部分)

**职责**:`wmm_posix_memalign / wmm_aligned_alloc`。**分三档,逐档实现**(指南 §4.4 的"免费对齐"思想):

| 档 | 条件 | 做法 |
|---|---|---|
| 1 | `alignment ≤ 16` | 直接 `wmm_alloc(size)`(所有类 16 对齐,G1) |
| 2 | `16 < alignment ≤ 4096` 且 `size ≤ 14336` | `rounded = (size + align-1) & ~(align-1)`;`wmm_alloc(rounded)`(指南 §4.4 的取整技巧,类对齐 ≥ align) |
| 3 | `alignment > 4096` 或 `size > 14336` | 大对象过分配:`wmm_large_alloc(size + alignment)`,把返回基址向上对齐到 `alignment`;free 走页表反查 M6(M6 完成后本档才可用) |

- **档 2 正确性依据**(写注释):所有 bin 类的 reg_size 是 16 的倍数,且 `rounded` 是 alignment 的倍数时,其上界类必为 alignment 的倍数(类边界在 16 整数倍上);
- **边界**:`alignment` 非 2 的幂 → 返回 `EINVAL`;`size` 溢出 → `ENOMEM`;`posix_memalign` 失败返回错误码且 `*memptr=NULL`。

### 自测清单(M5)
- [ ] 随机 `alignment∈{1,2,4,8,16,32,64,128,256,512,1024,4096} × size 随机`:返回值对齐;释放正确;
- [ ] 非法 alignment、溢出用例返回码正确。

---

## 8. M6 物理层 `src/wmm_phys.c`

**职责**:页从哪来、还到哪去、如何合并、大对象。**这是对抗外部碎片的命脉(PHY1)**。

### M6.1 extent 与地址序链
```c
typedef struct wmm_extent_s {
    void       *addr;      /* 页对齐 */
    size_t      npages;
    uint32_t    state;     /* WMM_EXT_FREE_DIRTY / FREE_RETAINED / INUSE */
    struct wmm_extent_s *ar_prev, *ar_next;  /* 全量地址序双向链(含 INUSE) */
    struct wmm_extent_s *fl_prev, *fl_next;  /* 空闲链(仅 FREE 态) */
    wmm_slab_t *slab;      /* 若承载小对象 slab;大对象为 NULL */
} wmm_extent_t;
```
- **单一地址序双向链**:carve 时插入、free 时原地改 state——**找邻居合并 = 链上 prev/next,O(1)**;
- 空闲另挂一条 free 链(便于 first-fit 搜索)。

### M6.2 carve(拿页)
```c
/* 优先序(指南 §9.2): 1) 空闲链 first-fit(够大)  2) 新 mmap(MAP_FIXED 到堆区内) */
void *wmm_extent_get(size_t npages, uint32_t *state_out);
```
- first-fit:扫 free 链,找到 `npages 大` 的最小地址项;剩余部分(若有)留在链上;
- 无可用 → `mmap(g_heap_cur, npages*PAGE, PROT_READ|WRITE,
  MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS)`(MAP_FIXED 到 boot 保留的 VA 窗口内),`g_heap_cur += npages*PAGE`;
- 新映射 → 建 extent、插地址序链;
- **失败**:堆区耗尽返回 NULL → `wmm_alloc` 返回 NULL(OOM 契约)。

### M6.3 释放与合并(核心)
```c
void wmm_extent_free(wmm_extent_t *e);   /* PHY1 */
/* 1) 状态 → FREE_DIRTY;入 free 链;
   2) 合并: 检查地址序链邻居(prev/next),
      邻居 FREE 且 addr+npages 相接 → 吞并(更新 npages, 摘除邻居, 继续向两侧);
   3) 若为 slab 承载页,先由 bin 摘除(全空 slab, M3.3)再走本函数; */
```

### M6.4 大对象路径
```c
void *wmm_large_alloc(size_t size);    /* npages = ceil(size/PAGE);
                                          页表登记: 每页 {tag=1, ptr=extent}(PTE1 覆盖大对象) */
void  wmm_large_free(void *p);         /* pte→extent→wmm_extent_free */
```
- 页表:extent **每一页**都登记同一 extent 指针(简化,`wmm_free` 任意页进入都可反查);free 时以 p 所在页的 extent 为准,整体归还;
- **大对象 `live_large` 计数(M11 填)** 先占位。

### M6.5 小对象与 extent 的接线(wmm_core 回调)
- `wmm_bin_alloc` 返回 NULL 时:`e = wmm_extent_get(pgs)` → `wmm_slab_init(sl=e->addr, i)` → `e->slab=sl` → `wmm_bin_slab_install` → 重试;
- `wmm_bin_dalloc` 返回"可归还"时 → `wmm_extent_free`。

### 自测清单(M6)
- [ ] PHY1:交替分配/释放固定大小后,`free 链上前 K 项` 的地址连续性 = 合并成功(RSS 不涨);
- [ ] PHY3:大对象(≥16K)分配/释放、`usable_size` 为页面倍数、页表全页登记正确;
- [ ] 随机混合(小+大)压力 5 分钟,`g_heap_cur` 增长有界(碎片被合并吸收)。

---

## 9. M7 里程碑:oracle 属性测试(单线程正确性总验收)

**文件**:`tests/test_oracle.c` + `tests/test_stress.c`

### M7.1 oracle 测试(test_oracle.c)
参考模型 = 自维护"活指针集合 + 请求大小"结构(数组/哈希)。
```
随机 100 万步,操作:
  alloc(随机 1..1MB) | free(随机活指针) | realloc(随机活指针)
每步后断言:
  ① G1 对齐: (uintptr_t)p % 16 == 0(8B 请求允许 8)
  ② usable_size(p) ≥ 请求
  ③ free 的指针必在集合中(双释放即错)
  ④ 集合元素数 == 内部统计(打开 MM_DEBUG 钩子:Σ 各 bin live == 集合数)
每 10 万步: 全量 free 后再全量 alloc,确保状态收敛(无泄漏语义)
```
- **glibc 对照模式**:同一条随机序列喂给 glibc malloc 跑一遍,双方都必须"成功完成"(行为兼容验证)。

### M7.2 压力测试(test_stress.c)
- 多线程(4/8/16)各自独立随机序列 10 分钟,全部通过 M7.1 断言;
- 观察 `/proc/self/statm` RSS:整体曲线应平稳(合并生效)。

### 自测清单(M7)
- [ ] `make test` 全绿;**里程碑通过 → 允许进入 B 阶段**;
- [ ] RSS 曲线平稳打印确认(PHY1 生效)。

---

## 10. M8 tcache `src/wmm_tcache.c`(第一部分:快路径 + 批量)

**职责**:把 95% 的分配/释放变成无锁弹栈。结构用 head 索引版(wmm_internal.h 已定)。

### M8.1 初始化与栈内存
- `g_tcache` 用 `_Thread_local`;首次使用时惰性创建:
  - 栈数组内存:一个 `mmap` 按需分配(M15 优化:批量一个数组);**不能用 wmm_alloc 自举(I4)**;
  - 每个 bin 的 `ncached_max` 按公式(指南 §7.4):
    ```
    小对象: candidate = nregs << 1(取偶), clamp [20,200]
    大对象缓存(bins[36]): 20
    ```
- `stack = mmap(NULL, ncached_max*8, ...)`;`head=0; low_water=0`。

### M8.2 快路径(核心,对照指南 §7.2)
```c
void *wmm_tcache_alloc(szind_t i) {
    wmm_cache_bin_t *b = &g_tcache->bins[i];
    if (b->head == 0) return NULL;                 /* 慢路径 */
    void *p = b->stack[--b->head];
    if (b->head < b->low_water) b->low_water = b->head;   /* 每次弹栈更新水位(简化) */
    return p;
}
void wmm_tcache_free(szind_t i, void *p) {
    wmm_cache_bin_t *b = &g_tcache->bins[i];
    if (b->head == b->ncached_max) wmm_tcache_flush(i, b->ncached_max - b->ncached_max/4); /* 满则部分刷 */
    b->stack[b->head++] = p;
}
```

### M8.3 批量填充/刷出
```c
/* 填充(仅在 head==0 时调用): 一次性向 arena 要 nfill 个 */
void wmm_tcache_fill(szind_t i, unsigned nfill);
/* 步骤: unsigned n = wmm_bin_fill(&g_bins[i], i, nfill, b->stack);  // M8.4
        b->head = n; b->low_water = n; */
/* 刷出(GC/线程退出): 从栈底刷 n 个回 bin */
void wmm_tcache_flush(szind_t i, unsigned n);
/* 步骤: 逐对象 wmm_bin_dalloc(或批量版); memmove 剩余下移;
        head -= n; 刷后 b->low_water = head; */
```

### M8.4 arena 批量填充(wmm_core 补一个函数)
```c
/* 一次持锁填 n 个(指南 §8.5);返回实际填了多少 */
unsigned wmm_bin_fill(wmm_bin_t *b, szind_t i, unsigned want, void **out);
```

### M8.5 主路径接入(改 wmm_alloc/free)
```
wmm_alloc: if (size<=14336 && tcache 启用):
             p = wmm_tcache_alloc(i);
             if (p) goto out;
             p = wmm_bin_alloc(...); 若 NULL→取页重试;
             顺手 wmm_tcache_fill(i, nfill);  // 只填一次,对象先出 bin
           out: thread 计数(M11)占位
wmm_free:  反查 szind → if (小 && tcache 启用) wmm_tcache_free(i, p)
           else 直接 bin/大对象归还
```
- **大对象**也走 tcache 缓存(bins[36],容量 20)——M8.6。

### 自测清单(M8)
- [ ] TC1:单线程分配 10 万次,命中 tcache(打点:head 无变化路径计数)≥ 90%;
- [ ] TC2:任何时刻 `head ≤ ncached_max`;批量填充后 `head == nfill` 或不足;
- [ ] M7 oracle 全量重跑仍绿(行为不回归)。

---

## 11. M9 tcache GC + 线程退出 `src/wmm_tcache.c`(第二部分)

### M9.1 自适应 GC(指南 §7.5,公式照抄)
```c
/* 在 tcache_free 发现"满"或周期性触发时执行 */
static unsigned wmm_tcache_gc_bin(szind_t i) {
    b = &g_tcache->bins[i];
    ncached = b->head;
    used_since_gc = ncached - b->low_water;      /* low_water = 上次 GC 后 head 最小值 */
    retain = (used_since_gc == 0) ? 2
           : (used_since_gc + (used_since_gc >> 2));
    if (retain > b->ncached_max) retain = b->ncached_max;
    nflush = ncached - retain;
    if (nflush > 0) wmm_tcache_flush(i, nflush);
    b->low_water = b->head;                      /* GC 后重置 */
    return nflush;
}
```
- **触发点**:a) `wmm_tcache_free` 满时(先 GC 再压栈);b) 慢路径经过时按 bin 轮流 GC(简单版:每 N 次分配 GC 一个 bin,轮转);
- **填充目标自适应**(可后做,先固定 nfill= ncached_max 的一半):`nfill` 翻倍/减半公式见指南 §7.5。

### M9.2 线程退出(必做,指南 §7.6)
```c
void wmm_thread_exit(void) {
    for (i in 0..36) if (head>0) wmm_tcache_flush(i, head);
    munmap(tcache 内存); g_tcache = NULL;
}
```
- 注册:`__thread` 析构函数 或 `pthread_key_create` 析构;
- **验证 G5**:线程反复创建销毁后,各 bin `live` 计数与"无 tcache"基线一致。

### 自测清单(M9)
- [ ] TC3:人为"偶尔用一次"的 bin,GC 后 `head ≤ retain` 公式值;
- [ ] TC4/G5:1000 次线程创建/退出,`Σ live` 不变、RSS 不涨;
- [ ] M7 oracle + 多线程压力重跑全绿。

---

## 12. M10 purge 后台线程 `src/wmm_phys.c`(第二部分)

### M10.1 后台聚合线程(先做物理回收,观测聚合 M12 挂同线程)
```c
static void *wmm_bg_thread(void *arg) {
    while (!stop) {
        sleep(1s);
        wmm_phys_purge_if_needed();     /* M10.2 */
        wmm_shm_publish();              /* M12 再填,先空 */
    }
}
```

### M10.2 回收策略(指南 §9.4)
```c
void wmm_phys_purge_if_needed(void) {
    if (dirty 总字节 > wmm_opt.dirty_max /* 默认 8MB */) {
        for (free 链中 state==FREE_DIRTY 的 extent):
            madvise(addr, npages*PAGE, MADV_DONTNEED);
            state = FREE_RETAINED;
    }
}
```
- `MADV_FREE` 做成运行期开关(`wmm_ctl("config.madvise_mode")`);
- **注意**:RETAINED 页仍可复用(carve 时优先,免 syscall)。

### 自测清单(M10)
- [ ] PHY2:分配 1GB 再全 free,后台 2 秒内 `active_bytes`(M11 打点)回落、RSS 下降;
- [ ] 线程生命周期:`wmm_ctl("bg.enable")` 启停正确,退出时 join。

---

## 13. M11 统计与事件 `src/wmm_stats.c`

**职责**:让"数据可信"成为可断言的事实(G2)。这是核心目标,不是附加。

### M11.1 计数器埋点(对照指南 §10.1,精确到位置)
| 位置 | 代码 |
|---|---|
| wmm_alloc 快路径出口 | `g_tcache->t_alloc_bytes += size; t_alloc_cnt++`(线程局部) |
| wmm_free 快路径出口 | `t_free_bytes += usize; t_free_cnt++` |
| wmm_bin_alloc / bin_dalloc / bin_fill | `b->live += n / --`(锁内,唯一写点,G2) |
| 大对象 | `g_large.live++; live_bytes += npages*PAGE` |
| mmap/madvise/munmap | `g_phys.active_bytes ±=` |
| 高水位 | 快照时更新 `high_water_live = max(...)` |

### M11.2 快照
```c
int wmm_stats_snapshot(wmm_stats_t *st) {
    /* 1) 合并各 bin 的 live/ndalloc(G2 保证一致);
       2) 读线程局部计数(允许瞬时偏差);
       3) 填 active/frag/high_water;epoch++; */
}
```
- **自检断言(测试钩子,始终开启)**:`Σ_i live[i]*reg_size[i] + large.live_bytes == st->live_bytes`(OBS1)。

### M11.3 事件日志
```c
#define WMM_EVENT_RING 1024
typedef struct { uint64_t ts; uint32_t type, tid; uintptr_t ptr, pc0, pc1; } wmm_event_t;
void wmm_event_log(uint32_t type, void *ptr);   /* 单生产者无锁,写指针原子++ */
int  wmm_event_drain(wmm_event_t *buf, size_t n, size_t *got);
```

### 自测清单(M11)
- [ ] OBS1:oracle 每步后钩子为真(打开 MM_DEBUG);
- [ ] 事件:人为触发 OOM/异常路径,`wmm_event_drain` 能取到;
- [ ] 快照字段与手工统计一致。

---

## 14. M12 共享内存监控 `src/wmm_shm.c`

### M12.1 布局(冻结 ABI,指南 §11.1)
```
0x000  magic "WMMST" + version(4+4)
0x010  seqlock(uint64, 奇数=写入中)
0x020  wmm_stats_t 主快照(固定大小,由 wmm_stats_t 布局决定)
0x800  TOP N 采样表(256 项 × 32B)——M13 填
0x4000 wmm_event_t 环(1024 × 32B)
```
- 建:内存映射文件(`memfd_create` 或临时文件)+ `ftruncate(0x4000+1024*32)` + 映射;监控进程 `mmap` 同文件只读;
- 后台线程每 1s `wmm_shm_publish()`:锁 seqlock → memcpy 快照 → 解锁。

### M12.2 seqlock 读端(监控进程/测试用)
```c
do { s1 = seq; memcpy(local, snap); s2 = seq; } while (s1 & 1 || s1 != s2);
```

### 自测清单(M12)
- [ ] SHM1:另开一个进程映射同一文件,读到与进程内 `wmm_stats_snapshot` 一致的快照(epoch 对齐);
- [ ] 高并发分配时读端永不看到撕裂数据(seqlock 生效,压力下验证)。

---

## 15. M13 调用点采样 `src/wmm_prof.c`

### M13.1 栈遍历(手写,~20 行,依赖 -fno-omit-frame-pointer)
```c
static unsigned wmm_backtrace(void **buf, unsigned max) {
    /* frame pointer 链: 取 rbp → 循环 { buf[n++]=((void**)fp)[1]; fp=((void**)fp)[0]; } */
    register void *fp __asm__("rbp");   /* 或 __builtin_frame_address(0) */
}
```

### M13.2 采样与聚合(指南 §10.3)
- 触发:`wmm_alloc` 慢路径每 N=1000 次(线程局部计数)采样一次;
- 存线程环形缓冲(256 项,`{pc[32], usize}`);后台线程(挂 M10 同线程)合并进
  `调用点哈希(pc 折叠前 4 帧 → {bytes×N, count×N})`;
- 输出:`wmm_ctl("prof.top", k, out)` 返回 TOP k(经 `wmm_event`/共享区公布)。

### 自测清单(M13)
- [ ] OBS2:写一个"吃内存函数"(循环 1MB alloc 不 free),开启采样 10s,该函数出现在 TOP 3(地址经 addr2line 可对名);
- [ ] 采样关闭时快路径零额外指令(看反汇编或计数)。

---

## 16. M14 guard/quarantine + 泄漏三档 `src/wmm_guard.c`

> 全部默认关闭,`wmm_ctl` 运行期开关。开启时 M3 的 slab/freelist 语义要配合(见下)。

### M14.1 poison + redzone(指南 §10.5)
- **poison**:`wmm_slab_free` 时 `memset(p, 0xBC, reg_size)`(仅 guard 开启);
  `wmm_slab_alloc` 时抽查首 8B 是否仍是 0xBC(被改 → UAF 事件);
- **redzone**:对象前后各 8B guard(0xBD):slab 建立时在 region 两侧布局 guard 区域 →
  **会改变 reg 布局,故 guard 模式用独立 slab 布局**(reg 区间 = [redzone + 数据 + redzone])——做成编译期或运行期二选一,先做运行期独立 bin 池更简单;
- 检查点:free 时 + GC 时抽查,命中 → `wmm_event_log(0xBD, p)`。

### M14.2 双释放/野指针
- 页表反查 `raw==0` → 事件(已在 M4.1 预留);
- free 时 `wmm_usable_size` 区域检查(可选)。

### M14.3 泄漏三档(指南 §10.4)
| 档 | 实现 |
|---|---|
| C(默认) | 后台线程:连续 W 个 1s 窗口 `live_bytes` 单调上升且 `(alloc-free) 失衡` → 告警事件 + 自动 `prof.enable` |
| B | 告警后对比 TOP N 增量(靠 M13) |
| A | `wmm_ctl("leak.dump")`:遍历全部 slab 的 live region,记录调用点(slab 元数据加 4B 采样哈希,编译开关) |

### 自测清单(M14)
- [ ] OBS3:注入泄漏函数(如每 100 次 alloc 漏 1 次),C 档 30s 内告警,B 档 TOP 增量定位到函数;
- [ ] OBS4:注入越界写 / UAF 读 / 双释放,三种都产出对应类型事件且含调用点;
- [ ] guard 开启时 M7 oracle 仍全绿(正确性不因 guard 破坏)。

---

## 17. M15 wmm_ctl 补全 + `--wrap` 集成

### M15.1 wmm_ctl 命令表(全部落地)
| 命令 | 实现 |
|---|---|
| `stats.snapshot` | M11 |
| `stats.per_class` | M11 |
| `sampling.enable/disable` | M13 |
| `leak.check/dump` | M14 |
| `guard.enable/disable` | M14 |
| `purge` | M10(同步执行一次) |
| `gc` | 遍历所有线程 tcache 强制 GC(M9) |
| `config.lg_page` / `config.madvise_mode` | M2/M10 常量 |
| `bg.enable/disable` | M10 线程 |

### M15.2 `-Wl,--wrap`(指南 §12 方案 B)
```c
/* wmm_wrap.c */
void *__wrap_malloc(size_t n)       { return wmm_alloc(n); }
void  __wrap_free(void *p)          { wmm_free(p); }
void *__wrap_calloc(size_t n, size_t s)  { return wmm_calloc(n, s); }
void *__wrap_realloc(void *p, size_t n)  { return wmm_realloc(p, n); }
int   __wrap_posix_memalign(void **m, size_t a, size_t s) { return wmm_posix_memalign(m, a, s); }
void *__wrap_aligned_alloc(size_t a, size_t s) { return wmm_aligned_alloc(a, s); }
```
- 冒烟测试(`tests/test_wrap.c`):跑一个混合分配的程序 → 断言 ① 跑通 ② 统计非零且合理 ③ 退出零泄漏;
- `nm` 检查:链接后二进制内 malloc 全部为 `__wrap_malloc`(G3 审计)。

### 自测清单(M15)
- [ ] API1(经 wrap 再全过一遍);
- [ ] G3:`--wrap` 冒烟三连通过。

---

## 18. M16 监控进程 + 基准 + 收尾

### M16.1 `monitor/` 独立进程
- `mmap` 共享文件只读 → seqlock 读 → 每 1s 打印/写 CSV:live/active/RSS/per-class TOP5/事件;
- 告警阈值(可配):live 高水位、frag 占比、事件环非空。

### M16.2 `bench.c`
- 场景:单线程顺序、多线程随机、大小混合、大对象;指标:吞吐(op/s)、p50/p99 延迟、RSS;
- 与 glibc 对照跑同一套(输出对比表)。

### M16.3 playbook 演练(指南 §13)
- 7 类症状各演练一次,确认数据出口可回答:内存去哪/谁在涨/越界/泄漏/碎片/峰值。

### 自测清单(M16)
- [ ] 监控进程实时显示全部指标,事件环可见;
- [ ] 基准表:快路径吞吐与 glibc 同量级;p99 合理;
- [ ] playbook 7 项全部有复现步骤与结论。

---

## 19. 附录:实现顺序速查

```
M0 → M1 → M2 → M3 → M4 → M5 → M6 → M7(里程碑:可用的单线程分配器)
   → M8 → M9 → M10 → M11 → M12 → M13 → M14 → M15 → M16
```
每步"能编译 + 自测清单绿"再前进。写不动/拿不准时,把问题连同当前代码贴给我,
我按规格(指南/plan.md)给出最小修改建议——**但代码始终由你落笔**。
