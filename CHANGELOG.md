# Changelog

格式参照 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/)。

## [Unreleased]

### Added

- 每线程大对象 extent 缓存:jemalloc 式按尺寸类组织,类 = 精确页数,
  每类独立 LIFO、默认 20 槽;单块上限 `tcache_max`(默认 32 KiB,与
  jemalloc 一致,可调至 8 MiB)与
  每线程页预算(默认 8 MiB)双阀,`0` 分别表示关闭与不限。稳态大对象
  路径不再触碰全局 `g_ext_lock`。缓存块保持 INUSE 并计入 live,
  flush/线程退出/fork 子进程清理时排空,`cached_total` 与字节对账
  按类链同步扩展。
- 大缓存按取用量 GC:上次 GC 以来未取用的页数类整类归还,用过的类
  保留取用量加 25% 的最新块;`gc` 命令、跨线程 `gc_req` 与每 256 次
  put 的滚动收缩三条路径生效。
- 新增 `wmm_ctl` 命令 `config.tcache_max` 与 `config.tcache_budget`:
  按字节设置并向下取整到页,带硬上限与非法值校验。
- 项目初始化:目录结构、BSD-2-Clause 许可证、规格文档入库(`doc/`)。
- 按 `doc/tasks.md` 建立模块骨架(手写起点,M0 起)。
- 基准程序独立目录 `bench/`(`bench/bench.c`)。
- 生产验证基座:`validate/soak.c`(分阶段浸泡、跨线程释放、线程
  churn、RSS 与快照不变式)、`validate/forkstress.c`(活跃负载下
  反复 fork,含 guard 模式)、`validate/oomcheck.c`(填满真实窗口,
  校验 NULL/ENOMEM 契约与 abort_oom 的 SIGABRT)、
  `validate/cgroup_oom.sh`(cgroup v2 限额与 peak/oom_kill 检查)、
  `validate/gate.py`(独立进程编排、硬门禁、JSON/TXT/CSV 报告)。
- `make validate` / `make validate-full` / `make soak` / `make
  forkstress` / `make oomcheck` / `make cgroup-oom`。
- bench 增加 `server` 请求模型与 p999/p9999/max、请求吞吐、分配失败
  列;前 8 列保持兼容。
- bench server 尾延迟归因:`--server-big/realloc/session/touch 0|1`
  逐项关闭负载成分(随机流不变),stderr 打印最慢请求的对象数、
  最大尺寸、调用数与标志位。
- bench 增加 `--tcache-max` / `--tcache-budget`:启动时按字节设置
  wmm 的大缓存单块上限与每线程预算,用于同负载对照不同调优值。
- soak CSV 与 `SOAK_RESULT` 增加 `window_kb`、`base_kb` 高水位:分别
  观测窗口 bump 决定的页表常驻页与 base 元数据开销;多余列不影响
  `gate.py` 解析。

### Changed

- 大缓存滚动 GC 改为按页限额并轮转页数类:每 256 次大对象 put 的
  自动收缩每次最多归还 `WMM_TCACHE_LARGE_GC_PAGES`(默认 128)页,
  游标从上次停下的页数类继续;被预算截断的类保留取用量,下一圈继续
  收缩。显式 `gc`、跨线程 `gc_req`、flush、线程退出与 fork 子进程
  清理仍是完整收缩。超过当前上限的类不可命中,滚动路径遇到时整类
  归还且不占页预算,保持降低 `tcache_max` 立即生效。
- 物理层空闲 extent 改为尺寸分桶:1..256 页精确成桶,更大按
  `floor(log2(npages-1))` 分桶,DIRTY 与 RETAINED 分开;分配从请求
  桶向大扫,不再全地址链扫描,全局 `g_ext_lock` 临界区随之缩短。
  地址序链保留,只负责相邻合并与 purge 不变式。大对象页表登记移到
  锁外,块在锁内已标 INUSE。新增 `wmm_phys_audit` 白盒桶一致性检查
  并接入 `test_phys` 每个检查点;补尺寸桶跳过/切分用例。
- 继续缩短大对象路径的全局 `g_ext_lock` 临界区:空闲尺寸桶增加
  非空位图,分配从请求桶直接跳到下一个非空桶;释放侧页表清零与
  窗口 bump 的 `mmap` 移到锁外,锁内只做预留、状态与计数。mmap
  失败时回滚占位块与 active 计数,预留的 VA 洞直接跳过。audit 同步
  校验位图与桶头一致。

### Fixed

- 大对象分配改用交付块的实际页数 `e->npages` 计入 `live_bytes` 并
  登记页表,与释放侧口径一致;切分结点分配失败导致整块超额交付时,
  不再出现分配少记、释放多减的漂移。
- `test_stress` 的 RSS 总检容差补上线程缓存容量差(阶段间每多一个
  线程一份),修复 TSan 构建下 4→16 线程阶段 22.9 MB 增长被误判
  为复用失效的问题;精确的窗口 bump 断言不变。
- 空 slab 退役时回收 `wmm_slab_t` 头:此前头从只增不还的 base 区
  分配且从不复用,随机释放导致的 slab 生命周期会让元数据随运行
  时长线性增长,soak 的 RSS 门禁因此失败。新建 slab 优先复用退役
  头,取页失败也归还;guard 的备用头池保持不变。新增
  `WMM_FAULT_SLAB_HDR` 注入点,在逻辑头分配入口检查,复用链与
  base 两个来源都能确定性模拟头耗尽。
- `alloc_aligned`:alignment ≤ 4096 路径在 `size` 接近 `SIZE_MAX` 时
  加法回绕,可能返回不满足请求大小的块;`size == 0` 时返回的小块不
  满足更大的对齐。两处都按契约修正,并补 `test_api` 断言。
