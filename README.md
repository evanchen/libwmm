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

(待实现 —— 构建骨架为 `doc/tasks.md` M0 的手写任务)

```sh
make debug     # -O0 -g -fsanitize=address,undefined
make release   # -O2
make test
make bench
```

产物:`libwmm.a`(静态)+ `libwmm.so`(可选)。

## 文档

| 文档 | 说明 |
|---|---|
| `doc/plan.md` | SDD 单一执行源:规格(SPEC)与 DoD |
| `doc/tasks.md` | 手写实现任务分解(M0~M16,含自测清单) |
| `doc/allocator_build_guide.md` | 设计规格全文 |
| `doc/DESIGN.md` | jemalloc 架构分析(规格参考) |

## 许可证

BSD-2-Clause,详见 [LICENSE](LICENSE)。
