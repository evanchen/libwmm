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

### Fixed

- `alloc_aligned`:alignment ≤ 4096 路径在 `size` 接近 `SIZE_MAX` 时
  加法回绕,可能返回不满足请求大小的块;`size == 0` 时返回的小块不
  满足更大的对齐。两处都按契约修正,并补 `test_api` 断言。
