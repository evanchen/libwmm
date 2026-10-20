/* wmm_shm.h —— 监控共享区布局与 seqlock 读端 */

#ifndef WMM_SHM_H_INCLUDED
#define WMM_SHM_H_INCLUDED

#include "wmm.h"

#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WMM_SHM_MAGIC   0x534D4D57u
#define WMM_SHM_VERSION 1u
#define WMM_SHM_SIZE    0x10000u
#define WMM_SHM_PROF_N  256
#define WMM_SHM_EVENT_RING 1024

/* 共享区:monitor 只读;seqlock 保证 stats/prof/events 三段整体一致 */
typedef struct {
    uint32_t          magic;
    uint32_t          version;
    uint64_t          events_total;
    _Atomic uint64_t  seq;
    uint64_t          publish_ts;
    wmm_stats_t       stats;

    unsigned char pad_to_prof[0x1800 - 0x020 - sizeof(wmm_stats_t)];
    wmm_prof_entry_t prof[WMM_SHM_PROF_N];
    unsigned char pad_to_ring[0x4000 - 0x1800 -
                              WMM_SHM_PROF_N * sizeof(wmm_prof_entry_t)];
    wmm_event_t   events[WMM_SHM_EVENT_RING];
} wmm_shm_t;

#define WMM_SHM_READ_TRIES 100

/* seqlock 读端:seq 为奇数表示写入中;前后两次读值一致才算成功 */
static inline int wmm_shm_read(const wmm_shm_t *h, wmm_stats_t *stats,
                               uint64_t *events_total, uint64_t *publish_ts)
{
    for (int i = 0; i < WMM_SHM_READ_TRIES; i++) {
        uint64_t s1 = atomic_load_explicit(&h->seq, memory_order_acquire);
        if (s1 & 1u)
            continue;
        if (stats != NULL)
            memcpy(stats, &h->stats, sizeof *stats);
        if (events_total != NULL)
            *events_total = h->events_total;
        if (publish_ts != NULL)
            *publish_ts = h->publish_ts;
        atomic_thread_fence(memory_order_acquire);
        uint64_t s2 = atomic_load_explicit(&h->seq, memory_order_relaxed);
        if (s1 == s2)
            return 0;
    }
    errno = EAGAIN;
    return -1;
}

/* 读 TOP 表,seqlock 协议同上 */
static inline int wmm_shm_read_prof(const wmm_shm_t *h,
                                    wmm_prof_entry_t *dst)
{
    for (int i = 0; i < WMM_SHM_READ_TRIES; i++) {
        uint64_t s1 = atomic_load_explicit(&h->seq, memory_order_acquire);
        if (s1 & 1u)
            continue;
        memcpy(dst, h->prof, sizeof h->prof);
        atomic_thread_fence(memory_order_acquire);
        uint64_t s2 = atomic_load_explicit(&h->seq, memory_order_relaxed);
        if (s1 == s2)
            return 0;
    }
    errno = EAGAIN;
    return -1;
}

/* 读事件环,seqlock 协议同上 */
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

#endif
