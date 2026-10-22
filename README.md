# libwmm

小而精的 Linux 内存管理库:内嵌于后端服务,提供自定义内存管理 +
**实时可观测与问题定位**(内存去哪了 / 谁在涨 / 越界与 UAF / 碎片程度)。

> **状态**:开发中(in development)。按 `doc/tasks.md` 的模块分解,由作者手写实现。

## 设计目标

- 36 个小对象尺寸类 + 大对象类;slab + freelist 布局;页表反向映射
- 无锁 tcache 快路径 + 自适应 GC;大对象按精确页数分类的线程缓存(每类 20 槽、有界预算、按取用量收缩,滚动收缩按页限额轮转);全局单 arena + bin 锁
- 尺寸分桶空闲链 + 非空桶位图 + extent 合并;retained/dirty 两级物理层;purge 后台线程
- **可观测核心**:per-class/per-thread 计数、调用点采样、泄漏三档检测、
  guard/quarantine、事件日志、共享内存监控协议(seqlock)
- 集成:`wmm_*` 显式 API + `-Wl,--wrap` 链接期接管

## 构建

```sh
make debug     # -O0 -g -fsanitize=address,undefined,跑测试用
make release   # -O2,产物在 build/release/
make test      # 白盒用例 + --wrap 冒烟与符号审计
make tsan      # 数据竞争检查(独立的 build/tsan)
make bench     # wmm 与 glibc 的同负载对照表
make validate  # 生产验证基座(压测/soak/fork/OOM/cgroup 门禁)
make monitor   # 独立只读监控进程(不链接 libwmm)
make playbook  # 七类症状演练负载
```

产物:`build/{debug,release}/libwmm.a`、`build/release/libwmm.so`、
`build/test/run`、`build/test/wrap`、`build/monitor/monitor`、
`build/bench/bench`、`build/demo/playbook`、`build/validate/`。

## 使用

### 显式 API

```c
#include "wmm.h"

wmm_boot();
void *p = wmm_alloc(1024);
wmm_free(p);
```

### 共享监控区

目标进程创建共享文件并启动后台发布:

```c
wmm_ctl("shm.create", "/tmp/app.shm");
wmm_ctl("bg.enable");
```

任意进程只读观察(不需要链接 libwmm,只要求监控协议版本一致):

```sh
./build/monitor/monitor /tmp/app.shm --pid <pid> \
    --live-high 256M --frag-pct 30 --csv /tmp/app.csv
```

每个周期输出 live/active/frag、尺寸类 TOP5、线程净增量与事件;
告警写 stderr,数据写 stdout/CSV。目标退出后用 `--once` 读残留现场。

### 基准

```sh
make bench
# 或单独跑
./build/bench/bench wmm   --scenario mixed --threads 8 --ops 5000000
./build/bench/bench glibc --scenario mixed --threads 8 --ops 5000000
```

场景:`seq`(单线程分配/释放)、`mt`(多线程随机槽位)、
`mixed`(大小混合)、`large`(16 KiB~1 MiB)、`server`(模拟请求:
1~6 个对象、少量 realloc、会话槽位跨请求存活)。输出 14 列:

```
impl,scenario,ops,seconds,ops_per_sec,p50_ns,p99_ns,rss_kb,
p999_ns,p9999_ns,max_ns,reqs,req_per_sec,alloc_fail
```

前 8 列含义不变。`server` 的延迟单位是一次请求,`reqs`/`req_per_sec`
是请求数与请求吞吐;其余场景的计时单位是一次分配器调用,`reqs` 等于
`ops`。`alloc_fail` 非零表示本轮压测少做了工作,结果不可信。

`make bench` 把每个实现/场景拆成独立进程运行,并在同一进程只跑一个
场景,避免前一轮的分配器存量污染 RSS。`BENCH_REQUESTS` 控制 server
场景的请求总数(默认 200000)。

尾延迟归因可用 `--server-big 0`、`--server-realloc 0`、
`--server-session 0`、`--server-touch 0` 逐项关闭 server 的负载成分;
关闭时随机流不变,可直接横向对照。每次 server 运行还会在 stderr 打印
最慢请求的对象数、最大尺寸、调用数与标志位,不影响 CSV 行。

### 生产验证

`make validate` 用 release 库跑一套带明确判定的生产验证,数据落在
`build/validate/`:

```sh
make validate        # 约 1-2 分钟:bench + 60s soak + fork + OOM + cgroup
make validate-full   # soak 30 分钟、bench 重复 3 次取中位数
```

单独运行:

```sh
make soak SOAK_SECONDS=3600 SOAK_ARGS="--threads 8"
make forkstress FORK_ARGS="--seconds 30 --forks 300"
make oomcheck
make cgroup-oom CGROUP_ARGS="--live-mb 64 --max-mb 192 --seconds 5"
```

| 工具 | 检查 |
|---|---|
| `validate/soak.c` | 分阶段升降负载、跨线程释放、线程 churn;每秒快照不变式、事件环、RSS 增长上限;结束时 live/active/缓存归零 |
| `validate/forkstress.c` | 活跃分配与统计压力下反复 fork;子进程校验快照后 `_exit`;专用 forker 线程与主线程 fork 两种形态;guard 模式另跑一轮 |
| `validate/oomcheck.c` | 用 64 MB extent 填满真实 64 GB 窗口;检查 NULL/ENOMEM 契约、旧块在 realloc 失败后完好、释放后可复用;子进程验证 `abort_oom` 的 SIGABRT |
| `validate/cgroup_oom.sh` | cgroup v2 限额内 bounded 探针,检查退出码、oom_kill、memory.peak;无权限时 SKIP(77) |
| `validate/gate.py` | 编排上述进程,硬门禁:退出码、alloc_fail、server p999 尾延迟、各工具 PASS、cgroup 结果 |

默认硬门禁不比较吞吐排名,只抓病态:server p999 必须不超过
`max(10 ms, 10 × glibc p999)`;p9999/max 与吞吐比只进报告。
`--strict-perf` 可把吞吐比也纳入门禁。cgroup 不可用时报告 SKIP,
`make cgroup-oom REQUIRE_CGROUP=1` 或 gate 的 `--require-cgroup`
会把 SKIP 当失败,避免“没测”被当成“通过”。

`build/validate/report.txt` 一行一个门禁,`report.json` 带主机、
参数、bench 行、对比数据与门禁明细;`bench.csv`、`soak.csv` 与各
进程日志同目录留存。

### 问题定位演练

`doc/playbook.md` 给出七类症状的复现命令、要看的字段与判定结论;
负载在 `build/demo/playbook`:

```sh
./build/demo/playbook leak     # 或 slowleak/rss/oob/uaf/double/peak
```

## 文档

| 文档 | 说明 |
|---|---|
| `doc/plan.md` | SDD 单一执行源:规格(SPEC)与 DoD |
| `doc/tasks.md` | 手写实现任务分解(M0~M16,含自测清单) |
| `doc/allocator_build_guide.md` | 设计规格全文 |
| `doc/DESIGN.md` | jemalloc 架构分析(规格参考) |
| `doc/playbook.md` | 七类症状演练:复现、读数、结论 |

## 许可证

BSD-2-Clause,详见 [LICENSE](LICENSE)。
