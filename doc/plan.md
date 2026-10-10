# plan.md —— 小而精的 Linux 内存管理库(SDD)

> **本文件是 SDD 的单一执行源(plan.md)。**
> 实现时按 §4 任务依赖序执行,每任务以 **DoD(Definition of Done)** 判定完成;
> 每条规格(§3)必须有对应的自动化验收;所有 jemalloc 引用见规格附录 `allocator_build_guide.md`(下称 **指南**)与 `DESIGN.md`。

---

## 1. 目标与范围

### 目标
一个 Linux-only、内嵌于后端服务、~2.8k 行的内存管理库 `libwmm`,核心诉求:
**实时可观测 + 问题定位**(内存去哪了 / 谁在涨 / 越界与 UAF / 碎片程度)。

### 范围(做)
- 36 个小对象尺寸类 + 199 个大对象类(页面倍数);查表 size→index
- slab + freelist region;页表反向映射;extent 合并 + retained/dirty 两级
- 无锁 tcache(快路径)+ 自适应 GC;全局单 arena + bin 锁
- 统计/采样/泄漏检测/guard/quarantine/事件日志;共享内存监控协议
- 集成:`wmm_alloc` 显式 API + `-Wl,--wrap` 链接期接管

### 非目标(明确不做)
HPA/大页子系统、per-CPU arena、binshard、prof 家族、跨平台层、USDT、
符号重命名体系、edata 位域压缩、decay 平滑算法(用阈值 purge 替代)。

### 约束
- 平台:Linux x86-64,glibc;页 4K(`LG_PAGE=12`);C11
- 编译:`gcc -std=c11 -O2 -fno-omit-frame-pointer -fno-builtin-malloc -pthread`
- 产物:`libwmm.a`(静态)+ `libwmm.so`(可选)
- 快路径约束:**无锁、无系统调用、最多一次非原子整数累加**

---

## 2. 公共 API 契约(SPEC-API)

```c
/* wmm.h —— 公共 API,冻结后禁止修改签名(新增命令必须走 wmm_ctl) */
#ifndef WMM_H_INCLUDED
#define WMM_H_INCLUDED
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- 分配/释放(与 malloc 家族同语义,§12 方案 B 的 __wrap_* 转调这里) ---- */
void  *wmm_alloc(size_t size);
void  *wmm_calloc(size_t n, size_t size);
void  *wmm_realloc(void *p, size_t size);
void   wmm_free(void *p);
size_t wmm_usable_size(const void *p);
int    wmm_posix_memalign(void **out, size_t align, size_t size);
void  *wmm_aligned_alloc(size_t align, size_t size);

/* ---- 生命周期/集成钩子 ---- */
void wmm_boot(void);          /* 单线程,首个 wmm_alloc 前调用;可自动触发 */
void wmm_thread_init(void);   /* 可选;__thread 析构等效于 thread_exit */
void wmm_thread_exit(void);   /* 显式归还 tcache(G5) */
void wmm_atfork_prepare(void);/* 注册给 pthread_atfork */
void wmm_atfork_parent(void);
void wmm_atfork_child(void);  /* 子进程:重建锁 + __thread 状态 */

/* ---- 运行期控制与查询 ---- */
int  wmm_ctl(const char *cmd, ...);
int  wmm_stats_snapshot(wmm_stats_t *st);
int  wmm_event_drain(wmm_event_t *buf, size_t n, size_t *got);

/* ---- 观测结构(布局见指南 §10.2/§10.6,冻结) ---- */
typedef struct {
    uint64_t epoch;
    uint64_t allocated_bytes, deallocated_bytes;
    uint64_t live_bytes, live_objects;
    uint64_t active_bytes, frag_bytes, high_water_live;
    struct { uint64_t live, ndalloc, size; } cls[37];
    struct { uint64_t tid, alloc_bytes, free_bytes, alloc_cnt; } threads[128];
} wmm_stats_t;
typedef struct {
    uint64_t ts; uint32_t type; uint32_t tid;
    uintptr_t ptr; uint32_t cls; uintptr_t pc0, pc1;
} wmm_event_t;
#endif
```

**错误契约(SPEC-API-ERR,必须单测覆盖)**
1. OOM → 返回 NULL;`opt_abort` 开启时 abort
2. `wmm_free(NULL)` 无操作;`wmm_realloc(NULL,n)==wmm_alloc(n)`;`wmm_realloc(p,0)==wmm_free(p)+NULL`
3. `wmm_usable_size(p)` 对 `wmm_alloc` 返回值恒 ≥ 请求 size
4. `wmm_boot` 之前仅可调用 `wmm_boot`;wmm_ctl 未知命令返回非零并置 `errno=EINVAL`

**wmm_ctl 命令表(SPEC-CTL)**
```
stats.snapshot    快照(同步合并线程计数)         stats.per_class  直方图
sampling.enable|disable   采样开关(默认关)       leak.check|dump  B/A 档泄漏
guard.enable|disable      redzone+poison+quarantine(默认关)
purge                    强制回收 dirty          gc             强制全 tcache GC
config.lg_page           返回页面 2^N            config.verbose  verbose
```

---

## 3. 规格索引(实现与验收的唯一依据)

> 每条规格正文在**指南**对应章节;此处只登记编号、要点、验收。不变量 G1-G5 为全局规格,所有任务不得违反。

| 编号 | 规格 | 要点 | 验收(自动化) |
|---|---|---|---|
| G1 | 对齐(指南 §4.4) | 返回值 16B 对齐(8B 类 8B) | 属性测试:随机 size 断言对齐 |
| G2 | live 单点(§10.1) | per-class `live` 只在 bin 锁内改 | 测试钩子:每操作后 `Σlive[i]*size[i]==live_bytes` |
| G3 | 归属(§12) | 同指针不跨分配器 | CI 审计 + 冒烟 |
| G4 | 自举(§15.1) | 元数据全走 mmap 专用区 | 代码审查 + 无递归分配 |
| G5 | 无泄漏(§7.6) | 线程退出归还全部 tcache | 线程退出后 live 不变断言 |
| SC1 | 尺寸类表(§4.2) | 36 bin + 199 大对象类,生成器导出 | 生成表与指南 §4.2 逐行一致 |
| SC2 | size2index(§4.4) | ≤4096 查表,否则公式;结果最小上界 | 属性测试:index 的 size ≥ 请求,且上一类 < 请求 |
| SC3 | slab 页数(§4.3) | pgs=LCM(PAGE,reg)/PAGE | 生成表断言 1/2/3/5/7 |
| SLAB1 | slab 布局(§5.2) | region = addr + regind*reg_size,不重叠全覆盖 | 单测遍历全部 region |
| SLAB2 | freelist(§5.2) | pop/push 维护 nfree 与 free_head | 单测随机序列后 nfree==空闲数 |
| PTE1 | 页表(§6.1) | 每页项 → 所属 slab/extent | 随机分配后全量抽查 |
| PTE2 | 反查(§6.2) | free(p) 得正确类与 slab | 单测 + 属性测试 |
| TC1 | 快路径(§7.2) | 无锁正确;仅 1 次比较命中 | 多线程压测 + 结果正确 |
| TC2 | 批量(§7.3) | 填充后 ncached ≤ ncached_max | 单测断言 |
| TC3 | 自适应 GC(§7.5) | GC 后 ncached ≤ retain 公式 | 单测按公式断言 |
| TC4 | 线程退出(§7.6) | 全部归还 | 见 G5 |
| AR1 | bin 状态机(§8.2/8.4) | slabcur→nonfull(最低地址)→新 slab;全空归还 | 单测覆盖状态转移 |
| PHY1 | 合并(§9.3) | 相邻空闲合并 | 碎片测试:交替分配释放后 active 单调下降 |
| PHY2 | purge(§9.4) | dirty 超阈值回收 | 压测后 active 下降断言 |
| PHY3 | 大对象(§9.5) | ≥16K 页面倍数分配/释放 | 单测 + usable_size 断言 |
| OBS1 | 计数器(§10.1/10.2) | 快照字段全正确 | 同 G2 钩子 |
| OBS2 | 采样(§10.3) | TOP N 能定位调用点 | 注入"吃内存函数"→ TOP 可见 |
| OBS3 | 泄漏三档(§10.4) | C 档告警;A 档 dump | 注入泄漏 → 告警 + dump 定位 |
| OBS4 | guard(§10.5) | 越界/UAF/双释放可报 | 注入三类 bug → 事件日志命中 |
| SHM1 | 共享协议(§11) | seqlock 读一致性 | 监控进程快照 == 进程内快照 |
| API1 | 公共 API(§2) | 全部签名与错误契约 | 单测覆盖 SPEC-API-ERR 全项 |

---

## 4. 实现任务(依赖序,DoD 判定)

### P0 生成器与工程骨架

**T0.1 `gen_sc.py` → 生成 `wmm_sc.h`**
- 内容:size2index lookup 表(513 项)+ `bin_info[36]`(reg_size/pgs/slab_size/nregs)+ 大对象量化表
- DoD:脚本内断言与指南 §4.2 逐行一致;`SC1/SC3` 过

**T0.2 构建骨架**
- 内容:Makefile(debug/release/sanitize 三变体)、`wmm.h`(§2)、`bench/bench.c`(吞吐/latency/RSS,与 glibc 对照)
- DoD:空库可编译链接;`-fno-omit-frame-pointer` 生效(`nm` 无 FP 缺失警告);基准可复跑并输出三指标

### P1 核心(单线程正确性,无 tcache)

**T1.1 `wmm_core.c` 分配主路径**
- 内容:`wmm_boot`(页表 mmap + 36 bin 初始化)、`wmm_size2index`(SC2)、
  slab 取/还(freelist,SLAB1/2)、页表登记/反查(PTE1/2)、
  bin 状态机(AR1)、extent 合并 + retained(§9.2/9.3)、`wmm_alloc/free/realloc/usable_size`
- DoD:`API1` 全过;`G1/G2/G4` 钩子全绿;`SLAB/PTE/AR/PHY1` 单测过

**T1.2 测试骨架:属性测试(glibc oracle)**
- 内容:随机操作序列(alloc/free/realloc 混合),维护"活指针集合",
  每步与 glibc malloc 对照(指针集合一致、usize 断言、对齐断言)
- DoD:同序列下行为一致;`G1/G2` 钩子全程为真

### P2 tcache

**T2.1 `wmm_tcache.c` 快路径 + 批量**
- 内容:cache_bin 结构(§7.1)、快路径 alloc/free(§7.2)、fill/flush(§7.3)、ncached_max 公式(§7.4)
- DoD:`TC1/TC2` 过;多线程压测吞吐 ≥ 单线程 90%(无锁生效)

**T2.2 自适应 GC + 线程退出**
- 内容:GC 公式(§7.5)、`wmm_thread_exit` / `__thread` 析构(§7.6)
- DoD:`TC3/TC4/G5` 过;线程池反复创建销毁后 live 不变、RSS 不涨

### P3 物理层完善

**T3.1 `wmm_phys.c` purge 与大对象**
- 内容:后台聚合线程(每秒:pollute 检查 + 观测聚合 §11)、
  `MADV_DONTNEED/FREE` 回收(§9.4,运行期配置)、大对象完整路径(§9.5)、`wmm_ctl("purge")`
- DoD:`PHY2/PHY3` 过;大对象 stress 无碎片爆炸

### P4 可观测与共享内存(核心目标,提前)

**T4.1 `wmm_stats.c` 计数与事件**
- 内容:L1 线程计数(§10.1)、bin 锁内 live 维护(G2)、快照合并(§10.2)、事件日志(§10.6)
- DoD:`OBS1` 钩子全程为真;`wmm_event_drain` 正确

**T4.2 `wmm_shm.c` 共享内存协议**
- 内容:seqlock 快照区 + 事件环 + 采样表(§11.1 布局,固定 ABI)、后台线程每 1s 写入
- DoD:`SHM1` 过(独立监控进程读到与进程内一致的快照)

### P5 定位工具

**T5.1 `wmm_prof.c` 调用点采样**
- 内容:frame-pointer 栈遍历、环形缓冲、Horvitz-Thompson 聚合、TOP N(§10.3)
- DoD:`OBS2` 过(注入函数出现在 TOP N)

**T5.2 `wmm_guard.c` guard/quarantine + 泄漏三档**
- 内容:poison/redzone/quarantine(§10.5)、C/B/A 档泄漏(§10.4)
- DoD:`OBS3/OBS4` 全过(四类注入:泄漏/越界/UAF/双释放,均报出调用点)

### P6 集成与收尾

**T6.1 `wmm_wrap.c` 链接期接管**
- 内容:`__wrap_malloc/free/realloc/calloc/posix_memalign/aligned_alloc`(指南 §12 方案 B)、`wmm_atfork_*`
- DoD:`nm app` 仅见 `__wrap_*`;冒烟三连:跑通 → 统计正确 → 退出零泄漏;`G3` 审计过

**T6.2 监控面板与文档**
- 内容:独立监控进程(读共享区画曲线/告警)、playbook 落文档(指南 §13)
- DoD:面板实时显示全部指标;按 playbook 演练 7 类症状各一次

---

## 5. 测试策略(每条规格的验证手段)

| 层 | 手段 | 覆盖 |
|---|---|---|
| 单元 | 每 SPEC 独立用例(`tests/`) | SC/SLAB/PTE/TC/AR/PHY/API |
| 属性 | 随机操作序列 vs glibc oracle(T1.2) | G1/G2/SC2/PTE2/API1 |
| 压力 | 多线程随机分配 10 分钟 | TC1/G2/G5/PHY1 |
| 注入 | 四类 bug 注入(泄漏/越界/UAF/双释放) | OBS2/3/4 |
| 对照 | 同一属性测试跑 ASan + valgrind | 全规格交叉验证 |
| 基准 | `bench/bench.c` 吞吐/latency/RSS | 性能回归,入 CI |

**CI 门槛**:所有单元 + 属性 + 注入测试通过;`live` 恒等钩子全程为真;
基准与 glibc 对比在可接受区间(快路径同量级)。

---

## 6. 验收(对应指南 §16 的六个问题 → 任务)

| 问题 | 由谁验证 |
|---|---|
| 内存去哪了? | T5.1 → OBS2 |
| 谁在涨? | T4.1/T4.2 → per-thread/per-class 曲线 |
| 越界/UAF/双释放? | T5.2 → OBS4 |
| 碎片多严重? | T4.1 + T3.1 → `active−live`、`RSS−active` |
| 性能可接受? | T0.2 bench + T2.1 吞吐 |
| 稳定性? | 压力 + ASan/valgrind 对照 |
| 数据可信? | G2 钩子全程为真 |

**完成定义**:P0→P6 全部 DoD 通过;§6 七个问题均有可复现的验证步骤。

---

*规格附录:`allocator_build_guide.md`(设计规格全文)、`DESIGN.md`(jemalloc 架构)、`malloc_flow.html`(调用链)。*
