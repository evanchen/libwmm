# jemalloc 深度定制 fork 技术设计文档

> 基线:上游 jemalloc `master`(5.4.0 之后),fork 领先 **46 个提交**(dev 分支,HEAD `fde6a21a`),
> 涉及 116 个文件、约 +2875/−695 行。
> 视角:工程设计与实现。目标是还原整体设计框架、逐模块功能设计,以及模块间的交互与耦合关系。

---

## 目录

1. [项目概览与工程主题](#1-项目概览与工程主题)
2. [总体架构:分层与模块地图](#2-总体架构分层与模块地图)
3. [模块功能设计](#3-模块功能设计)
   - 3.1 公开接口与配置层
   - 3.2 大小类与地址映射层
   - 3.3 核心分配层
   - 3.4 页面供给层(物理层)
   - 3.5 并发基础设施层
   - 3.6 高级功能层
4. [模块交互与耦合分析](#4-模块交互与耦合分析)
5. [fork 定制设计](#5-fork-定制设计)
6. [工程设计与实现评价](#6-工程设计与实现评价)
7. [附录:测试体系与构建](#7-附录测试体系与构建)

---

## 1. 项目概览与工程主题

jemalloc 是工业级多线程内存分配器,核心设计理念是 **arena(隔离)+ tcache(线程缓存)+ extent 缓存分级**:
把并发压力分散到多 arena,把热分配收敛到线程本地,把页面往返 OS 的开销用多级缓存吸收。

本 fork 在 5.4.0 之上的 46 个提交,工程主题可归纳为四条相互咬合的主线:

| 主线 | 代表提交 | 目标 |
|---|---|---|
| 性能快速路径 | `8487209a` bitmap group 批量填充、`e5b2ff07` cfs_lu、`2fee33d2` emap 快速路径、`3d07f2b5` dallocx 出线、`c42af462` 移除 last-thread 捷径、`8a2aa363` TCACHE_NBINS_MAX 收紧 | 压低 malloc/free 快路径延迟 |
| 可观测性 | `e0131540` otel_memory USDT、`55469566` bpftrace 脚本、`adfe588f` prof_bt_max:0 | OpenTelemetry/eBPF 外部采样 |
| 可配置性 | `a0863b3a` 动态 page size(lg_page)、`cf35c74d` MALLOC_CONF_TEST | 运行时适配不同页大小 |
| 平台适配 | `d4c9f03e` 无 RTLD_NEXT、`84286c27` Illumos、`344b5eb3` in-tree 头优先 | 跨平台可编译性 |

其中**动态 page size** 是最宏大的主线:它把几十个编译期常量(LG_PAGE/PAGE/HUGEPAGE_PAGES 及由其派生的 SC_NBINS、TCACHE_NBINS、rtree 层数、edata 尺寸)改为运行时值,并反向迫使静态初始化器运行期化、edata 动态收缩。

---

## 2. 总体架构:分层与模块地图

```
┌───────────────────────────────────────────────────────────────────────┐
│ L0  公开接口与配置       jemalloc.c  malloc_dispatch  zone  jemalloc_cpp│
│     初始化/配置/控制    jemalloc_init  jemalloc_fork  conf  ctl        │
├───────────────────────────────────────────────────────────────────────┤
│ L1  核心分配             arena  arenas_management  bin  bin_info      │
│     大小类/地址映射      tcache  cache_bin  large  extent              │
│                         sc  sz  emap(rtree)  edata  slab_data          │
├───────────────────────────────────────────────────────────────────────┤
│ L2  页面供给(物理层)     pa  pa_extra  pac  hpa  hpa_central  hpdata   │
│                         psset  ecache  eset  sec  exp_grow            │
│                         base  edata_cache  pages  ehooks              │
│                         extent_dss  extent_mmap  san  san_bump        │
├───────────────────────────────────────────────────────────────────────┤
│ L3  并发与基础设施       mutex  witness  atomic  tsd  rtree           │
│                         ph  rb  ql  qr  bitmap  bit_util  ckh  hash   │
│                         prng  fxp  div  ticker  spin  counter         │
│                         mpsc_queue  util  assert                      │
├───────────────────────────────────────────────────────────────────────┤
│ L4  高级功能             prof  prof_data  prof_sys  prof_log          │
│                         prof_recent  prof_stats  prof_hook            │
│                         decay  background_thread  thread_event        │
│                         peak_event  percpu_arena  stats  inspect      │
│                         safety_check  test_hooks  malloc_io  log      │
│                         emitter  buf_writer                            │
└───────────────────────────────────────────────────────────────────────┘
          ↑ 依赖方向:上层调用下层;同层间经 edata/PAI/事件等契约交互
```

**分层原则**:越往下越接近 OS、越"重";越往上越热、越"轻"。上层(arena/tcache)只认 `edata_t` 与 PAI 接口,不直接触碰任何 OS 调用;下层(页面供给)只负责把页面变成 edata、把 edata 变成页面,不理解分配语义。

各层一句话职责:

- **L0**:把 C/C++ 分配器 ABI 翻译成内部调用;管理全局初始化、配置、运行期查询(mallctl)。
- **L1**:把"请求大小"翻译成"size class",把内存按 slab 切分/回收,线程缓存与 arena 协同。
- **L2**:管理内存的物理供给:从 OS 拿页、按 dirty/muzzy/retained 分级缓存、合并、purge、大页(HPA)。
- **L3**:提供锁、原子、线程局部存储、地址→元数据映射、通用容器与数学原语——所有上层的地基。
- **L4**:横切关注点:剖析、后台回收、采样、统计、安全、平台集成,通过钩子/事件插在 L0/L1 上。

---

## 3. 模块功能设计

### 3.1 公开接口与配置层

#### jemalloc.c —— 公开 API 面与快/慢路径分派

职责:导出 `je_malloc / je_free / je_mallocx / je_dallocx / je_sdallocx / je_realloc / je_posix_memalign / je_mallctl` 等全部 C ABI(`include/jemalloc/jemalloc_protos.h.in`),以及 C++ 的 operator new/delete(`jemalloc_cpp.cpp`)。

设计要点:

- **标志编码**(`jemalloc_macros.h.in:15-33`, `internal_types.h:34-57`):`MALLOCX_*` 把对齐、ZERO、TCACHE、ARENA 压进一个 `int flags`——`LG_ALIGN` 低位、`ZERO=0x40`、`TCACHE` 在 bit8、`ARENA` 在 bit20(12 位、上限 4095)。慢路径统一由 `static_opts_t / dynamic_opts_t`(jemalloc.c:306-390)解包,再用 `imalloc_body`(jemalloc.c:549)执行:溢出/对齐校验→zero 处理→对齐尺寸→prof 分派→thread event 计数。
- **快路径内联**(`inlines_c.h:286-368` `imalloc_fastpath`):size≤4K 走 `sz_size2index_usize_fastpath` 一次拿 ind+usize,再 `cache_bin_alloc_easy/alloc` 直接命中 tcache;未命中才尾调用慢路径。慢路径整体出线,保持快路径无栈帧。
- **malloc_dispatch**(`malloc_dispatch.c`):fork 重组核心原语,提供 `malloc_dispatch_malloc/dalloc/ralloc/palloc`,内部处理 small/large 分流、prof 提升对象的 demote 释放、no-move 重分配等。

#### jemalloc_init.c —— 分阶段初始化

初始化分四阶段(`malloc_init_hard`,jemalloc_init.c:536):

1. **a0_locked**(:156):自举核心——pages→sc→conf→san→sz→emap→ctl→arena_boot→tcache→`narenas=1` 脚手架,先让分配器可用。
2. **recursible**(:332):探测 `ncpus`、注册 `pthread_atfork`(:372),处理递归初始化(init 期间的分配)。
3. **malloc_init_narenas**(:421):percpu arena 校验、`MALLOCX_ARENA_LIMIT` 截断、`percpu_arena_boot`。
4. **finish**(:512):组装 `malloc_slow_flags`(哪些特性开启导致必须走慢路径)。

构造器 `jemalloc_constructor`(:646)保证库加载即初始化。

#### jemalloc_fork.c —— fork 一致性

`prefork` 按 **9 阶段**依次锁住 arena 及各全局资源(:74-111),`postfork_parent/child` 逆序释放;parent/child 靠 pid 判别,避免子进程只拥有父进程一个线程的锁视图导致死锁。

#### conf.c —— 配置解析

- 手工解析 `key:value` 逗号分隔串(`conf_next`,:80),**5 级优先级**来源(`obtain_malloc_conf`,:303):编译期 `config_malloc_conf` → `je_malloc_conf` → `/etc/malloc.conf` → `MALLOC_CONF` 环境变量(secure_getenv)→ fork 新增的弱符号 `je_malloc_conf_2_conf_harder`(jemalloc.c:58)。
- 前缀匹配 + `CONF_MATCH/CONF_HANDLE_*` 宏逐项处理(:416);**两阶段**:`malloc_conf_init_pre`(:1180)先解析 abort/confirm 类选项,`malloc_conf_init`(:1189)再全量解析 + 依赖检查(:1156)。

#### ctl.c —— mallctl 运行期控制

- 静态名字树 `super_root_node`(ctl.c:971),节点分 named/child 与 indexed(`arena.<i>`)。`ctl_lookup`(:1585)按 `.` 逐层分解并同步填 MIB。
- **MIB 快路径**:`mallctlnametomib` 一次翻译,之后 `mallctlbymib → ctl_bymib → ctl_lookupbymib`(:1723)免字符串比较直达回调。
- `ctl_arenas` 快照(ctl.h:102):`2 + MALLOCX_ARENA_LIMIT` 个槽——0=ALL(合并统计)、1=DESTROYED、其余为各 arena。

### 3.2 大小类与地址映射层

#### sc —— size class 体系(编译期生成)

size class 是分配器的几何骨架,由少量宏在编译期推导(`sc.h`):`SC_LG_NGROUP=2`、`SC_LG_TINY_MIN=3` → `SC_NGROUP=4`、`SC_NTINY=LG_QUANTUM−3`、`SC_NPSEUDO=4`;64 位 4K 页下 `SC_NBINS=232`、`SC_LARGE_MINCLASS=PAGE*NGROUP`、`SC_LOOKUP_MAXCLASS=1<<12`。

`size_classes()`(sc.c:85-253)按顺序生成三类:
1. **tiny**:最小类起每档 +1 个(满足对齐保证);
2. **首个伪组**:从 quantum 起连续 NGROUP 个;
3. **常规组**:lg_base 逐组 +1,组内 ndelta 1..NGROUP 均分。

`slab_size()`(sc.c:22-51)用最小公倍数法算每个小类的 slab 页数(保证区域内不浪费)。每个 `sc_t` 记录 index/lg_base/lg_delta/ndelta/psz/bin/pgs/lg_delta_lookup。

**耦合**:sc 是地基,被 sz(建表)、bin_info(reg_size/nregs)、arena_boot(div_info、bin 偏移)、tcache 消费。fork 为其新增 `*_FOR(_lg_page)` 宏族使这些值可随页大小变化(见 §5.3)。

#### sz —— 大小查找表

维护三张 cacheline 对齐表:`sz_pind2sz_tab`(页大小索引→大小)、`sz_index2size_tab`(sc 索引→大小)、压缩的 `sz_size2index_tab`(请求大小→sc 索引,按 tiny_min 除缩),均由 `sz_boot` 从 sc_data 填充。

- `sz_size2index()`(sz.h:220):size≤4K 查表,否则 `sz_size2index_compute_inline`(:164)按 lg_floor 分组位运算;
- 快路径 `sz_size2index_usize_fastpath`(:299)一次得到 ind+usize;
- `sz_s2u / sz_sa2u` 计算可用大小/对齐;`sz_psz2ind` 用于页大小量化;`sz_large_pad` 做 cache-oblivious 填充。

**耦合**:全分配链路的第一个动作就是 sz 映射,是真正意义上的"入口热点"。

#### emap —— rtree 的分配器封装

`emap_t{rtree_t}` 是全局地址反向映射(全局单例 `arena_emap_global`,arena.c:37)。每个 rtree 叶存 `rtree_contents_t` = edata 指针 + metadata(szind/slab/state/is_head,emap.c:143-157)——**这就是 alloc_ctx 的编码来源**。

- 快速路径:`emap_alloc_ctx_try_lookup_fast`(emap.h:336,free 用)、`emap_alloc_ctx_lookup`(:259,先 L1 快读再慢读);批量查 `emap_edata_lookup_batch`(:380)供 tcache flush。
- 边界/内部注册:`emap_register_boundary`(emap.c:159)、`emap_register_interior`(:184);合并/扩展前用 `emap_try_acquire_edata_neighbor`(:42)把邻居置 merging 态,防并发撕裂。

#### edata —— extent/slab 的统一元数据

`edata_t` 是**全系统最核心的共享载体**,128 字节对齐:

- 64 位 `e_bits` 位域(edata.h:159-246)一字段打包:arena_ind/slab/committed/pai/zeroed/guarded/state(3b)/szind/nfree/binshard(6b)/is_head/pinned;
- 后随 `e_addr`、`e_size_esn`、`e_ps`(HPA 页数)、`e_sn`(序列号,供合并);
- 三个 union 复用空间:`active 链表/配对堆`、`ql_link_inactive | e_slab_data | e_prof_info`(edata.h:297-309)——**同一块内存,活跃时挂 arena/binc,不活跃时挂 ecache/eset 或 edata_cache**。

`slab_data_t`(slab_data.h:8-11)是每 region 占用位图,由 bin 读写。fork 在动态页大小下让 `edata_boot`(edata.c:10-25)按实际页大小收缩尾部 bitmap,避免按最大页预留导致 edata 膨胀到 256 字节。

### 3.3 核心分配层

#### arena —— 分配中枢

`arena_t`(arena.h:60-144)内含:双 nthreads、stats、**内嵌 pa_shard**(通往整个物理层)、base、`large` 列表 + large_mtx、内嵌 `all_bins[]`。

- **入口**:`arena_malloc_hard`(arena.c:917)按 slab 标志分流到 `arena_malloc_small`(:867)或 `large_malloc`。
- **小分配**:`bin_choose`(按 tsd binshard 选 bin shard)→锁 bin→`bin_malloc_no_fresh_slab`;slab 耗尽则 `arena_slab_alloc`(:706,pa_alloc 拿 slab 页并初始化 bitmap/nfree/binshard)再试。
- **批量填充**:`arena_ptr_array_fill_small`(:736-865)优先 slabcur→nonfull→新 slab,锁只在循环外取放,支持 flat combining(多线程合并拿锁)。
- **释放**:`arena_dalloc_small`(:956)→`arena_dalloc_bin`→`bin_dalloc_locked_step`。
- **初始化**:`arena_new`(:1468)从 base 分配、`pa_shard_init`、逐 shard `bin_init`;`arena_boot`(:1686)预计算 `arena_bin_offsets` 与除法 `div_info`(把"region 偏移/reg_size"变成乘法)。

#### bin / bin_info —— slab 内部分配

`bin_t`(bin.h:23-50)= 锁 + stats + `slabcur`(当前活跃 slab)+ `slabs_nonfull`(配对堆,按地址序)+ `slabs_full`(链表)。`bin_info_t` 由 sc 派生:reg_size、slab_size、nregs、n_shards、bitmap_info。

- 单取:`bin_slab_reg_alloc`(bin.c:72)用 `bitmap_sfu` 找空闲 region;
- **批量取(本 fork 关键修改)**:`bin_slab_reg_alloc_batch`(bin.c:89-134)不再逐 region `bitmap_sfu`(树状位图需根到叶 walk + fullness 传播),而是用 `bitmap_ffu_group` 定位首个非满底层组,组内连续 `cfs_lu` 取满,最后 `bitmap_group_set` 一次写回+传播(见 §5.1);
- slab 生命周期:满/空迁移 `bin_refill_slabcur`、`bin_lower_slab`(:211)、`bin_dalloc_locked_handle_newly_empty`(:246)→`arena_slab_dalloc`。

#### tcache / cache_bin —— 线程本地缓存

`cache_bin_t`(cache_bin.h:88-135):**向下生长的栈**,`stack_head` 全指针 + 低 16 位复用三个标志(low_water/full/empty,单栈 < 2^16 字节的巧妙前提)。

- 容量:`tcache_ncached_max_compute`(tcache.c:717)= 小类 `nregs<<TCACHE_LG_NSLOTS_MUL` 限幅,大类固定 `TCACHE_NSLOTS_LARGE`;`tcache_max` 默认 32KB(可配)。
- 取:`cache_bin_alloc_easy/alloc`;放:`cache_bin_dalloc_easy`(及 UAF stash 变体)。
- 填充:`tcache_alloc_small_hard`(tcache.c:421)按自适应 nfill → `arena_ptr_array_fill_small` → `cache_bin_finish_fill`;刷出:`tcache_bin_flush_bottom`(:456)按 arena/binshard 分批。
- **GC**:`tcache_gc_small`(:226)按 low_water 重算保留目标,优先刷"远端 slab 的指针"(把可合并性差的先扔掉)。

**解耦设计**:tcache 不直接碰 arena 内部——填充/刷出由 arena 的 `arena_ptr_array_fill_small/flush` 执行,arena 只通过 `cache_bin_array_descriptor`(cache_bin.h:143)读统计。**双向解耦**。

#### large —— 大对象

`large_malloc → large_palloc`(large.c:17-58):`sz_sa2u` 算对齐尺寸→`arena_choose_maybe_huge`→`arena_extent_alloc_large`(arena.c:363,含 sz_large_pad、cache-oblivious 随机化、memset)→`pa_alloc`。非 auto arena 的大对象挂入 `arena->large` 列表跟踪,`large_dalloc` 从列表移除→`pa_dalloc`。`large_ralloc`(large.c:189)先试 `pa_expand/pa_shrink` 原地扩展,失败才新分配+拷贝。

#### extent —— 页面供给的编排者(核心层侧)

`extent.c` 同时服务于核心层与物理层,是二者之间的粘合:

- 获取:优先 `extent_recycle`(extent.c:633,从 dirty/muzzy/retained 的 eset 中 extract+split);失败 `extent_alloc_retained`(:877)/`extent_grow_retained`(:732,指数增长虚拟内存);最后 `extent_alloc_wrapper`(:1143)→`ehooks_alloc`(mmap 抽象)+ `extent_register`(:348)写入 emap。
- 释放:`extent_record`(:1032)在 ecache 锁下 `extent_try_coalesce` 合并(大 extent 有 max_coalesce 上限限制),再入 eset LRU。
- split/merge 经 `emap_prepare/commit` 两步走,保证并发安全。

### 3.4 页面供给层(物理层)

#### PAI 抽象与 pa

**PAI(Page Allocator Interface)** 是本层最重要的设计:一个"概念接口",两个同构实现——`pac_*()`(经典分配器)与 `hpa_*()`(大页分配器),签名一致(`edata_t*` 进出 + `deferred_work_generated` 布尔标志),支持 alloc/dalloc/expand/shrink/do_deferred_work。

- `pa_central_t`(pa.h:23)是**进程级单例**(共享 HPA 的全局 eden);
- `pa_shard_t`(:66)是 per-arena 句柄 = central + nactive + use_hpa 状态 + hpa + edata_cache + pac + base/emap/ind;
- `pa_alloc`(pa.c:124)按 `use_hpa` 分派到 `hpa_alloc` 或 `pac_alloc`,成功后 `pa_nactive_add`、`emap_remap`、注册 slab 内部页;
- `pa_dalloc`(:219)按 edata 上的 `EXTENT_PAI` 位(edata.h:58)分派——**分配器归属编码在 edata 里,释放时无需上下文**;
- expand/shrink 仅走 pac(HPA 为 stub)。

#### pac —— 经典页面分配器

`pac_t`(pac.h:101-153)= **SEC + ecache_dirty(延迟合并)/muzzy/retained/pinned + exp_grow + san_bump + decay_dirty/decay_muzzy + grow_mtx + extent_sn_next**。

- `pac_alloc_real`(pac.c:155)的缓存优先级链:**SEC → pinned → dirty → muzzy → grow(保留地址空间)→ 直 mmap**;
- `pac_dalloc`(:417):guarded 大 extent 不缓存直接 unguard;否则进 SEC,溢出 flush 到 ecache;
- decay:`pac_decay_to_limit`(:563)先 stash 再 purge(dirty→muzzy 用 purge_lazy,→retained 用 purge_forced/unmap,支持 process_madvise 批量),deferred work 完成后唤醒后台线程(`pac_maybe_wake_bg`,:908)。

#### ecache / eset —— 分级 extent 缓存

- `ecache_t` = mtx + eset + guarded_eset + state + ind + delay_coalesce(ecache.h:10-23);逻辑实现在 extent.c(`extent_recycle/record`)。guarded_eset 单独存放 guard extent,驱逐优先级更低。
- `eset_t`(eset.c):`(SC_NPSIZES+1)` 个 bin,每 bin 一个 `edata_heap`(**配对堆**,按 sn/addr 的 cmp_summary 排序)+ fb 位图标记非空桶 + LRU inactive 链。`eset_first_fit`(:291)带 `lg_max_fit` 最大拟合比(防碎片)与 prefer_small;pinned extent 不进 LRU(不被 decay)。

#### sec —— Small Extent Cache

SEC 是 **per-PAI 的小 extent 缓存**(sec.h:70-88):`nshards × npsizes` 个 `sec_bin_t`(mtx + freelist + bytes_cur + 统计)。

- `sec_shard_pick` 用 **Lemire 免除法随机数**选 shard(sec.c:72-97);多 shard 先 trylock 再阻塞兜底(:163-199)——把锁竞争降到最低;
- 超 `max_bytes` 时 flush 至 3/4 水位(:244-270);`sec_fill` 成批填充到 max_bytes/4(sec.h:107-114,min/max nallocs 2/8)。
- fork 曾为其加过 USDT probes,`fde6a21a` 已移除(高频路径减噪,见 §5.2)。

#### hpa 家族 —— Huge Page Allocator

HPA 面向大页友好的工作负载:

- **hpa_central**(进程级):一次性 mmap 全局 eden(128×HUGEPAGE),每次按一个 HUGEPAGE 切给 shard;THP=always 时 start_as_huge。
- **hpdata**:每个 hugepage 一份元数据——`active_pages/touched_pages` 两个 fb 位图 + longest_free_range + age;`hpdata_find_alloc_offsets` 做 first-fit 切块(:174-260);purge 时用 `dirty = ~active & touched` 并扩展 gap 减少 TLB shootdown(:340-489)。
- **psset**:按 longest_free_range 的 pszind 分桶的 age 配对堆 + empty LIFO 链 + `to_purge[N]`(按脏度桶 LRU + 位图)+ to_hugify FIFO;`update_begin/end` 协议维护不变量(:287-336)。
- **hpa_shard** = grow_mtx/mtx + central + psset + sec + edata_cache_fast + age_counter。`hpa_alloc`(hpa.c:903):SEC→psset 批量分配→不足时持 grow_mtx 向 central 要新 pageslab;deferred work = purge(按 dirty_mult 与 purge_threshold 选候选)+ hugify(`hugify_delay_ms` 延迟)。
- **hpa_hooks**:把 pages_* 映射成 HPA 可替换的 hooks,含 hpa 专属 USDT 探针(hpa_hooks.c:21-101);Windows 上 `hpa_supported()=false`。

#### base / edata_cache —— 元数据的内存供给

- **base**:内部元数据分配器——blocks 链表 + SC_NSIZES 个 avail 配对堆;沿 psz 序列**指数增长块大小**(上限 128MiB);`metadata_thp=auto` 模式第 2 块起用大页;`base_alloc_impl`(:578)先查 avail 再 bump 切分;`base_new` 用栈上 fake_ehooks 自举(:478-546)。
- **edata_cache**:mtx + avail 链 + count + base,弹空则 `base_alloc_edata`;`edata_cache_fast` 是 HPA shard 的无锁快捷层(本地 LIFO,空了从 fallback 一次取一批)。

#### pages / ehooks —— 与 OS 的最后边界

- **pages**:统一 OS 封装。`pages_map` 乐观精确 mmap、失败退超额映射+trim;purge 分 lazy(MADF_FREE,启动时探测)与 forced(MADV_DONTNEED,含"dontneed 后是否真的清零"探测);支持 `process_madvise` 批量 purge;读取 THP 状态决定 HPA hugify 策略。
- **ehooks**:把 `extent_hooks_t`(用户自定义钩子)与默认实现桥接;`extent_alloc_core`(ehooks.c:27)按 dss_prec 依次尝试 dss→mmap→secondary dss;默认表(:266-279)把 commit/decommit/purge/split/merge 映射到 pages_*。**它是核心层触达 OS/自定义 hooks 的唯一通道**。
- **extent_dss**(sbrk,自旋 CAS 串行化扩展)与 **extent_mmap**(即 pages_map)。

### 3.5 并发基础设施层

#### mutex / witness —— 锁与死锁防护

- `malloc_mutex_t` = OS 锁 + `atomic_b_t locked` 自旋探针(置于 prof_data 后减缓存行颠簸,mutex.h:28-44)+ witness(非 debug 构建零占用)。slow path 先自旋(`opt_mutex_max_spin=600`)再阻塞并记等待统计(mutex.c:51-107)。
- **witness rank 是设计文档级的工程决策**:`witness.h:12-81` 的枚举即锁获取的**全序**——低 rank 先获取、只能递增(同 rank 需地址有序或互斥)。核心链条:全局级(CTL/TCACHES/ARENAS)→ per-arena(ARENAS)→ extents(EXTENTS/HPA_SHARD/DECAY)→ EDATA_CACHE → RTREE → BASE → ARENA_LARGE → 叶锁 BIN/ARENA_STATS。detect 到逆序即打印并 abort。**这把"死锁不可测"变成"违反全序即编译/运行期可查"**。

#### atomic —— 多后端原子抽象

编译期选后端:GCC `__atomic` → gcc_sync(用 fence 实现 acquire/release)→ MSVC → C11;`JEMALLOC_GENERATE_ATOMICS` 宏批量生成各类型 load/store/exchange/CAS/fetch_add。

#### tsd —— 线程局部数据统一管理

- `TSD_DATA_SLOW/FAST/SLOWER` 三组宏静态声明全部槽位(事件计数、arena、ticker_geom、rtree_ctx、tcache、thread_allocated 等),`O(n,t,nt)` 宏批量生成访问器,编译期展开成单一 `tsd_t`——**"动态槽位"由宏驱动,零运行时查找**。
- 状态机(uninitialized→nominal/minimal_initialized→reincarnated→purgatory)惰性初始化(tsd.c:173-233),线程退出按状态分派清理;存储后端 TLS/win32/pthread_key 宏切换。

#### rtree —— 地址→元数据的反向映射

- 按虚拟地址位数算 `RTREE_NHIB/NLIB/NSB`,1-3 层 radix tree;叶元素 compact 编码(单指针字打包 edata/szind/state/is_head/slab,rtree.h:75-99)。
- **两级 TSD 缓存**:L1 直接映射(16 项)+ L2 LRU(8 项)(rtree.h:431-509,rtree_tsd.h:23-64);miss 走 `rtree_leaf_elm_lookup_hard`(rtree.c:219)用 init_lock 惰性建节点;dependent 读 relaxed、否则 acquire。
- fork 的 `emap_alloc_ctx_lookup` 把 L1 命中路径内联、慢路径外置 `rtree_read_slow`(rtree.c:319)。

#### 容器与数学原语

| 模块 | 设计 |
|---|---|
| `ph`(配对堆) | twopass aux-list 优化:按 `ffs(insert_count)` 批量 pop-and-link,插入摊还 O(1);用于 edata_heap、hpdata age 堆 |
| `rb`(红黑树) | left-leaning 2-3 树,`RB_COMPACT` 把颜色压入指针低位省父指针;用于 prof 的 gctx/tdata/tctx 树 |
| `ql/qr` | 环形/带头链表,是 witness 持有表、cache_bin、日志等的基础 |
| `bitmap` | 以 `BITMAP_GROUP_NBITS`(字宽+3)分组,超阈值自动启用多层树(≤5 层),inverted 表示(1=空闲);fork 新增 `bitmap_ffu_group/bitmap_group_set` |
| `bit_util` | ffs/fls/popcount 多平台实现、lg_floor/ceil、**fork 的 `cfs_lu`(x & (x−1) 清最低位)** |
| `ckh` | 双哈希 cuckoo hashing(桶=1 缓存行),prof 上下文表用 |
| `hash/prng/fxp/div` | MurmurHash3、LCG(取高位)、16.16 定点数(核心代码零浮点)、magic-number 快速除法 |
| `ticker/spin/counter/mpsc_queue` | 倒计时节流(含几何 ticker)、指数退避自旋、lockedint 字节累加计数、Treiber 两栈式 MPSC 无锁队列(预留) |

#### 输出基础设施

`emitter`(纯头文件,44 个 static inline)统一 JSON/JSON-compact/表格三格式,供 stats/ctl;`buf_writer` 缓冲写;`malloc_io` 自研**无浮点 snprintf + syscall 直写 fd**——避免依赖 libc 的 malloc 重入;`log` 层次化运行时日志(`malloc_conf log:` 配前缀开关)。

### 3.6 高级功能层

#### prof 家族 —— 采样剖析

- **prof.c**(API/选项层):`opt_prof`、`lg_prof_sample` 默认 19、fork 新增 `opt_prof_bt_max`(默认 128/上限 256);`prof_sample_new_event_wait`(:298)按几何分布生成采样间隔;`prof_boot0/1/2`(:687/696/722)建 unbias 表与 gctx/tdata 锁池。
- **prof_data.c**:全局 bt2gctx ckh(:61)+ tdatas 树(:67);`prof_lookup`(:334)先查线程 bt2tctx 再全局并建 `prof_tctx_t`。
- **prof_sys.c**:`prof_backtrace`(:426)走**可替换 hook**;`prof_bt_max==0` 时装 noop 跳过 in-process backtrace(供外部 eBPF 工具采栈);BT_FRAME 展开到 256 帧。
- 附:prof_log(三链表日志)、prof_recent(最近分配)、prof_stats(按 szind live/accum)。
- **采样插入点**:`imalloc_body`(jemalloc.c:618)用 `prof_sample_lookahead` 预判(不推进计数)→ `prof_alloc_prep`(prof_inlines.h:162)命中才取 tdata 建 tctx → 命中走 `imalloc_sample`(:476,页对齐+禁 slab+arena_prof_promote);分配完成后 `thread_alloc_event` 推进计数,越过阈值触发 prof_sample 事件 handler。

#### decay —— 页面衰减算法

`backlog[64]` 记录各 epoch 新增脏/模糊页(:117-150),**fixed-point smoothstep 系数**加权得 `npages_limit`(:95-109)——把"多久没被用"转成"最多留多少脏页"。epoch 推进带 jitter 防同步(:176);`decay_ns_until_purge`(:241)二分搜索预估剩余可 purge 页,供后台线程休眠决策。

#### background_thread —— 后台回收

默认关闭(可由 `background_thread:true` 开启)。info 数组按 arena 分段;`background_work_sleep_once`(:303)遍历所辖 arena 调 `pa_shard_do_deferred_work` 并取 `pa_shard_time_until_deferred_work` 最小值决定休眠(下限 100ms,:206);thread0 兼管其他线程创建/回收。**decay 双触发**:后台线程周期执行 + 应用线程分配/释放路径也可触发(pac.c:852-883)。

#### thread_event / peak_event —— 线程事件机制

`thread_event_registry.h:10-39` 定义 te_alloc 事件(prof_sample/stats_interval/tcache_gc/peak + 4 用户)与 te_dalloc 事件。`te_event_advance`(thread_event.h:200)累加 thread_allocated 字节,越过 next_event 调 `te_event_trigger`(thread_event.c:300):各 handler 扣减自身 wait、到期入 to_trigger 数组、取最小 wait 重设阈值——**一个共享计数器驱动多种周期性行为**。

#### percpu_arena —— per-CPU arena

`percpu_arena_map[PERCPU_ARENA_MAX_CPUS]`(:19)是 CPU→arena 映射;模式 percpu(每 CPU 一组)/phycpu(超线程共享,组数 ncpus/2)。`percpu_arena_boot`(:104)用 `os_cpu_affinity_cpus` 读亲和掩码建映射,未知 CPU 回退 `c%n`。`arena_choose_hard`(arenas_management.c:226)percpu 命中时直接 bind;`arena_migrate`(:191)换绑后对空旧 arena flush。fork 移除 last-thread 捷径并新增映射表修复亲和性越界(见 §5.4)。

#### stats / san / zone / hooks

- **stats**:`stats_print` 先建 ctl 快照,emitter 按 opts 输出 JSON/表;分 merged/destroyed/unmerged。
- **san**:guard page(`ehooks_guard` 加左右 SAN_PAGE_GUARD 并改 edata 标记,`san_guard_pages` :62)、UAF 检测(释放时 junk 填充+stash,`san_check_stashed_ptrs` :174 校验写后释放)、`lg_san_uaf_align` 控 cache_bin 非快掩码;`san_bump` 从 retained 大块尾部 bump 分割+右侧 guard。
- **zone**(macOS 全套 malloc_zone_t 回调)、**test_hooks**(4 个测试钩子)、**safety_check**(redzone 32B/0xBC,大小不匹配检测)、**inspect**(实验性堆内省 API)。

---

## 4. 模块交互与耦合分析

### 4.1 关键调用链

#### malloc 快路径

```
je_malloc (jemalloc.c:771)
 → imalloc_fastpath (inlines_c.h:286)   [size≤4K]
    → sz_size2index_usize_fastpath      [sz]
    → cache_bin_alloc_easy/alloc        [cache_bin,无锁]
 → 未命中:malloc_default → imalloc_body (jemalloc.c:550)
    → imalloc_no_sample → iallocztm_explicit_slab
    → malloc_dispatch_malloc
    → tcache_alloc_small (tcache_inlines.h:102)
    → tcache_alloc_small_hard (tcache.c:421)   [自适应 nfill]
    → arena_ptr_array_fill_small (arena.c:736) [取 bin 锁,flat combining]
    → bin_slab_reg_alloc_batch (bin.c:89)      [bitmap group 批量取 region]
        → 或 arena_slab_alloc (arena.c:706)    [slab 耗尽 → 物理层]
            → pa_alloc (pa.c:125) → pac_alloc/hpa_alloc
            → extent_recycle / extent_alloc_wrapper → ehooks_alloc → pages_map → mmap
大对象:arena_malloc_hard → large_malloc → pa_alloc
```

#### free 快路径

```
je_free (jemalloc.c:1099)
 → free_fastpath (inlines_c.h:467)
    → emap_alloc_ctx_try_lookup_fast      [emap/rtree L1 命中,拿 szind+slab]
    → cache_bin_dalloc_easy               [入 tcache 栈,无锁]
 → 未命中:idalloctm → malloc_dispatch_dalloc
    → tcache_dalloc_small → tcache_bin_flush_small (满)
    → arena_ptr_array_flush (arena.c:997) [按 arena/binshard 分批]
    → bin_dalloc_locked_step (bin_inlines.h:76)
       → bin_slab_regind → bitmap_unset + nfree++
       → newly_empty → arena_slab_dalloc (arena.c:439)
          → pa_dalloc (pa.c:220) [按 EXTENT_PAI 位分派]
          → extent_record (extent.c:1032) [合并] → ecache/eset
          → decay 到期 → purge → pages_purge → madvise/munmap
```

#### mallctl

```
je_mallctl → malloc_init → ctl_byname (ctl.c:1676) → ctl_lookup (1585)
  [super_root_node 静态树 + 节点回调]
MIB 路径:mallctlnametomib → ctl_nametomib → mallctlbymib → ctl_bymib
  → ctl_lookupbymib [免字符串比较]
```

### 4.2 数据结构共享:edata 的多重挂载

**`edata_t` 是全系统的"通用令牌"**,同一块内存同时/不同时挂在多个容器:

```
                    ┌──────────────┐
   活跃(小对象)      │  arena->bin  │ slabcur / slabs_nonfull / slabs_full
   活跃(大对象)      │  arena->large│ 链表
   不活跃(待复用)    │  ecache/eset │ dirty/muzzy/retained 配对堆 + LRU
   缓存层            │  sec / hpa   │  freelist / psset
   元数据池          │  edata_cache │  avail 链
   反向索引          │  emap(rtree) │ 地址 → edata + szind/slab/state
```

这套设计的工程价值:**释放路径零上下文**——只要拿到指针,emap 反查 edata,edata 自描述(pai/szind/arena_ind/nfree),即可走完整条回收链。代价是 edata 必须 128 字节对齐、位域紧凑,并严格遵循 witness 锁序。

### 4.3 并发模型与耦合边界

| 边界 | 机制 | 效果 |
|---|---|---|
| 热路径无锁 | tsd + rtree L1/L2 缓存 + cache_bin | malloc/free 常见情形零锁、零系统调用 |
| 锁的全序 | witness rank 枚举 | 死锁从"运行时事故"变为"违反全序即报错" |
| arena↔物理层 | PAI 接口 + edata 令牌 | 物理层实现(pac/hpa)可替换、可并行存在 |
| 核心↔OS | ehooks 唯一通道 | 用户可注入自定义 extent hooks;san 也借此挂 guard |
| 异步工作 | `deferred_work_generated` 布尔 + 事件计数 | 分配路径只"上报"工作量,后台线程执行,不做重活 |
| 分派 | edata 的 EXTENT_PAI 位 | 释放时无需记住从哪个分配器来的 |

**核心层的自洽性**:arena/bin/tcache 只认识 `edata_t` 与 `pa_alloc/pa_dalloc/pa_expand/pa_shrink`,不认识 mmap/madvise/hpdata——物理层的任何重构(如 HPA 开关、SEC 调整)对核心层透明。

### 4.4 值得注意的耦合点

1. **sc 的全局性**:所有表(sc/sz/bin_info/arena_boot)依赖同一份 sc_data,初始化顺序(§3.1)必须保证 sc_boot 最先;
2. **emap 的单例**:`arena_emap_global` 被 arena/extent/large/emap 同时读写,rtree 的惰性建节点需 init_lock;
3. **decay 的三方协作**:ecache(状态)→ decay(算法)→ background_thread/分配路径(触发),三者通过 pac 的 deferred work 收紧;
4. **prof 的快路径渗透**:采样逻辑内联在 imalloc_body/free 中,靠 thread_event 的共享计数器降频——这是"功能横切但不破坏性能"的范例;
5. **base 的自举**:物理层所有模块的元数据都来自 base,而 base 自身用 fake_ehooks 从 OS 直接拿,形成启动闭环。

---

## 5. fork 定制设计

(相对上游 5.4.0 的 46 个提交;commit 为作者自述主题,代码现状以 HEAD 为准)

### 5.1 性能快速路径

**bitmap group 批量填充**(`29a4539e` 因子化 + `8487209a` 应用 + `e5b2ff07` cfs_lu)——本 fork 最核心的优化:

- 问题:原 `bin_slab_reg_alloc_batch` 每取一个 region 做一次 `bitmap_sfu`;树状位图下每次都要根到叶 walk + 全量传播,是 tcache 批量填充的热点。
- 方案:新增 `bitmap_ffu_group()`(bitmap.h:372,定位首个非满底层组)+ `bitmap_group_set()`(bitmap.h:246,整组写回+向父层传播 full 状态);`bin_slab_reg_alloc_batch`(bin.c:89)每轮只定位一次组,组内用 `cfs_lu` 连续取 region,整组一次写回。**"每 region 一次 bitmap 写+树传播"降为"每 group 一次"**,减少 cache line 竞争与树遍历。
- `cfs_lu`(bit_util.h:289)用 `x & (x−1)` 清最低置位,解掉"先 ffs 再清除"的循环携带链(x86 由 tzcnt+btc 降为单条 blsr,arm64 为 sub+and),且 index 与更新值可并行计算。
- 验证:`test_bin_slab_reg_alloc_batch_fragmented`(test/unit/bin.c)对照单次分配,校验碎片 slab 下选取 region 与最终位图完全一致。

**emap / dallocx 快速路径**(`2fee33d2`, `6361a50b`, `3d07f2b5`):

- `emap_alloc_ctx_lookup`(emap.h:259)内联 L1 命中路径,慢路径外置 `rtree_read_slow`;slab 或大 size class 启用时 usize 直接由 szind 算出,免 deref edata;
- `dallocx` 的 flags!=0 分支移入 noinline `dallocx_default()`,使 `dallocx(ptr,0)` 与 `free()` 共享同一无栈帧快速路径(aarch64 gcc -O3 下每对 mallocx+dallocx 103.6→98.6 指令,clang ThinLTO 106.5→82.5);
- `39839998` 修正 large 统计的 allocated 用 `active_bytes`,避免大 size class 禁用时按上界高估。

**TCACHE_NBINS_MAX 收紧**(`8a2aa363`):原上限混用 _MAX 与下界,过估;补齐 `SC_LG_LARGE_MINCLASS_MAX` 后按 `LG_PAGE_OR_MAX` 取紧界,缩小 cache_bin 静态数组(与动态 page size 配套)。

### 5.2 可观测性:USDT / OpenTelemetry

- **probe 基础设施**(`jemalloc_probe.h`):按宏选后端——`JEMALLOC_EXPERIMENTAL_USDT_STAP`(sys/sdt.h)→ `jemalloc_probe_stap.h`;`..._CUSTOM`(自实现 asm nop)→ `jemalloc_probe_custom.h`;MSVC→空;否则仅参数类型检查。`3b5b9669` 给 `JE_USDT` 加 provider 参数,支持自定义 provider。
- **otel_memory 采样探针**(`e0131540`):分配完成发 `otel_memory:alloc(ptr, size, weighted_size)`(prof.c:207)、释放发 `otel_memory:free`;`prof_sample_weighted_size()` 按 Horvitz-Thompson 估计量(基于 Poisson 字节采样概率)对申请 size 加权;realloc 路径在旧地址复用前先发 free。配套 `tools/prof_usdt/profile_example.bt` bpftrace 脚本(`55469566`),Linux CI 实跑(`beb06cee`)。
- **prof_bt_max:0**(`adfe588f`):允许 0 跳过 in-process backtrace(`prof_backtrace_noop`),采样/统计/堆 dump 照常(归入单一空 backtrace),供外部 eBPF 工具采栈。
- **SEC probes 移除**(`fde6a21a`):SEC 是每-CPU shard 高频路径,非采样 probe 的噪声与开销大于价值,且与 otel_memory 目标无关——**可观测性与性能的取舍被显式写进提交**。hpa_hooks 保留 hpa_* probes(低频路径)。

### 5.3 动态 page size(lg_page)

把编译期 `LG_PAGE/PAGE` 在 `DYNAMIC_PAGE_SIZE` 下重定义为运行时 `lg_page_size/page_size`(pages.h:12-18),`LG_PAGE_OR_MIN/MAX` 提供静态数组的上下界,`HUGEPAGE_PAGES` 亦变 `hugepage_pages`。传播链(按提交顺序):

```
aabec954 hpdata/HUGEPAGE_PAGES
 → 1b51cc17 sc/SC_NPSIZES
 → 64f1acdf SC_NBINS_FOR(_lg_page)/SC_SMALL_MAXCLASS_FOR/... + tcache/TCACHE_NBINS
 → dce0b0ec rtree(rtree_levels[] 变运行时数组,RTREE_NSB_MAX 定高度与 root 大小)
 → c493e5f7 slab/edata bitmap
 → d99e9356 收尾:临时迁移宏统一回 LG_PAGE/PAGE
```

配套:`lg_page` conf 选项(0=从 os_page 推导)在初始 pass 解析(conf.c:555-566),`pages_pre_boot` 校验并写全局(pages.c:509-522);`421b3517` 按实际页大小运行时算 slab bitmap 大小,`edata_boot` 动态收缩 edata,避免按 MAX_LG_PAGE 预留导致 edata 128→256 字节膨胀;sec_opts/hpa_opts 默认值由常量换 `DYNAMIC_PAGE` 并运行期化(`00693caa`/`be7b922b`)。

### 5.4 per-CPU arena 与平台适配

- **移除 last-thread 捷径**(`c42af462`):该捷径在两个生产负载中导致 10%/37% 的选择用了过期 arena,而 `sched_getcpu` 仅 ≤7 cycles——直接读更准。
- **percpu_arena_map**(`62b1a279`):boot 期按亲和掩码构建 CPU→arena 映射表(percpu_arena.h:101-119),修复进程受亲和性限制时索引越界(撞上 huge_arena_ind 或不可见)的问题;`test/unit/percpu_arena_map.c` 用独立实现对照校验。
- **杂项**:无 RTLD_NEXT 时 dlsym 退 RTLD_DEFAULT(`d4c9f03e`);Illumos 支持(`84286c27`);SH-3 `LG_QUANTUM=4`;`__has_include` 守卫;Makefile 让 in-tree 头优先于已安装头(`344b5eb3`);`MALLOC_CONF_TEST` 注入测试配置(`cf35c74d`);aligned new 失败后保对齐重试(`bf553efa`)。

### 5.5 主线间的依赖关系

- **动态 page size 是最大主线**:派生 `*_FOR(_lg_page)` 宏族与运行时数组,反向迫使静态初始化器运行期化(be7b922b/00693caa)与 edata 动态尺寸(421b3517);TCACHE_NBINS_MAX 收紧是它的后续修正;
- **bitmap group 与 cfs_lu 是同一优化的两面**(bin.c 批量填充);
- **otel_memory USDT + prof_bt_max:0 + bpftrace 脚本构成完整外部采样工作流**;SEC probes 移除与 percpu 简化同属"高频路径减噪";
- 整体呈现为 **Meta 面向生产(低延迟 + OpenTelemetry/eBPF 观测)的定制维护分支**。

---

## 6. 工程设计与实现评价

### 设计亮点

1. **PAI 抽象层**:用同构函数集 + edata 上的 PAI 位实现"分配器可插拔",是分层解耦的范本;pac/hpa 可以独立演进、独立开关。
2. **快路径工程学**:热代码全部 `static inline` 进头文件、慢路径显式出线(noinline)、分支按"大概率先命中"排列;`malloc_slow_flags` 一次性汇总"哪些特性需要慢路径"。
3. **编译期生成替代运行时多态**:sc 表、tsd 槽位、容器(ph/rb/ql/qr/bitmap)全部宏生成——零虚表、零间接调用,同时保持可读的"声明式"描述。
4. **witness rank 即文档**:锁层次被写成可执行的枚举,新模块加锁必须声明 rank,工程约束内化到编译器。
5. **edata 的位域自描述**:128 字节内打包全部元数据 + 三重 union 复用,是"数据结构即协议"的极致。
6. **数学原语零浮点**:decay 用定点 smoothstep、fxp 处理比例、div 用 magic number——保证分配器无浮点路径(可移植性 + 确定性)。

### 权衡与代价

- **宏与内联的复杂度**:代码生成宏(tsd 的 O()、atomic 的 GENERATE、容器 ph_gen)调试体验差,报错信息难读;这是 jemalloc 一贯的"可维护性换性能"。
- **edata 的耦合**:所有模块都依赖 edata 的位域布局,新增元数据字段需要全局重排(本 fork 的动态 edata 收缩即为一例)。
- **rtree 的内存/路径开销**:全局反向索引是 free 快路径的依赖,两级 TSD 缓存缓解但仍占用 TSD 空间。
- **初始化顺序敏感**:物理层/核心层数百个全局的 boot 顺序(§3.1 四阶段)是隐性契约,新模块必须严格按位插入。

### 质量保障

- 170 个 unit 测试 + integration/stress/analyze 分层(见附录),且 fork 对每次优化都补了针对性测试(碎片 slab 批量填充、percpu arena map/resume、tcache_max_disabled);
- `witness` 在 debug 构建全开;`safety_check`、`san` 提供生产可用的双重检测;
- 代码生成脚本(smoothstep.sh 生成 decay 系数表)由 Makefile 驱动,避免手算误差。

### 潜在风险点

- 动态 page size 把大量"编译期常量"变成"全局变量",任何遗漏的 `_FOR()` 化都会造成数组越界——依赖 conf 依赖检查和测试覆盖;
- USDT CUSTOM 后端的 asm nop 在部分平台/优化级别下可能有边界效应;
- 生产分支与上游 5.4 的持续 rebase 成本会随上游演进累积。

---

## 7. 附录:测试体系与构建

### 测试分层(`test/`)

| 层级 | 内容 | 特点 |
|---|---|---|
| `unit/` | 170 个单测,几乎每模块一个 | 直接调用内部 API,不经过公开 ABI;覆盖 fork 新逻辑 |
| `integration/` | 跨模块集成 | 走公开 API 验证组合行为 |
| `stress/` | 压力测试 | 并发/长时间稳定性 |
| `analyze/` | 静态分析入口 | |
| `src/` + `include/` | 测试用公共头 | |

`test.sh.in` 统一驱动;`.sh` 后缀的用例带 shell 脚本(`*.sh`),支持 `MALLOC_CONF_TEST` 注入(`cf35c74d`)。

### 构建与代码生成

- autotools(`configure.ac`/`Makefile.in`/`autogen.sh`)+ MSVC(`msvc/`);`bin/` 提供 `jemalloc-config`、`jemalloc.sh` 等工具;
- **生成物**:`jemalloc.h`(由 protos/macros/typedefs/mangle/rename 五个 `.in` 拼接,public_namespace.sh 生成符号前缀宏)、`jemalloc_internal_defs.h`、`smoothstep.h`(smoothstep.sh 用 dc 算定点表)、`ticker`(ticker.py);
- fork 新增 `scripts/test_build_include_order.sh`(验证 in-tree 头优先)与 `tools/prof_usdt/`(bpftrace 脚本)。

---

*本文档基于代码现状(HEAD `fde6a21a`)与上游 5.4.0 的差异分析整理;所有 `file:line` 引用指向当前工作树。*
