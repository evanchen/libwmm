# Changelog

格式参照 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/)。

## [Unreleased]

### Added

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

### Changed

- 物理层空闲 extent 改为尺寸分桶:1..256 页精确成桶,更大按
  `floor(log2(npages-1))` 分桶,DIRTY 与 RETAINED 分开;分配从请求
  桶向大扫,不再全地址链扫描,全局 `g_ext_lock` 临界区随之缩短。
  地址序链保留,只负责相邻合并与 purge 不变式。大对象页表登记移到
  锁外,块在锁内已标 INUSE。新增 `wmm_phys_audit` 白盒桶一致性检查
  并接入 `test_phys` 每个检查点;补尺寸桶跳过/切分用例。

### Fixed

- 空 slab 退役时回收 `wmm_slab_t` 头:此前头从只增不还的 base 区
  分配且从不复用,随机释放导致的 slab 生命周期会让元数据随运行
  时长线性增长,soak 的 RSS 门禁因此失败。新建 slab 优先复用退役
  头,取页失败也归还;guard 的备用头池保持不变。
- `alloc_aligned`:alignment ≤ 4096 路径在 `size` 接近 `SIZE_MAX` 时
  加法回绕,可能返回不满足请求大小的块;`size == 0` 时返回的小块不
  满足更大的对齐。两处都按契约修正,并补 `test_api` 断言。
