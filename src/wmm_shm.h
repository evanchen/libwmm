/* wmm_shm.h —— 共享监控区的线上布局与 seqlock 读端
 *
 * 这个头就是"监控协议"本身:库端(发布)与一切读端(测试、独立
 * 监控进程、将来别的语言重实现)都按这里的偏移解析同一份文件。
 * 改字段顺序或大小 = 改协议,必须同步升 WMM_SHM_VERSION——文件尾
 * 的 static_assert 把关键偏移在编译期钉死,挪字段的人会立刻编译
 * 失败,而不是上线后监控读花。
 *
 * 布局(总 0x10000 = 64KB):
 *   0x000  magic(4) + version(4)  读端第一道检查:对不上就是别人的文件
 *   0x008  events_total(8)        事件环累计写入条数(单调,只增)。
 *                                 环的写游标 = events_total % 环容量,
 *                                 因此最新 k 条的数组下标是
 *                                 (events_total-k .. events_total-1) 取模;
 *                                 读端记下上次的值,差值就是新事件数
 *   0x010  seq(8)                 seqlock:奇数 = 发布进行中
 *   0x018  publish_ts(8)          最近一次发布的 CLOCK_MONOTONIC 纳秒;
 *                                 与读端自己的同源时钟对比可判数据新鲜度
 *   0x020  wmm_stats_t 主快照     (0x13B8,布局冻结见 wmm.h)
 *   0x1800 TOP N 采样表           256 项 × 32B;调用点采样落地后填,
 *                                 未启用时恒为全零
 *   0x4000 事件环                  1024 条 × 0x30,整环镜像内部事件环
 *
 * 一致性协议(单写者 + 多读者):
 *   写者:seq 置奇 → acq_rel 栅栏 → 写数据 → release 栅栏 → seq 置偶;
 *   读者:s1 = seq(acquire) → 读数据 → acquire 栅栏 → s2 = seq,
 *         仅当 s1 == s2 且为偶才接受。
 *   栅栏方向按 C11 语义对准:acquire 面管住"之后的读写不许提前",
 *   release 面管住"之前的写不许推后"。用错了方向编译器就有权把
 *   数据写提到置奇之前,读者会拿着旧序号读走半新的数据。
 *   x86-64 的 TSO 保证普通访存本就不乱序,这几个栅栏不生成指令,
 *   只约束编译器(所以也没有用 seq_cst 全栅栏的硬件代价)。
 *
 * 读端为什么有界重试:写者卡死(进程被杀在写段中间,seq 永远为奇)
 * 时宁可报 EAGAIN 让监控显示"数据陈旧",也不无限空转。
 */
#ifndef WMM_SHM_H_INCLUDED
#define WMM_SHM_H_INCLUDED

#include "wmm.h"        /* wmm_stats_t / wmm_event_t:布局在公共头冻结 */

#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WMM_SHM_MAGIC   0x534D4D57u   /* 内存序字节 = "WMMS" */
#define WMM_SHM_VERSION 1u            /* 协议版本;布局一变就升 */
#define WMM_SHM_SIZE    0x10000u      /* 映射总大小 */
#define WMM_SHM_PROF_N  256           /* TOP N 采样表项数 */
#define WMM_SHM_EVENT_RING 1024       /* 事件环容量(与内部环同容量) */

typedef struct {
    uint32_t          magic;          /* WMM_SHM_MAGIC */
    uint32_t          version;        /* WMM_SHM_VERSION */
    uint64_t          events_total;   /* 事件环累计条数;与环在同一写段发布 */
    _Atomic uint64_t  seq;            /* seqlock;奇数 = 写入进行中 */
    uint64_t          publish_ts;     /* 最近发布的 CLOCK_MONOTONIC 纳秒 */
    wmm_stats_t       stats;          /* 主快照 */

    unsigned char pad_to_prof[0x1800 - 0x020 - sizeof(wmm_stats_t)];
    unsigned char prof[WMM_SHM_PROF_N * 32];   /* 0x1800:每项 {pc,bytes,count,reserved} */
    unsigned char pad_to_ring[0x4000 - 0x1800 - WMM_SHM_PROF_N * 32];
    wmm_event_t   events[WMM_SHM_EVENT_RING];  /* 0x4000:整环镜像,按数组序回放 */
} wmm_shm_t;

/* 读端重试上限:每次重试最多拷一份完整快照,100 次在微秒级——写者
   正常时一次就中,100 次全撞上写段只可能是写者被长期挂起 */
#define WMM_SHM_READ_TRIES 100

/* 一致读端:seqlock 重试。stats / events_total / publish_ts 按需,
 * 传 NULL 跳过对应字段的拷贝。成功返回 0;重试耗尽返回 -1 并置
 * errno = EAGAIN(数据此刻不可信,不是文件损坏)。
 *
 * 要同时用主快照和 events_total 时,必须走这一次调用——分两次读
 * 会拿到两个不同发布代的数(协议不保证跨调用一致)。 */
static inline int wmm_shm_read(const wmm_shm_t *h, wmm_stats_t *stats,
                               uint64_t *events_total, uint64_t *publish_ts)
{
    for (int i = 0; i < WMM_SHM_READ_TRIES; i++) {
        uint64_t s1 = atomic_load_explicit(&h->seq, memory_order_acquire);
        if (s1 & 1u)                       /* 写段进行中:直接下一轮 */
            continue;
        if (stats != NULL)
            memcpy(stats, &h->stats, sizeof *stats);
        if (events_total != NULL)
            *events_total = h->events_total;
        if (publish_ts != NULL)
            *publish_ts = h->publish_ts;
        atomic_thread_fence(memory_order_acquire);
        uint64_t s2 = atomic_load_explicit(&h->seq, memory_order_relaxed);
        if (s1 == s2)                      /* 首尾同一偶数值 = 完整一代 */
            return 0;
    }
    errno = EAGAIN;
    return -1;
}

/* 事件环的一致读端:把整环与累计条数在同一次保护内取出。要按
 * events_total 的增量找新事件时,条数与环必须是同一代的数据,
 * 因此本函数把两者放进同一个 seqlock 读段。 */
static inline int wmm_shm_read_events(const wmm_shm_t *h, wmm_event_t *dst,
                                      uint64_t *events_total)
{
    for (int i = 0; i < WMM_SHM_READ_TRIES; i++) {
        uint64_t s1 = atomic_load_explicit(&h->seq, memory_order_acquire);
        if (s1 & 1u)
            continue;
        memcpy(dst, h->events, sizeof h->events);
        if (events_total != NULL)
            *events_total = h->events_total;
        atomic_thread_fence(memory_order_acquire);
        uint64_t s2 = atomic_load_explicit(&h->seq, memory_order_relaxed);
        if (s1 == s2)
            return 0;
    }
    errno = EAGAIN;
    return -1;
}

_Static_assert(offsetof(wmm_shm_t, events_total) == 0x008, "SHM layout frozen");
_Static_assert(offsetof(wmm_shm_t, seq)          == 0x010, "SHM layout frozen");
_Static_assert(offsetof(wmm_shm_t, publish_ts)   == 0x018, "SHM layout frozen");
_Static_assert(offsetof(wmm_shm_t, stats)        == 0x020, "SHM layout frozen");
_Static_assert(offsetof(wmm_shm_t, prof)         == 0x1800, "SHM layout frozen");
_Static_assert(offsetof(wmm_shm_t, events)       == 0x4000, "SHM layout frozen");
_Static_assert(sizeof(wmm_shm_t) == WMM_SHM_SIZE, "SHM layout frozen");

#endif /* WMM_SHM_H_INCLUDED */
