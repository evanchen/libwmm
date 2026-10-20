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
└── bench/bench.c              # 基准
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
extern wmm_pte_t *g_pte;             /* 大小 = WMM_HEAP_VA_SIZE/PAGE,M2 boot 时 mmap */

/* ---- slab ---- */
typedef struct wmm_slab_s wmm_slab_t;
struct wmm_slab_s {
    uint32_t     szind;               /* 尺寸类 */
    uint32_t     nfree;
    uint32_t     free_head;           /* 空闲链头(region 索引),见 M3 */
    void        *addr;                /* slab 基址(页对齐) */
    wmm_slab_t  *full_prev, *full_next;    /* full 双向链 */
    wmm_slab_t  *ph_prev, *ph_next, *ph_child;  /* nonfull 地址序配对堆(仿 jemalloc
                                                 edata_heap;开发期可退化为有序双链,
                                                 只用前两个指针,M7 前升级) */
    void        *ext;                 /* 所属 extent(M6 之后才用到) */
};

/* ---- bin(每类一把锁;M3 单线程阶段先不加锁,M8 多线程起用) ---- */
typedef struct {
    pthread_mutex_t lock;             /* PTHREAD_MUTEX_INITIALIZER 静态初始化;fork 后 child 重置 */
    wmm_slab_t *slabcur;
    wmm_slab_t *full_head;
    wmm_slab_t *nonfull_root;         /* 地址序配对堆根(最低地址在堆顶,AR1) */
    uint64_t live, ndalloc, nslabs, curslabs;   /* M11 才用,先占位 */
} wmm_bin_t;
extern wmm_bin_t g_bins[WMM_NBINS];   /* 全局单 arena */

/* ---- tcache(head 索引版,可手写优先) ---- */
typedef struct {
    void    **stack;                  /* 容量 ncached_max 的数组 */
    uint16_t ncached_max;
    uint16_t head;                    /* 已缓存数;栈顶=stack[head-1] */
    uint16_t low_water;               /* 上次 GC 后 head 的最小值 */
    uint16_t nfill;                   /* 下次填充目标,自适应(M9;初值 ncached_max>>1) */
    uint64_t nrequests;
} wmm_cache_bin_t;

/* ---- 线程数据(tcache 与线程注册表节点合一,整块一次 mmap,布局见 M8.1;
       实现以 src/wmm_internal.h 为准) ---- */
typedef struct wmm_tcache_s wmm_tcache_t;
struct wmm_tcache_s {
    struct wmm_tcache_s *prev, *next; /* g_threads 双向链;g_threads_lock 保护 */
    uint32_t tid;                     /* gettid() 缓存(事件/快照) */
    bool     enabled;                 /* 本线程 tcache 开关(默认 true) */
    uint64_t t_alloc_bytes, t_free_bytes;  /* L1 计数(§10.1):非原子,快照时合并 */
    uint32_t t_alloc_cnt, t_free_cnt;
    wmm_cache_bin_t bins[WMM_NBINS + 1];   /* [0..35] 小对象;[36] 预留
                                              (大对象第一版不缓存,理由见 M8.5) */
};
extern _Thread_local wmm_tcache_t *g_tcache;   /* 惰性创建;NULL=本线程未建 */
extern wmm_tcache_t *g_threads;       /* 线程注册表链头(M11 快照 / M15 枚举 GC) */
extern pthread_mutex_t g_threads_lock;

/* ---- 全局单例(M2 boot 初始化;文件头需 #include <pthread.h>) ---- */
extern void *g_heap_base, *g_heap_cur, *g_heap_end;  /* VA 堆区窗口(M2.1) */
extern pthread_mutex_t g_ext_lock;    /* extent 地址链 + free 链 + g_large 共用一把 */
typedef struct { uint64_t live, live_bytes, ndalloc; } wmm_large_stats_t;
extern wmm_large_stats_t g_large;     /* 大对象统计(M6 起埋点;g_ext_lock 内更新;
                                         匿名 struct 跨编译单元类型不兼容,必须命名) */
typedef struct {
    size_t   dirty_max;               /* 默认 8MB(M10) */
    int      madvise_mode;            /* MADV_DONTNEED / MADV_FREE,运行期可切 */
    unsigned sample_n;                /* 采样间隔,默认 1000;0=关(M13) */
    bool     guard;                   /* redzone/poison/quarantine 总开关(M14) */
    bool     verbose;
    bool     abort_oom;               /* OOM 时 abort(plan §2 opt_abort,默认 false) */
} wmm_opt_t;
extern wmm_opt_t g_opt;

/* ---- 索引类型与页表查询(M4 实现,wmm_core.c) ---- */
typedef uint32_t szind_t;             /* 尺寸类索引;36 = WMM_INDEX_LARGE(大对象) */
#define WMM_INDEX_LARGE WMM_NBINS
wmm_pte_t wmm_pte_get(const void *p);
void      wmm_pte_set(const void *p, wmm_pte_t v);
bool      wmm_pte_is_slab(const void *p, wmm_slab_t **out);
bool      wmm_pte_is_extent(const void *p, void **out_ext);

/* ---- 元数据区(仿 jemalloc base.c;I4 自举) ---- */
void *wmm_base_alloc(size_t size);    /* 8B 对齐 bump,只分配不回收(M2.3) */

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
- 手工核对:`tab[1]=0(8B)`, `tab[2]=1(16B)`, `tab[3]=2(17..24B 归 32)`, `tab[512]=28(4096)`。
  (勘误:原稿 `tab[3]=1` 有误——24B 的最小上界是 32B 类;SC2"最小上界"属性测试会抓住它。)

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
WMM_HEAP_VA_SIZE = 64GB(编译期常量,定义在 wmm_internal.h,-D 可覆盖)
base = mmap(NULL, WMM_HEAP_VA_SIZE, PROT_NONE,
            MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)   /* 只占 VA,不占物理 */
```
- **为什么固定窗口**:页表因此可以是一次 mmap 到位的平坦数组,免掉
  "堆长大要扩页表"的全部动态管理(手写最容易错的地方);
- **代价模型(选窗口大小只看这两行)**:
  VA 预留 = 窗口/512(64GB → 页表数组 128MB;x86-64 地址空间 128TB,VA 免费);
  物理占用 ≈ 实际堆/512(0.2%),随使用惰性增长,**与窗口大小无关**;
- **预留失败重试**:strict overcommit(vm.overcommit_memory=2)会把预留计入
  commit 配额,mmap 失败则窗口减半重试(64G→32G→…≥4G),最终大小记录之;
- **撞顶处理**:窗口耗尽 = OOM,写 0xDB 事件并附当时用量
  (g_heap_cur−g_heap_base),让运维能区分"真没内存"与"撞了人为上限"。

### M2.2 页表
```
g_pte = mmap(NULL, (WMM_HEAP_VA_SIZE / WMM_PAGE) * sizeof(wmm_pte_t),
             PROT_READ|WRITE, MAP_PRIVATE|MAP_ANONYMOUS)
madvise(g_pte, 大小, MADV_NOHUGEPAGE)
/* 严禁 memset:匿名私有映射内核保证零填充(raw==0 即"未分配");
   手写 memset 会把整个数组触碰一遍,凭空给 RSS 垫 128MB 的底,
   还废掉"物理随用增长"的整个设计 */
```
- 首次写某个页表项才分配它所在页的物理内存——页表自身的驻留随堆长大;
- `MADV_NOHUGEPAGE`:防 khugepaged 把零星触碰的页表页合并成 2MB 大页
  (THP=always 的机器),把"每次驻留 4KB"放大成"每次 2MB"。

### M2.3 初始化
- 元数据区(base,仿 jemalloc `base.c`):按 4MB 块 mmap、8B 对齐线性切割,
  块尾不足一次请求则追加新块(大请求独占大块);`wmm_base_alloc()` 供
  slab 头 / extent 结点分配,**只分配不回收**(I4;杜绝递归分配);
- `g_bins[36]` 计数全零;36 把 bin 锁 + `g_ext_lock` + `g_threads_lock`
  在 boot 内显式 `pthread_mutex_init`(不赌"零值即合法互斥量"的
  可移植性,且 fork 后的 child 重建复用同一段做法);
- 记录 `g_heap_base / g_heap_cur / g_heap_end`(实际窗口,供 M6 bump 与越界判据);
- 注册 `pthread_atfork` 三个钩子并直接实现:prepare 按锁序冻结全部全局锁,
  parent 原序反向解锁,child 全部重建(§15 指南坑 3);
- **I4 自举检查**:boot 过程中只允许 `mmap/madvise/write` 类系统调用,
  禁止调用自身分配函数。

### 自测清单(M2)
- [ ] `wmm_boot` 后 `g_pte` 覆盖整个实际窗口、任意页首查为 0(零值来自内核
      零页而非 memset);`g_bins` 全零;
- [ ] boot 后 RSS 无与页表数组等量的台阶(仅实际触碰部分驻留);
- [ ] 重复调用 `wmm_boot` 无效果(幂等;注意其返回类型是 void,不存在"返回错误");
- [ ] 预留失败场景(ulimit -v 调小)会减半重试,最终窗口大小被记录(verbose 打印)。

---

## 5. M3 slab + freelist + bin 状态机 `src/wmm_core.c`(第一部分)

**职责**:一页(或 pgs 页)如何变成"可分配 region 的 slab",以及 bin 如何管理 slab 集合。
**本阶段只做"把已 mmap 的页初始化为 slab",物理层(M6)才负责拿页。**

### M3.1 slab 建立
```c
/* 把 [addr, addr+pgs*PAGE) 初始化为 slab。addr 必须页对齐。 */
void wmm_slab_init(wmm_slab_t *sl, void *addr, szind_t i);
/* 步骤: 1) 头部字段逐个显式赋值(szind/nfree=nregs/free_head=0/链指针清空;
            不用 memset 后覆盖——全字段显式赋值,新增字段漏赋一眼可见);
         2) 建立空闲链: free_head=0; 对 r=0..nregs-2: *(uint32_t*)(addr+r*reg_size) = r+1;
            最后一个 region 首字 = UINT32_MAX(链尾哨兵);
            于是弹出序 = 编号升序 = 地址升序;
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
void *wmm_bin_alloc(wmm_bin_t *b);   /* AR1;b 的下标即类号,不另传 */
bool  wmm_bin_dalloc(wmm_bin_t *b, wmm_slab_t *sl, void *p);
   /* 返回真 = 该 slab 已全空并从 bin 摘除,调用方须归还其物理页 */
void  wmm_bin_slab_install(wmm_bin_t *b, wmm_slab_t *sl);
   /* 新 slab 上任;前提 slabcur==NULL(bin_alloc 返回 NULL 的路径已保证) */
```
**bin_alloc 流程**(对照指南 §8.2):
```
1. slabcur 非空且 nfree>0 → wmm_slab_alloc(slabcur) 返回
2. 否则: slabcur 满 → 挂 full 链
3. nonfull 头(最低地址)取一个 → 成为 slabcur
4. 都没有 → 返回 NULL(调用方去 M6 取新页再 install)
5. b->live++(G2)
```
**bin_dalloc 流程**(对照指南 §8.4;判据按 slab 归属,不能只看 nfree——
热 slab 满(nfree==0)时仍占热位,直到下次 bin_alloc 才挂 full,若按
"nfree==1 即满→非满"判,会对不在 full 链上的 slab 做摘除、清掉整条满链):
```
1. wmm_slab_free 压链;b->live--(G2)
2. sl == slabcur:留在热位;若 nfree==nregs → slabcur=NULL、curslabs--,
   返回真(全空归还)
3. 否则 nfree==nregs(全空退休):nregs==1 从 full 摘(这类分配即满、
   释放即空,人在 full 链),其余从 nonfull 摘;curslabs--;返回真
4. 否则 nfree==1(由满转非满;nonfull 成员释放前 nfree≥1、释放后≥2,
   到不了 1,判据无歧义):摘 full,按地址序插入 nonfull;
   返回假
5. 其余:已在 nonfull,原地变空,返回假
```
**nonfull 数据结构**:M3 落地为地址升序双链(nonfull_root=最低地址,
slab 的 ph_prev/ph_next 承链);每个 bin 的非满 slab 通常个位数,
O(n) 插入无压力。若 M7 压测证明它成为瓶颈,升级为 jemalloc edata_heap
式配对堆——slab 结构的 ph_child 字段已预留,不必改布局。

- **特例 nregs==1 的类(4096/8192/12288)**:分配即满、释放即空,**永不进入 nonfull**
  (指南 §4.2 注,仿 `bin.c:202`);上面的通用状态机天然覆盖,不要为它们写 nonfull 插入分支;

### 自测清单(M3)
- [ ] SLAB1:单测建 slab 后遍历全部 region,地址 = addr+r*reg_size、互不重叠、覆盖全区间;
- [ ] SLAB2:随机 alloc/free 序列后,nfree 与链长一致;
- [ ] AR1:状态转移单测(全满→full;全空→归还;slabcur 切换取最低地址);
- [ ] PTE1:slab 每页页表项都指向该 slab。

---

## 6. M4 页表登记/反查 + 主路径 `src/wmm_core.c`(第二部分)

**职责**:把 M3 的"slab 世界"接到 `wmm_alloc/wmm_free` 上(单线程、无 tcache,先跑通)。
**附最小物理层** `src/wmm_phys.c`:主路径没有页源无法运行,而每个里程碑
必须编译可测,故 M4 随身落地取页/还页的最小集(窗口 bump + 即时交还内核,
含 extent 结点回收链);M6 在同一接口下升级为地址序链 + 相邻合并 + 阈值回收,
主路径无感。并发补件:`wmm_base_alloc` 加锁(叶子锁,锁序 bins→ext→threads→base,
fork prepare 同步跟进)。

### M4.1 页表辅助
```c
wmm_pte_t wmm_pte_get(const void *p);          /* p>>12 查表 */
void      wmm_pte_set(const void *p, wmm_pte_t v);
bool      wmm_pte_is_slab(const void *p, wmm_slab_t **out);
bool      wmm_pte_is_extent(const void *p, void **out_ext);
```
**free 反查流程(PTE2)**:
```
pte = g_pte[p>>12]                /* 先验窗口界,再查表 */
if raw==0        → 未分配地址,记事件(双释放/野指针,M14),返回
if tag==1        → 大对象:交给 wmm_phys_free_extent
else             → slab: bin 锁内 wmm_bin_dalloc(返回真 = 全空退休,
                   锁外 wmm_phys_free_slab 还页)
```

### M4.2 size2index(主路径第一行)
```c
static inline szind_t wmm_size2index(size_t size) {
    if (size <= 4096)  return wmm_lookup_tab[(size + 7) >> 3];
    if (size <= 14336) {  /* 4096 以上只有 8 个类:从类 29 起线性扫。
                             最坏 8 次比较,比二分的区间收缩开销低;
                             且以表为唯一权威——公式推导的常数在改
                             生成参数时会静默错位,扫表永远跟表走 */
        for (szind_t i = 29; i < WMM_NBINS; i++)
            if (size <= wmm_bin_info[i].reg_size) return i;
    }
    return WMM_INDEX_LARGE;
}
```

### M4.3 主路径
```c
void *wmm_alloc(size_t size) {
    if (size == 0) size = 1;                  /* API1: glibc 语义 */
    if (size > 14336) {                       /* 大对象:物理层整页 */
        ext = wmm_phys_alloc_extent(size);
        return ext ? ext->addr : NULL;
    }
    i = wmm_size2index(size);  b = &g_bins[i];
    /* 慢路径全程持 bin 锁(含物理层取页):v1 用简单换正确——两个
       线程同时发现无 slab,不会各自造一个;造 slab 每类低频,可接受 */
    lock(b->lock);
    p = wmm_bin_alloc(b);                     /* M3.3 */
    if (p == NULL) {
        sl = wmm_phys_alloc_slab(i);          /* M4 最小物理层 */
        if (sl) { wmm_bin_slab_install(b, sl); p = wmm_bin_alloc(b); }
    }
    unlock(b->lock);
    return p;
}
void  wmm_free(void *p)  { if (!p) return; 验界 → pte 反查 → 小/大分派 }
void *wmm_realloc(void *p, size_t n) {
    if (!p) return wmm_alloc(n);
    if (n == 0) { wmm_free(p); return NULL; }
    usize = wmm_usable_size(p);
    if (n <= usize) return p;                 /* 原地收缩,直接返回 */
    q = wmm_alloc(n); if (!q) return NULL;    /* 失败不丢旧块 */
    memcpy(q, p, min(usize, n)); wmm_free(p); return q;
}
void *wmm_calloc(size_t n, size_t size) {
    if (size != 0 && n > SIZE_MAX / size) return NULL;   /* 溢出即失败 */
    p = wmm_alloc(n * size); if (!p) return NULL;
    memset(p, 0, n * size);
    /* 无条件 memset:复用对象带上一任内容,不清零 = 信息泄漏 */
}
size_t wmm_usable_size(const void *p) {
    if (!p) return 0;
    验界 → pte 反查 → slab: bin_info[szind].reg_size; extent: npages*PAGE
}
```

### M4.4 调试
- `wmm_dprintf` 用于 boot/首错(OOM、野指针/双释放诊断)时打印;
  开关走运行期 `g_opt.verbose`(`wmm_ctl("config.verbose")` 可切)——
  不设编译期宏,免重编,语义相同。

### 自测清单(M4)
- [ ] API1 全过(§2 错误契约 4 条):size0、NULL、realloc 特例、usable_size≥请求;
- [ ] PTE2:随机分配后 `wmm_free` 全部正确归还(配合 M3 的 nfree 断言);
- [ ] 简单循环:`for 10000: alloc(随机≤4K) / 一半 free` 无崩溃、无 assert。

---

## 7. M5 对齐分配 `src/wmm_core.c`(第三部分)

**职责**:`wmm_posix_memalign / wmm_aligned_alloc`。**分三档**(指南 §4.4 的
"免费对齐"思想:能白拿的对齐绝不付过分配的代价):

| 档 | 条件 | 做法 |
|---|---|---|
| 1 | `alignment ≤ 8` | 直接 `wmm_alloc(size)`(类 0 的对象 8B 对齐、类 1 起全部 16B 对齐,≤8 的 2 的幂白拿) |
| 2 | `8 < alignment ≤ 4096` | `rounded = (size+align-1) & ~(align-1)`;`wmm_alloc(rounded)`(取整技巧;size 无上限——溢进大对象路径也是白拿:页对齐 ⊇ 任何 ≤4096 的 2 的幂,且 alignment 整除页大小,先取整再取页数与不取整相同,零浪费) |
| 3 | `alignment > 4096` | 大对象过分配:`wmm_phys_alloc_extent(size+alignment)`,返回地址向上对齐;free/usable_size 走页表反查 extent(M4 最小物理层即可用,不必等 M6) |

**勘误(原表档 1)**:原条件 `alignment ≤ 16` 直接 `wmm_alloc(size)` 有洞——
`alignment=16, size≤8` 落进 8B 类,奇数号对象只有 8B 对齐,违反契约。
档 1 边界改为 8,16 这档走档 2 取整到 16B 类。

- **档 2 依据(小对象路径)**:类规格形如 2^p×m(m 为奇数,最大 7),故
  不是 alignment(=2^k)倍数的类最大为 7×2^(k−1)=3.5×2^k;而 2^k、2·2^k、
  3·2^k 三者本身都是类。于是 alignment 的倍数 x 的最小上界类:x ≤ 3.5×2^k
  时必是上述三者之一(上界即自身);更大时一切非倍数类都 < x,上界类必为
  倍数。slab 基址页对齐,对象地址 = 基址 + 编号×规格 → 对齐成立;
- **usable_size 配套修正**:对齐档 3 的 p 不在 extent 基址上,可用大小 =
  映射末尾 − p(扣头部零头)。不扣则虚报,realloc 原地扩容分支会让应用
  写穿映射末端;
- **边界**:`alignment` 非 2 的幂(posix 另要求 ≥ sizeof(void*))→ EINVAL;
  档 3 的 size+alignment 回绕与 alignment 超窗口 → ENOMEM;
  `posix_memalign` 失败返回错误码且 `*memptr=NULL`(aligned_alloc 走
  errno 语义;alignment==0 要单列——`0 & (0-1) == 0` 骗过幂检查)。

### 自测清单(M5)
- [ ] 档 2 依据穷举断言:16..4096 全部 2 的幂 × 全部倍数,最小上界类
      规格 % alignment == 0(类表改坏立刻暴露);
- [ ] 全档位 × 尺寸段(含 >4096 档)两 API 一致:对齐成立、usable ≥ 请求、
      首尾字节可写、释放正确;档 3 白盒 usable 精确等于映射末尾 − p;
- [ ] 错误码:非 2 的幂 / <sizeof(void*) / ==0 → EINVAL(两种 API 各自
      语义),回绕与超窗口 → ENOMEM;档 1 边界(alignment=16 且 size≤8)
      不落 8B 类。

---

## 8. M6 物理层 `src/wmm_phys.c`

**职责**:页从哪来、还到哪去、如何合并、大对象。**这是对抗外部碎片的命脉(PHY1)**。

### M6.1 extent 与地址序链
```c
enum { WMM_EXT_INUSE = 1, WMM_EXT_FREE_DIRTY = 2, WMM_EXT_FREE_RETAINED = 3 };
typedef struct wmm_extent_s {
    void       *addr;      /* 页对齐 */
    size_t      npages;
    uint32_t    state;     /* 上列三态;RETAINED 由 purge(M10)产生,M6 不做转换 */
    struct wmm_extent_s *ar_prev, *ar_next;  /* 全量地址序双向链(含 INUSE) */
    struct wmm_extent_s *next;               /* 结点壳回收链(g_ext_free_nodes,LIFO) */
    wmm_slab_t *slab;      /* 若承载小对象 slab;大对象为 NULL */
} wmm_extent_t;
```
- **单一地址序双向链**:carve 时插入、free 时原地改 state——**找邻居合并 =
  链上 prev/next,O(1)**。新 bump 尾插、分裂余段原位后插,都不需要扫描找位置;
- **勘误(原设计"空闲另挂一条 free 链")**:已砍掉。first-fit 直接扫地址序链过滤
  FREE——单 arena、extent 千级,全扫是慢路径几十微秒;省一条链的全部
  插入/摘除代码与每结点 16 字节。jemalloc 的按大小分级搜索树是多 arena
  数十万 extent 规模的选择,不剥离语境移植;
- extent 结点壳从 base 取,合并吞并后挂 `g_ext_free_nodes` 复用;slab 头不复用
  (生命与类流量同长,量级小;统计证伪则同款回收链照加)。

### M6.2 取页 extent_get(内部,已持 g_ext_lock)
- first-fit:沿地址序链从最低地址找第一个 `state != INUSE 且 npages 足够` 的块;
  命中则低段交付、余段(≥1 页)原位留在链上(继承原状态);元数据耗尽无法分裂
  时整块交付(多给不损坏,只浪费);
- 偏好低地址即期望行为:低地址先复用先腾空,高地址空闲块更容易保持大块连续;
- 无命中 → 窗口 bump `mmap(g_heap_cur, …, MAP_FIXED)` 并尾插地址序链;
- **失败**:窗口耗尽 → OOM 路径(dprintf 用量 + abort_oom),`wmm_alloc` 返回 NULL。

### M6.3 释放 extent_release(核心,PHY1)
1. **页表立即清零**(raw=0):页表语义定为"当前归属"——不清零的话,复用窗口期
   内的 double-free 会反查到还在空闲链上的 extent 并把它再 free 一遍,直接毁链;
   清零后与"从未分配"统一为同一条诊断路径。**物理页不还**:FREE_DIRTY 复用零
   系统调用,真正还物理页是 purge(M10,madvise)的事——这正是 jemalloc 攒
   dirty 再批量还的动机;
2. state → FREE_DIRTY,`g_dirty_pages += npages`;
3. 合并:地址序链邻居 prev/next,空闲且地址相接 → 吞并(幸存结点页数扩张,
   被吞结点壳入回收链),向两侧各推到头。dirty 计数页数守恒,合并不改它。

### M6.4 大对象路径
- `wmm_phys_alloc_extent(size)`:npages=ceil(size/PAGE),extent_get(优先复用),
  **每一页**登记 `{tag=1, ptr=extent}`(任意页进入都可反查),g_large 统计
  (live/live_bytes/ndalloc,g_ext_lock 内);
- `wmm_phys_free_extent(ext)`:统计 → extent_release。size > WMM_HEAP_VA_SIZE
  的回绕挡板在入口(接近 SIZE_MAX 的请求会绕过页数取整)。

### M6.5 小对象与 extent 的接线(wmm_phys_alloc_slab/free_slab)
- alloc:base 取 slab 头 → extent_get(pgs) → `wmm_slab_init(sl, e->addr, i)`
  → `sl->ext=e; e->slab=sl` 双向接线 → 主路径 install;
- free(slab 退休):由 `sl->ext` 找回 extent → extent_release。bin 摘除已在
  bin 锁内完成,物理层只拿自己的锁(锁序 bin → ext → base)。

### 自测清单(M6)
- [ ] PHY1:奇偶交替释放 → 4 个独立空闲块;全释放 → 融成单个 128 页块;
      复用取最低地址且 `g_heap_cur` 不前进(零新映射);
- [ ] 分裂:5 页块上取 4 页 → 低段交付、余 1 页原位空闲,计数守恒;
- [ ] 跨路径吸收:3 个 slab(9 页)退休 → 大对象恰要 9 页时复用同一地址;
- [ ] PHY3:大对象每页页表登记同一 extent、usable 为页倍数、释放后全页 raw==0;
- [ ] 结构不变量(每个检查点):地址序链严格递增、不重叠、双向互指、
      `g_dirty_pages == Σ FREE 页数`;
- [ ] 混合压力 ×2 重放(同种子同序列):第二遍 `g_heap_cur` 一页不前进。

---

## 9. M7 里程碑:oracle 属性测试(单线程正确性总验收)

**文件**:`tests/test_oracle.c` + `tests/test_stress.c`

### M7.1 oracle 测试(test_oracle.c)
参考模型 = 自维护"活指针集合 + 请求大小"结构(定长 512 槽数组)。
```
随机 100 万步,操作配比 1:2:1:
  alloc(随机 1..1MB) | free(随机活指针) | realloc(随机活指针)
每步后断言:
  ① 对齐: (uintptr_t)p % 16 == 0(8B 类允许 8)
  ② usable_size(p) ≥ 请求
  ③ free 的指针必在集合中(双释放即错)
  ④ Σ 各 bin live + 大对象 live == 集合元素数(相对进场快照的增量)
每 10 万步: 全量 free(Σ 回基线)→ 全量 alloc(Σ == 槽数)
  → 全量 free 复位,确保状态收敛(无泄漏语义)
```
- **glibc 对照模式**:同一条随机序列(同种子同代码路径 = 同一串随机数)
  喂给 glibc malloc 跑一遍,双方都必须"成功完成"(行为兼容验证)。
  序列对齐是结构保证而非运气:每步抽取次数恒定(分支/槽位/尺寸各
  固定次数,尺寸在分支前预抽、释放步抽了不用也照抽),与槽位占用
  无关——个别分配失败只记罪不错位。
- **实现勘误(初稿 → 落地,理由同代码注释)**:
  - ④ 不走 MM_DEBUG 钩子——该编译期钩子已废除,统计用测试白盒直读
    `g_bins[].live` / `g_large.live`;基线取进场快照,前序组留下的
    计数史不归 oracle 管(绝对零只许断在自己强制出的状态上);
  - 尺寸按带宽加权(35/30/15/15/5 五档,见 gen_size)而非 1..1MB 均匀:
    均匀取值 99% 落大对象路径,bin/slab 机制得不到压测;
  - 释放打"随机槽位"(活→释放,空→回退分配)而非"扫描任意活槽":
    扫描版成功率与占用率无关,净漂移恒 -1/4/步,占用率钉死在 0
    附近——首轮实跑 peak live 20/512 暴露,churn 全覆盖但活集合
    压力形同虚设。打点版均衡在 q=3/4(分配支 +1/4、释放支
    (1-2q)/2、realloc 净 0),活指针数在 384 上下随机游走;
    抽取次数不变,对照模式序列对齐不受影响;
  - 追加两条深检查:头 8 字节存储指纹(抓"存储被重复交付")、realloc
    保内容 128 字节逐字节对账(抓拷贝偏移错)。仅 wmm 侧启用——ASan
    对 malloc(1) 只放行 1 字节,指纹写法在 glibc 侧是越界;
  - glibc 对照的对齐断言放宽到 8:debug 库带 ASan,glibc malloc 实际
    由 ASan 分配器接管,只承诺 8;对照验证"吃得下同一条序列",
    不断言与 wmm 逐字段一致。

### M7.2 压力测试(test_stress.c)
- 多线程(4/8/16)各自独立随机序列,逐步通过 M7.1 的 ①②③ 与
  指纹/保内容深检查;④ 在自建自旋屏障的静止时刻对账
  (Σ 各线程模型数 == Σ 全局计数,相对进场基线)——活线程随时改
  计数,只有全体静止的瞬间等式才有意义;
- 每阶段总步数固定(20 万)按线程均分、分 20 轮,轮末对账一次;
  阶段收尾全量释放后再对一次(并发下的无泄漏语义);
- 观察 `/proc/self/statm` RSS:整体曲线应平稳(合并生效)。
- **时长勘误**:跑确定性步数预算(每阶段 20 万步)而非 10 分钟墙钟
  ——`make test` 必须可重复、可预期;长时间浸泡用同一程序挂机即可,
  无须单独模式。
- **槽位配额勘误**:槽数若固定为"每线程 128",16 线程阶段的峰值
  活指针天然是 4 线程阶段的 4 倍——峰值工作集由并发容量(线程×槽)
  决定,不由总步数决定,RSS 跨阶段持平断言必假。改为总配额 512
  按线程均分(4×128/8×64/16×32),两个量都恒定,断言才成立。
- **占用率勘误(同 oracle,首轮实跑暴露)**:释放支同样改打随机
  槽位(活→释放,空→回退分配),均衡 75%;扫描版占用率钉死在 0
  附近,活集合压力形同虚设。
- **复用断言升级(首轮实跑 RSS 余量仅 3% 倒逼)**:RSS 混入
  ASan 影子页/线程栈/线程登记等非分配器量(跨阶段 +7944KB vs
  容差 8192KB)。补白盒尖断言:阶段 2/3 的窗口 bump 指针不得越
  阶段 1 足迹 + 8MB——三阶段容量/步数/配比全同,峰值需求必落在
  既有足迹内;预算 8MB 吸收占用率随机游走的峰谷差(活集合 ~21MB,
  峰值波动天然 MB 级;oracle 同分布实测 peak 442/512)。复用失效
  特征 = 每阶段 4~5GB 流量,1% 漏复用 >40MB,与预算隔一个数量级。
  RSS 容差放宽到 20MB 作粗粒度总检(构成实测分解:线程栈 + ASan
  线程开销 join 后留驻 ~8MB + 占用波动)。

### 自测清单(M7)
- [ ] `make test` 全绿;**里程碑通过 → 允许进入 B 阶段**;
- [ ] oracle:四条不变量百万步零失败,peak live ≈ 450(占用率 75%
      均衡的观察确认,贴地数字 = 释放支退化为扫描),10 个收敛
      检查点全过,glibc 对照模式同序列无失败完成;
- [ ] stress:4/8/16 线程全部屏障对账零失败;阶段 2/3 窗口 bump
      不越阶段 1 足迹 +8MB(复用尖断言);RSS 跨阶段平稳(容差
      20MB 粗检;涨到峰值后持平是期望形态——空闲页按设计留在
      映射里等复用,真正回落要等 purge)。

---

## 10. M8 tcache `src/wmm_tcache.c`(第一部分:快路径 + 批量)

**职责**:把 95% 的分配/释放变成无锁弹栈。结构用 head 索引版(wmm_internal.h 已定)。

### M8.1 初始化与栈内存
- `g_tcache` 用 `_Thread_local`;首次使用时惰性创建:
  - **整块一次 mmap**:`[ wmm_tcache_t 结构 | 各 bin 栈数组 ]`(结构在块头,
    本身即 `g_threads` 链节点——这是 M11 合并线程计数、M15 枚举 GC 的基础);
    **不能用 wmm_alloc 自举(I4)**;线程退出整块 munmap——
    不走 base 元数据区(base 不回收,线程反复创建销毁会泄漏元数据);
  - 每个 bin 的 `ncached_max` 按公式(指南 §7.4,仅小对象):
    ```
    candidate = nregs << 1(取偶), clamp [20,200]
    ```
- 各 bin 栈空间按序排在上述整块 mmap 内;`head=0; low_water=0`。

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
- **大对象不进 tcache(第一版决策,勘误)**:原稿"bins[36] 容量 20"是从 jemalloc
  剥离语境的移植错误——jemalloc 的 20 槽属于"每个大对象类各一个 cache bin"的
  语境,弹出规格必然匹配请求;塞进单一混合槽后,要 1MB 可能弹出 16KB 的块
  (usable_size 错乱),且固定 20 槽 × 任意大的对象可囤出 GB 级缓存。大对象
  分配/释放直走物理层;将来若要缓存:释放对象首 8B 存页数、弹栈时校验
  (bins[36] 已为此预留,避免届时改 tcache 结构布局)。
- **live 语义(与指南 §10.1 I2 勘误一致)**:fill 时 bin 锁内 `live += n`、flush 时
  `live -= n`、慢路径直接分配/释放 ±1 → **live = 应用持有 + tcache 缓存**(jemalloc 同义)。
  自 M8 起,M7 oracle 断言④改为 `Σ live[i] − Σ tcache head[i] == 活指针集合数`;
  线程退出(G5)flush 归零后,`Σ live` 回到"应用持有"基线。
- **实现勘误(初稿 → 落地)**:
  - live 记账不另设批量加减:填充/刷出全部走 `wmm_bin_alloc` /
    `wmm_bin_dalloc` 这两个单点(它们已逐个 `live±1`),缓存层只搬
    指针。语义仍是"live = 应用持有 + 全部线程缓存",但不存在第二套
    需要与 bin 计数对账的批量账本;
  - `wmm_tcache_t` 增加 `map_size` 字段:线程退出按创建时的整块长度
    munmap;按容量常量重算是"将来改公式即静默越界/截断"的地雷;
  - 刷出的退休 slab 收集进本地数组,放锁后交物理层(与 wmm_free 同一
    锁纪律)。同批次退休安全性:slab 要 nfree 涨到 nregs,须其全部对象
    都在空闲链上,包括本批次尚未归还者——故只可能在它的最后一个批次
    对象上退休,之后本批次不再含它的对象;
  - 线程退出与 pthread 键析构提前到 M8 落地:缓存默认开启后,不归还
    就是每线程泄一块 mmap;`wmm_thread_exit` 幂等(显式调用后再触发
    析构是空操作),M9 只在其上叠自适应 GC;
  - pthread 键的两处失败(`pthread_key_create` 与 `pthread_setspecific`)
    一律 fatal:键是线程缓存生命周期的唯一自动闭环,失效后死线程的缓存
    对象永久滞留在 live 账上,且没有任何人(线程已死、应用无通道)能替
    它回收——这是分配器自身不变量失守,与 VA 窗口/页表 mmap 失败同一
    处置(abort),不带着未知状态继续跑;`pthread_atfork` 失败同样 abort:
    锁的 prepare/child 重建与幽灵缓存清理是 fork 契约的组成部分,缺了
    它们,多线程程序 fork 出的子进程可能带着别的线程持有的锁继续跑,
    死锁现场与根因隔着整条调用链;
  - fork 子进程侧新增 `wmm_tcache_child_cleanup`:死线程节点逐类刷空
    后 munmap,注册表只留调用线程。快路径无锁、pthread 钩子无法冻结
    它,快照可能截到"head 已加、栈项未写"的半个条目——刷出的逐对象
    窗口+页表校验正是为此,坏项丢弃(计数留一笔小泄漏,好过把野指针
    挂进空闲链)。发布顺序:先挂注册表、后写 g_tcache,截在中间时子
    进程把未发布块当死节点清掉是安全的;
  - 主路径惰性创建只挂在分配路径:纯 free 线程不为一次慢路径释放付
    一整块 mmap(跨线程释放用例覆盖)。
- **测试适配(缓存默认开启后的既有用例)**:
  - test_api 防御性释放临时关缓存:双释放检测依赖"对象已回 slab、页表
    已清",缓存快路径会静默二次收纳(glibc tcache 同款盲区,检测归
    guard);随机风暴改为两段归还(先应用 free,再 wmm_thread_exit),
    零点只在"应用持有与缓存都空"后成立;
  - test_phys 整组关缓存:该组断言依赖"释放即回 slab、全空即退休"的
    即时性(curslabs、dirty 页、重放零新映射);缓存把对象压在栈上
    会同时破坏这三者;
  - oracle/stress 的 ④ 改为扣缓存项:Σ live − Σ 缓存 head(二者均相对
    进场基线),检查点/阶段终点先 flush_all,让收敛断言仍对着真正的
    零;stress 屏障对账把 cached_total 一并扣掉,阶段收尾刷空后再发布
    归零;
  - stress 窗口复用预算补缓存容量项(首轮 16 线程实跑暴露):断言原
    前提"三阶段容量全同"在 M8 后不成立——线程缓存是唯一随线程数
    增长的容量项,每线程每类最多囤 ncached_max 个,4096B 以上各类
    nregs≤4 全被 min 钳到 20,全类合计上限 ~2.1MB/线程;尺寸分布
    15% 落在 4097..14335,阶段 3 比 pin 实涨 12.5MB(≈12 个多出线程
    × ~1MB 实际驻留),M7 时代的 8MB 定额当场击穿。对账类检查全绿
    (泄漏会先炸屏障对账与阶段归零),增量随线程数线性——定性为
    容量输入漏钉,不是复用失效。预算改为 8MB 波动基数 +
    (线程数−阶段 1 线程数) × 每线程缓存容量上限,后者从真实缓存块
    实算(公式改则预算自动跟随);min-20 容量保留(jemalloc 的
    TCACHE_NSLOTS_SMALL_MIN 同款),对囤积的根治是 M9 的 GC——
    容量与 GC 在 jemalloc 里本就是一对,M8 只落了前一半;
  - 新增 test_tcache:TC1 用"分配前后 head 是否恰降一格"判命中(填充
    事件断言 head == nfill−1),10 万次 churn 命中率 ≥ 90%;TC2 逐步
    断言 head ≤ ncm、满栈释放的落点恰为 ncm/4+1;另覆盖容量公式、
    整块布局与 map_size 对账、注册表增删、线程析构归零、enabled 关闭、
    跨线程慢路径释放、fork 单线程过继与 fork 幽灵缓存清理(子进程只以
    退出码汇报,不碰 stdio)。

### 自测清单(M8)
- [ ] TC1:单线程分配 10 万次,命中 tcache(判据:分配前后 head 恰降一格)≥ 90%;
- [ ] TC2:任何时刻 `head ≤ ncached_max`;满栈释放按 3/4 部分刷出;
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
- **填充目标自适应**(必做,共 ~6 行,照指南 §7.5):每 bin 存 `nfill`
  (初值 `ncached_max>>1`,下限 2);GC 触发后 `nfill >>= 1`,连续填满则 `nfill <<= 1`
  (封顶 `ncached_max>>1`)。

### M9.2 线程退出(必做,指南 §7.6)
```c
void wmm_thread_exit(void) {
    for (i in 0..35) if (head>0) wmm_tcache_flush(i, head);
    munmap(tcache 内存); g_tcache = NULL;
}
```
- 注册:`__thread` 析构函数 或 `pthread_key_create` 析构;
- **验证 G5**:线程反复创建销毁后,各 bin `live` 计数与"无 tcache"基线一致。

### 自测清单(M9)
- [ ] TC3:人为"偶尔用一次"的 bin,GC 后 `head ≤ retain` 公式值;
- [ ] TC4/G5:1000 次线程创建/退出,`Σ live` 不变、RSS 不涨;
- [ ] M7 oracle + 多线程压力重跑全绿。

### 实现勘误(M9 落地时记)
- **满栈触发(触发点 a)落地为"先 GC,热 bin 退回 3/4 兜底"**:retain 顶到
  容量(用量 ≥ 容量)时 GC 无处可刷,原样压栈会越顶(TC2 必炸)——退回
  M8 的 3/4 部分刷出保证栈位。落点 = `min(retain, ncm/4 兜底) + 1`,
  TC2 改为按公式重放断言(测试里 `lw_tracked` 镜像 low_water);
- **轮转触发(触发点 b)计数器放释放路径,不放"每 N 次分配"**:囤积只
  发生在 free 侧(分配只会把 head 往下推),每 `WMM_TCACHE_GC_EVERY`
  (256)次 free 轮转 GC 一个 bin;快路径代价仅一条普通计数器自增;
- **"连续填满"信号 = `low_water == 0`**(这段窗口里缓存被完整吃空过):
  GC 时低水位为 0 即供不应求,nfill 翻倍(封顶 ncm/2);否则减半(下限
  2)。与 jemalloc 按低水位调 fill-div 同构——"填满事件"无法直接观测,
  "曾经到过 0"是它的白盒等价物;
- **新增内部入口 `wmm_tcache_gc(i)`**:满栈/轮转共用同一个 `tcache_gc_bin`,
  白盒测试直调(TC3),将来 wmm_ctl("gc") 复用;
- **M8 的 TC2 落点断言被 GC 取代**:M8 固定"刷 3/4 留 1/4"只在热 bin
  兜底时保留;TC2 现在的精确落点序列由 GC 公式重放(33/121/11 等随
  low_water 演化),公式若改测试同步改;
- 新增字段 `gc_evt`/`gc_next` 进 `wmm_tcache_t`(块尺寸运行期计算,
  布局测试自适应当前 struct 尺寸,不冻结 ABI);
- 测试侧两处 M8 假设被自适应打破:
  - "nfill==ncm/2" 只对新建块成立——主线程的块被 stress 等前序用例
    复用,nfill 早被磨过;创建公式改由 lifecycle 的新线程块白盒报告
    断言,容量公式只钉自适应范围 `[2, ncm/2]`;
  - lifecycle 的批次大小改取工作线程自己块的 nfill(填充用的是它
    自己块的值,主线程的 nfill 与之不再相等);
  - churn worker 必须释放全部对象:析构只回收缓存存量,应用手里
    的局部数组指针线程退出后无人释放(首轮实测每线程漏
    4×64B+2×4096B,live 随千次增删漂移)。

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

### 实现勘误(M10 落地时记)
- **后台线程默认不启动**:boot 不建线程,首次 `wmm_ctl("bg.enable")`
  才 `pthread_create`;`bg.disable` 置 stop 标志并 broadcast,
  `pthread_join` 返回后线程确已退出。周期等待用 `CLOCK_REALTIME`
  绝对时刻 + `pthread_cond_timedwait`,内层 while 吸收虚假唤醒且
  不重置周期;1s 粒度下时钟跳变最多让某轮早到/晚到一次,不值得
  为此引入 condattr 的单调时钟设置;
- **purge 的遍历与 madvise 都在 `g_ext_lock` 内**:单 arena 下把
  "移出链→解锁→批量回收→挂回"做对需要暂存数组与重挂逻辑,复杂度
  换来的并发收益在千级 extent 上不成立;madvise 失败(返回非 0)
  保持 FREE_DIRTY,下一轮重试,宁可多试不谎报;
- **跨状态合并记账(引入 RETAINED 后的新不变量)**:释放的 DIRTY
  块与 RETAINED 邻居合并时,存活结点状态归一到 FREE_DIRTY,被并
  的 RETAINED 页数补回 `g_dirty_pages`(下轮重复 madvise 是空操作);
  否则状态标签与计数脱节,`ar_check` 的页数守恒立刻失败,漏记的
  页也永远逃过 purge;
- **fork**:`prepare` 不拿 `g_bg_lock`——后台线程对分配器共享结构
  的触碰全部在 `g_ext_lock` 内,已被冻结;`atfork child` 调
  `wmm_bg_child_reset` 重建控制锁/条件变量并清零状态(子进程里
  没有该线程,join 继承来的 tid 不可移植);
- **extent 复用改两趟扫描**:先只在 FREE_DIRTY 里 first-fit(物理页
  还在,复用零系统调用),没有再接受 FREE_RETAINED(复用要吃缺页)。
  没有 purge 过的进程第一趟就是全集,行为与单趟一致;
- **ctl 接线**:`purge` 同步回收;`bg.enable/disable` 启停;
  `config.madvise_mode` 取 int 变参,只接受 MADV_DONTNEED/
  MADV_FREE(否则 EINVAL),只影响之后的 purge。其余命令仍 ENOSYS。
  测试只钉参数校验与模式切换:MADV_FREE 的物理回收效果依赖内核
  版本,不做断言;后台线程随进程退出终止,显式 shutdown 走
  bg.disable(join),不注册 atexit;
- **自测落地**:PHY2 用 256×4MB=1GB 每页写过后同步 purge,断言
  dirty 归零、页数守恒转 RETAINED、RSS 实测回落 ≥512MB、RETAINED
  复用零新映射;另补"跨状态合并"白盒用例。后台线程用 32×4MB:
  未启动等 1.3s 脏页不动 → enable(重复调用幂等)→ 2 秒内归零 →
  disable(重复调用幂等)→ 停后再攒 128MB 等 1.3s 不动 → 再启清尾。
  任务书的 `active_bytes` 断言待统计打点后补,M10 阶段以
  `g_dirty_pages` + RSS 替代。

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
- **自检断言(测试钩子,始终开启)**:`Σ_i live[i]*reg_size[i] + large.live_bytes == st->live_bytes`(OBS1;live 含 tcache 内对象,口径见 M8.5)。
- `threads[128]` 超员:第 129+ 个线程的计数合并进 `threads[127]`(聚合桶,tid 记 0)。

### M11.3 事件日志
```c
#define WMM_EVENT_RING 1024
typedef struct { uint64_t ts; uint32_t type, tid; uintptr_t ptr; uint32_t cls;
                 uintptr_t pc0, pc1; } wmm_event_t;   /* 定义以 include/wmm.h 为准 */
void wmm_event_log(uint32_t type, void *ptr);   /* 单生产者无锁,写指针原子++ */
int  wmm_event_drain(wmm_event_t *buf, size_t n, size_t *got);
```

### 自测清单(M11)
- [ ] OBS1:oracle 每步后钩子为真(打开 MM_DEBUG);
- [ ] 事件:人为触发 OOM/异常路径,`wmm_event_drain` 能取到;
- [ ] 快照字段与手工统计一致。

### 实现勘误(M11 落地时记)
- **事件环用互斥锁而非"单生产者无锁写指针"**:多生产者是事实——任何
  线程的错误路径(双释放/OOM)和后台线程的回收动作都会写。事件是低频
  路径,一把锁的代价可忽略;多生产者无锁环要 per-slot 就绪标记与读端
  幂等逻辑,复杂度换不来可测收益。环满覆盖最旧(排障时新事件值钱)。
  锁序 ext → event:purge 在 `g_ext_lock` 内记 0xDC,页表反查不加锁,
  不存在反向路径;
- **计数埋点在 wmm_alloc/wmm_free 出口,按可用大小计**:小对象记
  `reg_size`,大对象记页对齐字节;快慢路径共用出口、口径一致(同一次
  调用的两处计数互斥)。bin 层 live/ndalloc 在 bin 锁内(既有);大对象
  live/live_bytes 在 ext 锁内;`allocated/deallocated` 合并自线程局部
  计数,允许瞬时偏差——分配快路径零同步是换来这个的代价;
- **大对象路径的计数槽惰性补建 tcache**:`stats_slot()` 在没建过缓存的
  线程上补建一次;仍失败就放弃计数——统计口径允许偏差,不值得为计数
  影响分配成败;
- **active_bytes 语义 = 堆窗口内已映射未回收的字节**(页对齐,不含
  RETAINED),在五处增减:窗口映射成功、purge 成功、extent_get 切出
  RETAINED 区、跨状态合并两侧归一。`live ≤ active` 与
  `frag == active − live` 因此恒成立;M10 阶段以 `g_dirty_pages` + RSS
  替代的 active 断言由此补上;
- **快照锁序 bins(逐个)→ ext → threads**,与 fork prepare 的顺序
  一致,不会与任何持锁路径构成环。线程表遍历 `g_threads` 链(持
  `g_threads_lock`),计数本身是无锁写——快照口径接受瞬时旧值;
  第 129+ 个线程合并进 threads[127](tid 记 0);
- **stats.per_class 只填 cls[]**:不合并线程计数、不动 epoch/线程表,
  供监控高频轮询直方图;epoch 与线程表由 stats.snapshot 负责;
- **0xBE 双释放检测复用页表反查**:大对象释放后页表整块清零,再释放
  时 raw==0,与"从未分配"统一走同一条诊断路径(记事件+打印,不
  abort——契约上跨分配器释放是 UB,但 UB 不等于活该崩溃);
- **0xDB 覆盖物理层全部返回 NULL 的出口**:窗口上限挡板、窗口耗尽、
  mmap 失败、extent 元数据耗尽、slab 头元数据耗尽——每条失败都在
  事件里留痕,诊断 OOM 时不必靠猜哪个环节挡下的(初稿只在窗口/mmap
  失败处记,落地时补全为一条统一规则);
- **OBS1 的测试口径是独立重算,不是拿快照自己的求和重放**(重放恒真,
  测不出东西):test_stats 直读 `g_bins`/`g_large` 与快照逐字节对照;
  oracle 每步新增字节版 ④(模型按实测 usable 累计字节,扣缓存层后与
  直读对账),快照与直读的对照在检查点与终局执行——快照要 37 把锁加
  5KB 清零,每步做会把 oracle 时长拉长数倍,而逐步等式读的已是快照
  所读的同一批全局变量;
- **ctl 接线**:`stats.snapshot`/`stats.per_class` 取指针变参,
  `config.madvise_mode` 取 int 变参(默认提升);其余仍 ENOSYS;
- **自测落地**:test_stats 先重建并禁用本线程缓存(释放即回 bin,直读
  全局与快照可比),组尾恢复;增量断言取"快照差 = 手工工作量"而非
  绝对值(进程内前序组的计数史不归本组管)。事件用例以 1TB 请求(越
  窗口上限)、窗口外指针、大对象双释放分别触发 0xDB/0xBE,断言字段
  非空与取出即消费;线程表用例必须在线程存活时快照——退出即摘链
  munmap,行消失本身就是断言的一半。
- **被同一恒等式绑定的字段必须同源取值(并发压力测试暴露的缺陷)**:
  快照曾把 `cls[36]` 在 ext 锁内赋值,却在解锁后重新读 `g_large.live`
  / `g_large.live_bytes` 去累加 `live_objects` / `live_bytes`——两处
  取自两个瞬间,并发分配/释放大对象时
  "Σ cls[i].live == live_objects" 会破。共享内存压力测试(发布线程
  连续出代 + 写者不停跨小/大对象分配)以约 4% 的接受读把它打了出来;
  单线程的逐字节对照反而测不到(无人并发改 `g_large`)。修法:锁内
  一次读到局部变量,`cls[]` 与汇总共用同一份读数。

---

## 14. M12 共享内存监控 `src/wmm_shm.c`

### M12.1 布局(冻结 ABI,指南 §11.1)
```
0x000  magic "WMMST" + version(4+4)
0x010  seqlock(uint64, 奇数=写入中)
0x020  wmm_stats_t 主快照(sizeof==0x13B8,wmm.h 内 static_assert 锁死)
0x1800 TOP N 采样表(256 项 × 32B = 0x2000;项 = {pc, bytes, count, reserved},
       名字解析在监控端 addr2line)——M13 填
0x4000 wmm_event_t 环(1024 × 48B = 0xC000;sizeof==48,static_assert 锁死)
合计映射大小 0x10000(64KB)
```
- 建:内存映射文件(`memfd_create` 或临时文件)+ `ftruncate(0x10000)` + 映射;监控进程 `mmap` 同文件只读;
  (勘误:① 原稿 TOP N 在 0x800 会被主快照区覆盖——wmm_stats_t 实占 0x13B8,故挪至 0x1800;
   ② 原稿事件环按 32B/项计算有误——wmm_event_t 含两个 uintptr_t 调用点帧,实为 48B,
   环 0xC000,映射总大小 0x10000;两处尺寸均已由 wmm.h 的 static_assert 锁死)
- 后台线程每 1s `wmm_shm_publish()`:锁 seqlock → memcpy 快照 → 解锁。

### M12.2 seqlock 读端(监控进程/测试用)
```c
do { s1 = seq; memcpy(local, snap); s2 = seq; } while (s1 & 1 || s1 != s2);
```

### 自测清单(M12)
- [ ] SHM1:另开一个进程映射同一文件,读到与进程内 `wmm_stats_snapshot` 一致的快照(epoch 对齐);
- [ ] 高并发分配时读端永不看到撕裂数据(seqlock 生效,压力下验证)。

### 实现勘误(M12 落地时记)
- **具名文件 + `shm.create` 命令**:不用 `memfd_create`——监控进程要在
  不知道目标 pid 的前提下按约定路径找到文件,发布者退出后文件仍在
  (残留的最后一代数据对事后分析有用)。命令带 `const char *` 路径变参;
  一个进程只发布一份,第二次创建 -1/EEXIST;不做 destroy(撤写权意味着
  监控可能正读半截数据,"换文件"用重跑进程更省事)。open/ftruncate/mmap
  任一失败都 unlink 掉半截文件:0 字节文件对读端是 SIGBUS 陷阱;
- **头部空闲字节的用法**:0x008 = `events_total`(单调只增,环的写游标
  恒等于 total % 容量,读端据此反推最新 k 条的数组下标,不需要再发布
  游标);0x018 = `publish_ts`(CLOCK_MONOTONIC 纳秒,读端拿同源时钟
  判数据新鲜度)。magic 取 "WMMS"(4 字节):原稿 "WMMST" 五个字母塞不
  进 4+4 的头;
- **发布分段**:快照要逐 bin 持锁合并(37 把锁)、事件环拷贝要持事件锁,
  两者全部放在 seqlock 写段之外——写段里只有 memcpy 与序号操作,写段
  越短读者撞上奇数序号的概率越低。事件环先在事件锁内拷进 48KB 静态
  中转区,再整环 memcpy 进映射(两侧容量由 static_assert 钉住);
- **单写者契约**:运行时写者是后台线程(与回收同周期:先 purge 再发布,
  读到的 active/frag 是回收之后的状态);创建时由调用线程同步发布一代
  ——这次发布发生在 `g_shm` 指针公开之前,与后台线程不可能并行,监控
  在 create 返回后立刻有数。创建动作本身由一把创建锁串行;
- **fork 子进程放弃写权**:`wmm_shm_child_reset` munmap 掉继承来的写
  映射并把 `g_shm` 置空。父子若同时发布,seqlock 序号交错,读者会把
  两代数据拼成一份从未存在过的快照。子进程要读监控数据,自己按路径
  只读 mmap 即可(读端天然多读者);
- **读端有界重试**:100 次后返回 -1/EAGAIN——写者卡死(进程被杀在写段
  中间,序号永远为奇)时宁可让监控显示"数据陈旧",也不无限空转。
  `wmm_shm_read` 一次取齐 stats/events_total/publish_ts:分两次调用会
  拿到两个发布代的数据,协议不保证跨调用一致;
- **自测落地**:test_shm 的子进程不 boot 分配器,只按路径 open + 只读
  mmap 当外部监控,且等父进程 fork 之后再发布一代才读——证明发布经
  MAP_SHARED 跨进程可见,两进程解析结果逐字节一致(epoch 归零后
  memcmp);压力组三线程(写者猛分配+写事件、发布者空转刷、读者校验),
  每次成功的读验单代恒等式(Σ cls.live == live_objects、小对象字节 ≤
  live_bytes、frag == active − live),这些恒等式跨代不成立,撕裂必被
  抓住;EAGAIN 是合法结果,计数不算失败。事件环顺序检查按逻辑序
  (total−k .. total−1 取模)走时间戳:时间戳在取锁前采样,多生产者时
  可能逆序,本组单生产者故理论上严格不减,仍留每次 1 次逆序余量。

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

### 实现勘误(M13 落地时记)
- **触发口径改为全部成功分配,不是只挂 bin 慢路径**:×N 放大的估计器
  要求对全体分配均匀抽样。只抽慢路径会系统性低估缓存命中的热调用点,
  也完全漏掉大对象——大对象不经过 bin,却是 OBS2 里最典型的吃内存
  场景。大对象、缓存命中、bin 三个成功返回点共用同一入口;关闭时
  每个点只多一次可预测的全局标志测试。字面"零额外指令"要运行期
  改写代码,代价与收益不成比例( jemalloc 同样保留这个测试),所以
  自测清单第二条改成"关闭时环形缓冲 head 不动 + 反汇编可见一条测试";
- **`sampling.enable/disable` 都不带变参**:间隔固定 1000,不开放任意
  间隔——间隔是估计精度的旋钮而不是业务配置,固定值让不同进程的
  TOP 表口径一致。无参还保证 M14 的自动开启可以直接
  `wmm_ctl("sampling.enable")`,不会从变参表里读到不存在的实参;
- **环嵌在线程缓存块里**:随线程整块 mmap/munmap,线程退出不必为它
  单独管生命周期。关闭采样时只占虚拟地址:mmap 零页不被触碰就不
  驻留物理内存,因此刻意不逐槽 `atomic_init`(256 个槽横跨 16KB,
  触碰一遍就付 16KB RSS);槽的 seq 零值即"偶数、未写入",对这些
  无锁原子类型是合法表示。每槽一把微型 seqlock,写者置奇 → acq_rel
  栅栏 → 写字段 → release 栅栏 → 置偶,head 最后 release 发布;
- **槽位带绝对序号 index**:合并器按"从 merged 到 head 的每个绝对
  序号"逐个索取,只有槽里现存的 index 正是所求才收。仅在围圈时靠
  序号会重复计数——同一槽位的上一代样本会被当成新样本;index 比对
  把两代样本区分开。环绕圈时游标直接推到 `head − 256`,最旧的样本
  按设计丢弃(每秒 25.6 万次以上的分配才会触发);
- **样本自带权重**:weight 在采样时刻取自 `g_opt.sample_n`,运行期
  改间隔不会把旧样本按新间隔放大;
- **帧地址由 wmm_alloc 自己捕获并传参**:`__builtin_frame_address(0)`
  描述的是求值它的那个函数。放在遍历器里取,debug(-O0)构建拿到的是
  遍历器自己的帧,翻一帧落在采样器而不是分配器上;放在采样器里取
  也不够:它保存的是 wmm_alloc 的 rbp,而 wmm_alloc 自己没取过帧
  地址时,编译器可能把 rbp 挪作通用寄存器,保存下来的不是帧指针。
  因此帧地址由 wmm_alloc 在采样分支内取(收进只在该函数体内展开的
  宏,防止被误抽成辅助函数),传给采样入口,回溯从该帧的 `[1]`
  开始——正是用户对分配 API 的调用点,与中间层的帧布局、内联与否
  都无关。取帧地址这一事实在编译期就强制编译器为 wmm_alloc 建立
  规范的帧指针链,即使所在分支运行时未执行:只有
  `-fno-omit-frame-pointer` 并不够,`-O2` 或 ASan 构建仍可能把 rbp
  挪作通用寄存器,链上保存的就不是上一帧的帧指针(本地探针在
  MinGW GCC 上复现过该行为);放在分支里则让关闭采样的路径不再
  付出入口那次存储;
- **捕获深度收敛到键宽**:聚合只用前 4 帧,更深的帧在合并时被丢弃,
  而环是瞬态的(合并后槽位立刻被下一代覆盖),32 帧没有消费者,
  只让每槽 288B、每线程环约 72KB。现在 `WMM_PROF_DEPTH` 就是
  `WMM_PROF_KEY_FRAMES`:槽 64B、每线程环约 16KB。将来真要做完整
  栈转储,再单独放大捕获深度,聚合侧的 4 帧钳位已经留着;
- **键折叠前 4 帧,但桶内存原始 PC**:哈希只用于选探测起点,命中后
  逐帧比较前 4 帧的原始地址。哈希碰撞若直接合并会把两个无关调用点
  算成一行、给出错误归因;多存 32 字节就完全避免。1024 个桶,表满
  丢弃新样本——采样路径不能向分配器申请内存(自举);
- **锁序**:聚合表锁是叶子锁。后台合并先持 `g_threads_lock` 再取
  `g_prof_lock`,与 fork prepare 的 bins → ext → threads → prof →
  base 一致;线程退出只单独取聚合锁。fork child 重建聚合锁,并在
  清理死线程时先把它们环里的样本归账再 munmap;
- **线程退出先 drain 再 munmap**:环嵌在缓存块里,不取出来就随块
  消失,最多丢一个合并周期(1s)的样本。drain 由 pthread 析构自动
  触发,显式 `wmm_thread_exit` 同一条路;
- **输出**:`wmm_ctl("prof.top", int k, wmm_prof_entry_t *out)` 先同步
  合并再取数,返回写入条数(0..k),out[n..k) 清零;k 限 1..256,
  参数非法 -1/EINVAL。取数用插入选择而非全表排序,不分配内存。
  `wmm_prof_entry_t` 固定 32 字节,同时是共享区 0x1800 的线上格式;
  `wmm_shm_read_prof` 用同一把 seqlock 整表读出。发布端在写段之外
  取 TOP(要合并、持聚合锁),写段里只 memcpy;
- **已知缺口**:档 3 对齐分配(alignment > 4096)直走物理层、不过
  `wmm_alloc`,不在采样口径内;档 1/2、calloc 与需要搬移的 realloc
  都经过 `wmm_alloc`,正常采样。应用若省略帧指针,第一帧(直接
  调用点)仍准确——它来自 wmm_alloc 自己的帧;更深的帧可能提前
  断开,遍历的单调性/跨度检查会把坏链当终点而不是收下;
- **自测落地**:不按函数地址断言(采样 pc 是函数体中部的返回地址,
  ASan 插桩又改变布局,按地址框定比契约更严),改为每个吃内存函数
  用一个专属尺寸、按 `bytes == count × size` 唯一识别聚合行;断言
  包含:4000 次 1MB 分配 → count 4000 且排在小尺寸调用点之前;
  同一调用点多次采样只有一行;30 万次 128B 分配后 count 精确等于
  256×1000(环绕圈丢 44 个);工作线程 12000 次分配在 join 后完整
  可见;关闭采样后 head 不动;pc 打印出来供 addr2line 人工对名。

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

### M16.2 `bench/bench.c`
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
