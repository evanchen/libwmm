# 测试

每个模块对应一个 `test_*.c`,自测清单见 `doc/tasks.md` 各模块小节。

| 文件 | 覆盖 | 要点 |
|---|---|---|
| test_boot.c | 启动:窗口/页表零页/幂等/base 分配器/运行期配置命令 | 页表禁 memset 的 RSS 证据 |
| test_slab.c | slab 空闲链/页表登记/bin 状态机 | 满/非满/退休三态转换 |
| test_api.c | API 契约:对齐/usable/calloc/realloc/对齐分配/防御释放/OOM errno | 契约逐条对照 glibc 语义 |
| test_phys.c | 物理层:合并/分裂/尺寸桶/复用/slab 头回收/同种子重放/purge 回收/后台线程 | 重放第二遍零新映射;dirty→RETAINED 页数守恒 + RSS 实测回落;跨状态合并重新记账;退役 slab 头被下一次新 slab 复用;每个检查点跑尺寸桶一致性审计 |
| test_oracle.c | oracle 属性测试:随机百万步对照参考模型 + glibc 对照 | 每步对账 + 每 10 万步收敛 |
| test_stress.c | 多线程压力:4/8/16 线程独立序列 | 屏障对账 + RSS 平稳观察 |
| test_tcache.c | 线程缓存:快路径命中率/容量上限/注册表/线程退出/GC(含跨线程请求)与自适应填充/大对象按页数分类缓存与按取用量 GC/滚动收缩按页限额与游标轮转/上限与预算 ctl/fork 过继与幽灵清理/千次线程增删 | 白盒水位判命中,GC 落点按公式重放;大缓存逐类核对 head/count/used 与滚动页预算;跨线程请求位用两段握手观察;子进程只回退出码 |
| test_stats.c | 统计与事件:快照增量对账/live 恒等式独立重算/high_water/active 与 purge 差账/线程表/事件触发与 drain/ctl 接线 | 快照直读独立核算,不用快照自身的求和重放(恒真);组内关缓存、断言取增量 |
| test_shm.c | 共享内存 seqlock:创建契约/跨进程可见性/并发撕裂压力/映射与内部状态逐字节对账/TOP 采样表发布 | 子进程按路径只读 mmap 当外部监控;每次成功的读验单代恒等式(跨代不成立,撕裂必被抓住);EAGAIN 计数不算失败;TOP 表经监控端读口读回并与内部取数逐字节比对 |
| test_fail.c | 故障注入:窗口 mmap/purge madvise/线程缓存创建/元数据块分配失败 | 注入后 live/active/累计计数零漂移、事件环留 0xDB、解除后同一操作立即成功;MAP 用例先抽干空闲 extent,保证必达窗口 mmap |
| test_prof.c | 调用点采样:开关/节拍/调用点折叠/TOP 排序/环形缓冲绕圈/线程退出归账/ctl 参数契约 | 每个吃内存函数一个专属尺寸,按 `bytes == count × size` 识别行;绕圈后 count 精确等于保留样本数 × 权重;关闭时环形缓冲 head 不动;pc 打印供 addr2line 对名 |
| test_guard.c | guard/泄漏三档:单对象 slab 布局/redzone/隔离期 UAF/双重释放/野指针/leak.dump/leak.check/持续上涨告警/开关切换 | 事件按"类型 + 精确指针"认定,不数条数(同一坏 slab 会在 tick/复用/关闭时重复报);坏内存写刻意为之,检查的是检测能力而不是 ASan |
| test_wrap.c | --wrap 集成:七个 malloc 家族入口的混合负载/接管证据/统计对账/零泄漏 | 独立程序(自带 main),以 `-Wl,--wrap=...` 单独链接成 `build/test/wrap`;指针落在堆窗口内才算接管成功;`make test` 运行前做 nm 审计 |

统一入口:`tests/main.c` 维护用例表(每个用例文件导出 `int test_xxx(void)`,
返回失败断言数,0 = 通过);用例是白盒的,可直接包含 `src/wmm_internal.h`。
`make test` 编译并运行全部用例(链接 debug 库,ASan/UBSan 生效)。
`make tsan` 用独立的 `build/tsan` 目标目录编译同一套用例(TSan 不能与
ASan 混用),并以 `tests/tsan.supp` 抑制 prof/shm 两处自实现 seqlock 的
payload 假阳性;其余 race 报告一律按失败处理。

例外:`test_wrap.c` 不进 `tests/main.c` 的表。它必须以 `--wrap` 链接,
而主测试二进制故意不 wrap(test_oracle 的 glibc 对照依赖真 malloc 家族),
所以由 Makefile 单独构建、在主用例跑完后运行。

## 生产验证

`validate/` 下的 soak、forkstress、oomcheck 都是独立进程,链接 release
库,sanitizer 不介入。它们不登记进 `tests/main.c`,也不共享单元测试的
全局状态;由 `make validate` 与 `validate/gate.py` 编排,详见根目录
README 的“生产验证”。OOM 用例会真的填满 64 GB 虚拟窗口(约 128 MB
页表驻留),不要和 `make test` 并行跑。
