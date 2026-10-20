# playbook —— 七类线上症状的演示与读数

本文把指南里的症状表落成可执行的演练:每条症状都有负载进程、监控命令、
要看的字段和判定结论。所有负载都在 `demo/playbook.c`,监控端在
`monitor/monitor.c`,二者只通过共享文件说话。

## 0. 准备

```sh
make playbook          # release 库 + -no-pie 的 build/demo/playbook
make monitor           # 只读监控进程 build/monitor/monitor
```

演练进程启动后会打印自己的 pid 和一条可直接复制的监控命令,例如:

```
playbook: mode=leak pid=12345 shm=/tmp/libwmm-playbook-leak.shm ...
monitor: ./build/monitor/monitor /tmp/libwmm-playbook-leak.shm --pid 12345
```

另开一个终端运行监控。监控随时可以退出或重新 attach,不影响目标;
目标退出后共享文件仍保留最后一代数据,用 `--once` 还能读现场。

## 1. 数据出口

| 出口 | 回答什么 |
|---|---|
| monitor 主行 `live/active/frag` | 总体是否在涨、映射页与存活量差多少 |
| monitor `cls:` TOP5 | 哪几个尺寸类占着内存 |
| monitor `thr:` TOP5 | 哪个线程本周期净分配最多 |
| monitor CSV | 逐周期曲线,事后画图或做 epoch 对比 |
| monitor 事件行 | 越界/UAF/双释放/泄漏告警的类型、地址、尺寸类、调用点 |
| `leak.check` | 两次调用之间按调用点聚合的增量,泄漏归因 |
| `high_water_live` | 历史峰值;按快照更新,演示批次保持 1 秒,足以被每秒发布抓到 |
| `addr2line` | 把事件/采样里的 pc 还原到源文件行号 |

`live` 含线程缓存中的对象;要看应用真实持有量,先让目标线程刷缓存
(测试里用 `wmm_tcache_flush_all`,演示进程会在关键节点调 `gc`)。
`frag = active - live`,单位是字节;监控同时打印它占 active 的百分比。

## 2. 内存持续上涨

**复现**

```sh
# A 终端
./build/demo/playbook leak
# B 终端(路径与 pid 用 A 打印的)
./build/monitor/monitor /tmp/libwmm-playbook-leak.shm --pid <pid> \
    --live-high 8M --csv /tmp/leak.csv
```

默认每秒分配 1024 个 1 KiB 对象且不释放,持续 15 秒。

**看什么**

- 主行 `live` 每秒上升约 1 MiB,`objs` 同步上升,`active` 跟着涨;
- 第 5~6 秒左右事件行出现 `type=0xDA leak-alert`(连续 5 个上涨窗口
  且累计涨幅过 64 KiB);
- 负载进程每 5 秒打印一次 `leak.check` 增量,`pc` 就是泄漏分配点;
- CSV 的 `live_bytes` 是一条稳定斜线,`publish_age_ms` 保持在一秒左右
  (发布周期为 1 秒;明显变大说明目标进程的后台线程卡住了)。

**结论**

`live` 单调上涨、`frag` 不随释放回落、事件环给出 0xDA,三者同时成立
就是泄漏,不是碎片。`leak.check` 的 TOP 行把字节数摊到调用点:

```sh
addr2line -f -C -e build/demo/playbook 0x<pc>
```

**数据出口**:主行趋势 + 0xDA 事件 + `leak.check` TOP + `addr2line`。

## 3. 天级缓慢泄漏

**复现**

```sh
./build/demo/playbook slowleak --seconds 60
./build/monitor/monitor /tmp/libwmm-playbook-slowleak.shm --pid <pid> \
    --csv /tmp/slowleak.csv
```

默认每秒 2 个 16 KiB 大对象(32 KiB/s),是 `leak` 模式(1 MiB/s)
的三十二分之一。用大对象而不是小对象是有意的:小对象经 tcache
批量填充,低速分配时 `live` 呈阶梯状(每凑满一批才跳一次),1 秒的
上涨窗口会被平台期打断;C 档看的是窗口,演示必须让每个窗口都有
可观测的增长。

**看什么**

- 主行 `live` 的斜率明显小于 `leak` 模式,单看一秒的增量不大;
  CSV 连起来才是持续上涨;
- 约 5~6 秒后仍会出现 0xDA:C 档要求的是"连续 5 个上涨窗口 **且**
  累计过 64 KiB",不是只看斜率,慢而稳的增长同样会命中;
- `leak.check` 在演示窗口里可能没有行:采样率是 1/1000,按 2 次/秒
  算,一分钟只有一百多次分配,期望样本不足一个。这不是故障。生产里
  按天运行,样本会自然累积;要立刻演示归因,用第 2 节的 `leak` 模式。

**结论**

慢泄漏用 CSV/epoch 对比确认趋势,用 0xDA 确认已越过告警门槛;
采样归因需要与时间尺度匹配的样本量。把 `--seconds` 拉长或提高
`--rate` 可以看到 `leak.check` 逐渐出现同一调用点。

**数据出口**:CSV 长期斜率 + 0xDA + (样本足够后的)`leak.check`。

## 4. RSS 高但业务正常(碎片)

**复现**

```sh
./build/demo/playbook rss
./build/monitor/monitor /tmp/libwmm-playbook-rss.shm --pid <pid> \
    --frag-pct 50 --csv /tmp/rss.csv
```

负载先分配 20 万个对象(四种尺寸轮转),再把其中 7/8 释放,每个
slab 都留一个存活对象,然后执行 `gc` 与同步 `purge`,保持 12 秒。

**看什么**

- `rss-full` 阶段 live 与 active 接近;
- 稀疏释放后 `live` 掉到峰值的约 1/8,`active` 基本不动,`frag`
  长时间停在 85%~90%;
- `cls:` TOP5 仍能看到 128/256/384/512 几个类各留着约 1/8 的对象;
- 同步 `purge` 之后 frag 不下降:还能回收的全空 slab 已经回收完,
  剩下的是"有活对象的 slab 里的空槽位";若这次 purge 真收回了页,
  事件环会出现一条 `0xDC`(回收动作本身),不影响上面的判定;
- `thr:` 在释放后不再有正的净增量,排除"还有线程在涨"。

**结论**

RSS 高、业务正常时先分两种:

1. `active - live` 高且 purge 后仍高:存活对象稀疏地钉住了 slab,
   属于内部碎片。按 `cls:` 找集中在哪些类,再考虑调整对象尺寸分布、
   降低跨类 churn 或减少峰值批次的跨度;
2. 如果对象全部释放,空 slab 会转成 dirty extent 并被后台/同步
   purge 归还,`frag` 会明显回落——这正是"碎片"与"没释放"的分界。

**数据出口**:`active` vs `live` vs RSS + `cls:` + 同步 purge 后的 frag。

## 5. 偶发崩溃(越界写)

**复现**

```sh
./build/demo/playbook oob
./build/monitor/monitor /tmp/libwmm-playbook-oob.shm --pid <pid>
```

负载在数据区尾部紧邻的 redzone 首字节写 0x55,再 `free`。

**看什么**

```
ev type=0xBD redzone-overflow tid=... ptr=0x... cls=... pc0=0x... pc1=0x...
```

- `ptr` 是**被踩的哨兵地址**,不是对象首地址;它落在 redzone 里,
  用 `wmm_usable_size` 可以反推对象;
- `pc0` 是释放调用点,`pc1` 是分配调用点;
- 事件在 `free` 当场产生,不必等后台周期。

**结论**

0xBD 证明越界写真实发生,并给出释放点与分配点。用 `addr2line` 解析
两个 pc,沿分配点到释放点之间的所有写入检查数组下标/长度。监控先于
SIGSEGV 发现问题,崩溃现场因此可复现、可归因。

**数据出口**:0xBD 事件 + 两帧调用点 + `addr2line`。

## 6. 释放后使用(UAF)

**复现**

```sh
./build/demo/playbook uaf
./build/monitor/monitor /tmp/libwmm-playbook-uaf.shm --pid <pid>
```

负载释放对象后立刻写 `p[0]`,然后保持进程存活。

**看什么**

- 一到两个后台周期内(默认 tick 为 1 秒)出现
  `type=0xBF uaf-quarantine`;
- `pc0` 是**分配调用点**,`pc1` 为 0:隔离期校验发生在后台 tick,
  本库无法回溯是谁在释放后写了这块内存;
- `ptr` 指向第一个被改坏的字节(数据区偏移 0);
- 如果对象已经离开隔离区/回收池、物理页已归还,后续访问不再产生
  guard 事件:检测窗口就是隔离队列与回收池保留的那段时间,页还给
  内核后地址归属也随之消失。

**结论**

0xBF 证明"对象在隔离期内被写",配合分配点找到对象来源,再在该
对象生命周期里查找缺少的持有关系或释放后继续使用的路径。检测窗口
由隔离队列容量与 tick 周期共同决定:刚释放的热点对象最容易被抓到。

**数据出口**:0xBF 事件 + 分配点 + `addr2line`。

## 7. 双释放

**复现**

```sh
./build/demo/playbook double
./build/monitor/monitor /tmp/libwmm-playbook-double.shm --pid <pid>
```

负载对同一指针连续 `free` 两次。

**看什么**

```
ev type=0xBE bad-free tid=... ptr=0x... cls=... pc0=... pc1=...
```

- 事件在第二次 `free` 当场产生,`pc0` 是第二次释放的调用点,
  `pc1` 是首次分配点;
- `ptr` 是对象地址,`cls` 是尺寸类;
- 若指针完全不在堆窗口内,`cls` 为 0,`pc0/pc1` 退化为释放路径的
  两帧调用者,同样能定位调用方。

**结论**

0xBE + 两个 pc 足以区分"同一指针释放两次"与"释放野指针/内部指针"。
双释放往往与错误分支里的重复清理有关,重点检查 `pc0` 所在函数的
所有退出路径。

**数据出口**:0xBE 事件 + `ptr/cls` + 两帧调用点。

## 8. 峰值抖动

**复现**

```sh
./build/demo/playbook peak
./build/monitor/monitor /tmp/libwmm-playbook-peak.shm --pid <pid> \
    --live-high 64M --csv /tmp/peak.csv
```

负载每批拉起 2048 个 4 KiB 对象,保持一秒,全部释放,再停一秒,循环。

**看什么**

- 主行 `peak=` 是 `high_water_live`,批次释放后**不会**回落,它记录的是
  历史峰值(在统计快照时更新,后台发布每秒一次);
- `live` 呈方波,`active` 跟随,`frag` 在释放后短暂升高、后台 purge
  后再回落;
- `thr:` TOP5 指向执行批次的线程,`cls:` 指向 4096 B 类;
- CSV 的 `high_water_live` 列可以对齐到具体时刻和 epoch。

**结论**

峰值问题看 `high_water_live` 与当时的 per-thread/per-class,而不是
稳态均值。若峰值远高于业务预期,先查批次大小与线程数的乘积,
再决定限流、拆分批次或提高尺寸类复用率。

**数据出口**:`high_water_live` + `thr:` + `cls:` + CSV 对齐。

## 9. 事件字段速查

| 类型 | 名称 | `ptr` | `pc0` | `pc1` |
|---|---|---|---|---|
| 0xBC | uaf-pool-write | 首个被改坏的字节 | 分配点 | 0 |
| 0xBD | redzone-overflow | 被踩的哨兵地址 | 释放点 | 分配点 |
| 0xBE | bad-free | 对象地址/野指针 | 本次释放点 | 分配点(guard 路径) |
| 0xBF | uaf-quarantine | 首个被改坏的字节 | 分配点 | 0 |
| 0xDA | leak-alert | 0 | 后台 tick 调用链 | — |
| 0xDB | oom | 0 | 失败路径调用链 | — |
| 0xDC | purge | 0 | 回收路径调用链 | — |

0xDA/0xDB/0xDC 的 pc 是库内路径,用于确认"谁在什么时候报的",
不用于业务归因;业务归因看 `leak.check`、per-class 与 per-thread。

## 10. 收尾与清理

- 目标进程退出后:

  ```sh
  ./build/monitor/monitor /tmp/libwmm-playbook-<mode>.shm --once
  ```

  读最后一代快照与最近事件;`--once` 在读失败时返回非 0,适合脚本。

- 共享文件不会被库自动删除(残留数据对事后分析有用),换模式或
  重跑前不需要手工清理:`shm.create` 用 `O_TRUNC` 覆盖旧文件。重跑
  同一模式前先停掉还挂在旧文件上的监控进程,否则它会在文件被截断
  后读到无效页;不同模式用不同路径,互不影响。

- 演练负载用 release 库并加 `-no-pie`;不要套 ASan 跑故障模式,
  sanitizer 会先于分配器终止进程:

  ```sh
  addr2line -f -C -e build/demo/playbook 0x<pc>
  ```
