# libwmm

小而精的 Linux 内存管理库:内嵌于后端服务,提供自定义内存管理 +
**实时可观测与问题定位**(内存去哪了 / 谁在涨 / 越界与 UAF / 碎片程度)。

> **状态**:开发中(in development)。按 `doc/tasks.md` 的模块分解,由作者手写实现。

## 设计目标

- 36 个小对象尺寸类 + 大对象类;slab + freelist 布局;页表反向映射
- 无锁 tcache 快路径 + 自适应 GC;全局单 arena + bin 锁
- extent 合并 + retained/dirty 两级物理层;purge 后台线程
- **可观测核心**:per-class/per-thread 计数、调用点采样、泄漏三档检测、
  guard/quarantine、事件日志、共享内存监控协议(seqlock)
- 集成:`wmm_*` 显式 API + `-Wl,--wrap` 链接期接管

## 构建

```sh
make debug     # -O0 -g -fsanitize=address,undefined,跑测试用
make release   # -O2,产物在 build/release/
make test      # 白盒用例 + --wrap 冒烟与符号审计
make bench     # wmm 与 glibc 的同负载对照表
make monitor   # 独立只读监控进程(不链接 libwmm)
make playbook  # 七类症状演练负载
```

产物:`build/{debug,release}/libwmm.a`、`build/release/libwmm.so`、
`build/test/run`、`build/test/wrap`、`build/monitor/monitor`、
`build/bench/bench`、`build/demo/playbook`。

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
`mixed`(大小混合)、`large`(16 KiB~1 MiB)。输出
`impl,scenario,ops,seconds,ops_per_sec,p50_ns,p99_ns,rss_kb`。

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
