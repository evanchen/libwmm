# 测试

每个模块对应一个 `test_*.c`,自测清单见 `doc/tasks.md` 各模块小节。

| 文件 | 覆盖 | 要点 |
|---|---|---|
| test_boot.c | 启动:窗口/页表零页/幂等/base 分配器 | 页表禁 memset 的 RSS 证据 |
| test_slab.c | slab 空闲链/页表登记/bin 状态机 | 满/非满/退休三态转换 |
| test_api.c | API 契约:对齐/usable/calloc/realloc/对齐分配/防御释放 | 契约逐条对照 glibc 语义 |
| test_phys.c | 物理层:合并/分裂/复用/同种子重放/purge 回收/后台线程 | 重放第二遍零新映射;dirty→RETAINED 页数守恒 + RSS 实测回落;跨状态合并重新记账 |
| test_oracle.c | oracle 属性测试:随机百万步对照参考模型 + glibc 对照 | 每步对账 + 每 10 万步收敛 |
| test_stress.c | 多线程压力:4/8/16 线程独立序列 | 屏障对账 + RSS 平稳观察 |
| test_tcache.c | 线程缓存:快路径命中率/容量上限/注册表/线程退出/GC 与自适应填充/fork 过继与幽灵清理/千次线程增删 | 白盒水位判命中,GC 落点按公式重放;子进程只回退出码 |
| test_stats.c | 统计与事件:快照增量对账/OBS1 独立重算/high_water/active 与 purge 差账/线程表/事件触发与 drain/ctl 接线 | 快照直读独立核算,不用快照自身的求和重放(恒真);组内关缓存、断言取增量 |
| test_shm.c | 共享内存 seqlock:创建契约/跨进程可见性/并发撕裂压力/映射与内部状态逐字节对账 | 子进程按路径只读 mmap 当外部监控;每次成功的读验单代恒等式(跨代不成立,撕裂必被抓住);EAGAIN 计数不算失败 |
| test_prof.c | 采样(后续里程碑) | 待实现 |
| test_guard.c | guard/泄漏三档(后续里程碑) | 待实现 |
| test_wrap.c | --wrap 集成(后续里程碑) | 待实现 |

统一入口:`tests/main.c` 维护用例表(每个用例文件导出 `int test_xxx(void)`,
返回失败断言数,0 = 通过);用例是白盒的,可直接包含 `src/wmm_internal.h`。
`make test` 编译并运行全部用例(链接 debug 库,ASan/UBSan 生效)。
