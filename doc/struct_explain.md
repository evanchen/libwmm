# libwmm 数据结构关系图

> 结构定义的唯一权威是 [src/wmm_internal.h](../src/wmm_internal.h);本文是它的
> "总装图",按"从地到房"分层画出各结构的居住地与相互指向。配套源文件:
>
> | 文件 | 职责 |
> |---|---|
> | `wmm_boot.c` | 窗口预留、页表 mmap、base 区、全局状态定义点 |
> | `wmm_core.c` | bin/slab 状态机、页表辅助、alloc/free 主路径 |
> | `wmm_phys.c` | extent、取页/还页(窗口 bump) |
> | `wmm_sc.h` | 尺寸类常量表(由 `gen/gen_sc.py` 生成) |
>
> 代码演进后本文可能过时——与代码不符处,一律以代码为准。

---

## 1. 地基:VA 窗口 + 页表

boot 时一次 mmap 把整段 VA 窗口(期望 64GB,strict overcommit 下减半重试)
圈成 `PROT_NONE|MAP_NORESERVE`——只占地址空间,不占物理内存。此后所有
分配都发生在窗口内:取页 = `MAP_FIXED` 把一段预留原地改成可写;还页 =
盖回 PROT_NONE。窗口只向前 bump(`g_heap_cur`),不复用地址。

```
  g_heap_base            g_heap_cur(bump,只前进不后退)            g_heap_end
      │                        │                                      │
      ▼                        ▼                                      ▼
 ┌───────────┬───────────┬───────────┬───────────┬──────────────────────────┐
 │ 已退休的页 │ slab S1   │ slab S2   │ extent E1 │ 未动用(仍是 PROT_NONE)  │
 │ (盖回NONE)│ 的页      │ 的页      │ 的页      │                          │
 └───────────┴───────────┴───────────┴───────────┴──────────────────────────┘
       │           │           │           │
       ▼           ▼           ▼           ▼
 ┌───────────────────────── g_pte(平坦页表)──────────────────────────┐
 │ 下标 = (p − g_heap_base) >> 12;每 4K 页一项,一项 8B              │
 │ (64GB 窗口 = 128MB,仅 VA 预留;物理随触碰惰性增长,绝不 memset)   │
 │                                                                   │
 │      [ 0 ][ 0 ][S1头][S1头][S2头][E|1][E|1][ 0 ][ 0 ]...          │
 │        │         │           │         │                          │
 │        │         │           │         └ tag=1 → extent 结点指针|1 │
 │        │         │           └ tag=0 → slab 头指针                │
 │        │         └ 同一 slab 的每一页登记同一项:释放任意对象      │
 │        │           反查都落到同一归属                              │
 │        └ raw==0 → 未分配/已退休:free 反查落空 = 双释放/野指针     │
 └───────────────────────────────────────────────────────────────────┘
```

页表是"地址 → 归属"的唯一桥梁:free(p) 只拿到裸指针,查一次表即知 p 属于
哪个 slab(tag=0)或哪个大对象(tag=1),或根本不属于本分配器(raw==0)。
jemalloc 用多级基树(rtree)做同样的事以省内存;这里窗口定死为编译期常量,
平坦数组一次 mmap 到位,换来实现简单 + 单次访存。

---

## 2. 小对象世界:bin 是管家,slab 是资产(≤14336B)

```
 g_bins[36] —— 单 arena,一个尺寸类一个 bin,各自带锁
 │
 ├─ lock      保护本框内一切(三条链 + 全部计数)
 │
 ├─ slabcur ──────────►  热位 slab:多数分配从这出
 │                       (注意:满了 nfree==0 也仍占热位,不在 full 链)
 ├─ full_head ⇄─────────  满 slab 双向链(链体 = slab 的 full_prev/full_next)
 │                       只为 O(1) 摘除,不做查找
 └─ nonfull_root ⇄─────  非满 slab 地址升序链(链体 = ph_prev/ph_next)
                         链头 = 最低地址;分配永远取它
                         (ph_child 恒 NULL:配对堆升级的预留挂点)
 计数:live / ndalloc / nslabs / curslabs —— 锁内更新,任意时刻恒自洽
```

slab 的**四个位置**,`nfree` 驱动状态机:

```
 新造 ──install──► (slabcur 热位)
 (热位)    ──弹对象──►           (热位,nfree−−)
 (热位)    ──满且又来分配──►     (full 链)   同时 nonfull 头转正顶上热位
 (热位)    ──nfree==nregs──►     [退休]
 (full)    ──释放 1 个,nfree 0→1──► (nonfull 链)
 (full)    ──nregs==1 的类一次释放即全空──► [退休]
 (nonfull) ──nfree==nregs──►     [退休]
 (nonfull) ──热位空缺──►         (slabcur,链头转正)

 [退休] = 锁内摘链 → 放锁 → window_unmap 盖回 PROT_NONE + 页表清零
```

为什么分配永远取 nonfull 链头(最低地址):同批对象地址聚集,cache 局部性
好;低地址 slab 更容易先腾空整块归还。 jemalloc 的对应结构是 edata_heap
配对堆(O(log n));这里先用 O(n) 顺序找位换代码简单,n = 本类非满 slab
数,通常个位数。

nregs==1 的三个类(4096/8192/12288)分配即满、释放即空,永远不进
nonfull 链——状态机不必为它们写插入分支。它们存在的意义:请求 4K 差一点、
8K 差一点时给出近 exact 的规格,而不是向上凑整页浪费近一页。

---

## 3. slab 内部:零头部对象 + 侵入式空闲链

```
 wmm_slab_t 头(72B,住 base 区,一点不占堆页)
 ┌───────────────────────────────────────────────────────┐
 │ szind      ─► 归属哪个 bin(free 反查的回程票)        │
 │ nfree      ─► 0=满;nregs=全空(退休判据)             │
 │ free_head  ─► 空闲链头对象的编号(UINT32_MAX=链空)   │
 │ addr       ─► 窗口内基址(页对齐)                    │
 │ full_prev/full_next ─ 满链挂点(不在链上时为 NULL)    │
 │ ph_prev/ph_next     ─ 非满地址序链挂点(同上)        │
 │ ext        ─► 承载本 slab 的 extent 结点             │
 │              (当前物理层不做区间合并,恒 NULL;将来    │
 │               空闲区间按地址合并时,退休路径由此找到  │
 │               承载块与邻居合并)                      │
 └─────────────────┬─────────────────────────────────────┘
                   │ addr
                   ▼
    ┌────────┬────────┬────────┬────────┬────────────────┐
    │ obj 0  │ obj 1  │ obj 2  │ obj 3  │ ……  obj nregs−1│
    │[next=1]│ 已分配 │[next=3]│ 已分配 │      [UINT32_MAX]│
    └────────┴────────┴────────┴────────┴────────────────┘
      空闲对象自己当链结点:首 4 字节存下一个空闲对象的编号
      编号↔地址一次除法互推:regind = (p − addr)/reg_size
      建链 0→1→2→…,首批分配地址连续;释放是 LIFO 压头
```

对象不带任何头部是刻意的:元数据集中在 slab 头(base 区),堆页里的对象
密度做到理论最大。jemalloc 的对应物是 extent 头 + bitmap;bitmap 支持按位
查找但多约 200 行代码,空闲链一样 O(1) 且批量时从链头连续摘,够用。

建链那一次逐对象写入,是 slab 生命周期唯一一次全量触碰——页的物理内存在
建链时全部驻留,不存在分配时二次缺页。

---

## 4. 大对象世界:没有 slab、没有 bin(>14336B)

请求超过任何尺寸类的规格(含 14337..16383 的缝隙:近似到页比近似到
7 页 slab 更省)时,直接整页分配,不经 bin:

```
 wmm_alloc(大)──► ┌──────────────────────────┐   g_ext_free_nodes(回收链)
                  │ extent 结点(24B)        │◄── 壳复用:大对象生灭频繁,
                  │  addr   ─► 窗口内基址    │    逐块新拿会吃穿 base 区;
                  │  npages ─► 页数(向上取整)│──► 用完入链,空了才 base 新切
                  │  next   ─► 回收链挂点    │    (g_ext_lock 保护)
                  └───────────┬──────────────┘
                              │ window_map:MAP_FIXED 改 npages 页为可写
                              │ (bump 取址,窗口内只前进)
                              ▼
                       [E1 的页] 每页页表登记 (E1|1)
                              │
                              │ wmm_free(p):反查 tag=1 直接拿到结点
                              ▼
 wmm_phys_free_extent:还页 + 清页表 + g_large 统计(g_ext_lock)
                      + 结点壳入回收链
```

---

## 5. 元数据住在哪

铁律:分配器的元数据绝不从分配器自己拿(初始化完成前调用分配函数 =
无限递归),一律来自 mmap 或专用 bump 区。

```
 base 区(boot mmap 的一块,8B 对齐线性切,只进不出)
   ├─ slab 头 ×72B      与类流量同寿 → 不复用
   └─ extent 结点 ×24B  生灭频繁 → 壳走回收链复用(复用壳≠回收 base 块)

 独立 mmap,一次到位
   ├─ g_pte    窗口页数 × 8B;绝不 memset(零值=未分配,靠内核零页,
   │           memset 会凭空给 RSS 垫上页表全额的底)
   └─ 线程缓存:每线程一块 [tcache 头 | 36 个栈数组],线程退出整块还内核

 静态存储(wmm_boot.c 定义点)
   ├─ g_bins[36](含各自的锁)
   ├─ g_heap_base / g_heap_cur / g_heap_end
   ├─ g_ext_lock、g_large(大对象统计,与 ext 锁同域)
   └─ wmm_bin_info[36](只读常量表,gen_sc.py 生成)
```

线程缓存(tcache)结构已在 wmm_internal.h 定死、主路径尚未接线:它兼任
线程注册表节点(链入 g_threads,统计快照与缓存回收靠这张链找到人),每
尺寸类一个 cache_bin 小栈——弹栈即分配、压栈即释放、零锁;耗尽/攒满才
批量下 bin。落地后,bin 锁只在批量填充/刷出的慢路径出现。

---

## 6. free(p):把所有结构串起来的那条路

```
 wmm_free(p)
   │
   ├─ 不在 [g_heap_base, g_heap_end) ──► 非本库指针,诊断返回
   │
   ├─ 页表反查 raw==0 ──► 未分配/已退休:双释放或野指针(诊断返回)
   │
   ├─ tag=0(小对象)──► 拿到 slab 头 ──sl->szind──► 所属 bin
   │        持 bin 锁:wmm_slab_free 压链(nfree++)→ 状态机:
   │          ├─ nfree==1 且非热位:摘离 full,插入 nonfull
   │          ├─ nfree==nregs:退休(锁内摘链,放锁后还页+清页表)
   │          └─ 其余:原地变空,无动作
   │
   └─ tag=1(大对象)──► 直接拿到 extent 结点 ──► wmm_phys_free_extent
```

退休(还页)刻意放在 bin 锁之外:页表清零与 mmap 盖回只碰退休 slab 自己
的页,不碰任何 bin 状态;放进锁里只会让本类其他分配平白排队等一次 munmap。

---

## 7. 关系速查表

| 结构 | 住在哪 | 谁指向它 | 它指向谁 | 锁 |
|---|---|---|---|---|
| 窗口页 | 64GB VA 窗口 | g_pte 按页号索引 | — | g_ext_lock(仅 bump) |
| g_pte | 独立 mmap | — | slab 头 / extent 结点 | 写点在 bin 锁内(slab_init)或退休后(页已不可达) |
| wmm_slab_t | base 区 | 所属 bin 三链之一 + g_pte | addr→窗口页;ext(区间合并预留) | 所属 bin 的 lock |
| wmm_bin_t | 静态 g_bins | — | slabcur / full / nonfull | 自带 lock |
| wmm_extent_t | base 区 | g_pte、回收链 | addr→窗口页 | g_ext_lock |
| tcache(未接线) | 每线程独立 mmap | g_threads 链、g_tcache | 各类对象(窗口页) | 无锁(线程私有) |

---

## 一句话总结

**窗口是地,页表是"地址→归属"的桥,bin 管小对象(slab 四位置流转),
extent 管大对象(整页生灭),元数据全部住在分配器之外的 base 区/独立
mmap 里。**
