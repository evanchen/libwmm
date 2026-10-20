/* wmm_shm.c —— 共享监控区的创建与单写者发布 */

#include "wmm_internal.h"
#include "wmm_shm.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

static wmm_shm_t *g_shm;

static pthread_mutex_t g_shm_create_lock = PTHREAD_MUTEX_INITIALIZER;

static wmm_event_t g_ring_scratch[WMM_SHM_EVENT_RING];

_Static_assert(WMM_EVENT_RING == WMM_SHM_EVENT_RING,
               "事件环镜像按整环 memcpy,两侧容量必须相等");

_Static_assert(WMM_PROF_TOP_MAX == WMM_SHM_PROF_N,
               "TOP 表容量与共享区不一致");

static wmm_prof_entry_t g_prof_scratch[WMM_SHM_PROF_N];

/* 单写者发布:seq 变奇数,写完全部 payload,再变偶数。
   读端看到奇数或前后 seq 不一致就丢弃本次读取。 */
static void shm_publish_to(wmm_shm_t *h) {
    wmm_stats_t st;
    (void)wmm_stats_snapshot(&st);

    uint64_t ev_total = 0;
    wmm_event_copy(g_ring_scratch, &ev_total);

    (void)wmm_prof_top(g_prof_scratch, WMM_SHM_PROF_N);

    uint64_t ts = wmm_mono_ns();

    uint64_t s = atomic_load_explicit(&h->seq, memory_order_relaxed);
    atomic_store_explicit(&h->seq, s + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_acq_rel);

    memcpy(&h->stats, &st, sizeof st);
    h->events_total = ev_total;
    h->publish_ts   = ts;
    memcpy(h->prof, g_prof_scratch, sizeof g_prof_scratch);
    memcpy(h->events, g_ring_scratch, sizeof g_ring_scratch);

    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&h->seq, s + 2, memory_order_relaxed);
}

void wmm_shm_publish(void) {
    wmm_shm_t *h = g_shm;
    if (h != NULL)
        shm_publish_to(h);
}

/* 创建并初次发布;每进程只允许一个共享区,重复创建返回 EEXIST */
int wmm_shm_create(const char *path) {
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g_shm_create_lock);
    if (g_shm != NULL) {
        pthread_mutex_unlock(&g_shm_create_lock);
        errno = EEXIST;
        return -1;
    }

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        pthread_mutex_unlock(&g_shm_create_lock);
        return -1;
    }

    if (ftruncate(fd, (off_t)WMM_SHM_SIZE) != 0) {
        int e = errno;
        (void)close(fd);
        (void)unlink(path);
        pthread_mutex_unlock(&g_shm_create_lock);
        errno = e;
        return -1;
    }

    wmm_shm_t *h = mmap(NULL, WMM_SHM_SIZE, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
    if (h == MAP_FAILED) {
        int e = errno;
        (void)close(fd);
        (void)unlink(path);
        pthread_mutex_unlock(&g_shm_create_lock);
        errno = e;
        return -1;
    }
    (void)close(fd);

    memset(h, 0, WMM_SHM_SIZE);
    h->magic   = WMM_SHM_MAGIC;
    h->version = WMM_SHM_VERSION;

    shm_publish_to(h);

    g_shm = h;
    pthread_mutex_unlock(&g_shm_create_lock);
    return 0;
}

/* fork 后子进程:丢弃映射并重建创建锁;父进程的映射不受影响 */
void wmm_shm_child_reset(void) {
    if (g_shm != NULL) {
        (void)munmap(g_shm, WMM_SHM_SIZE);
        g_shm = NULL;
    }
    (void)pthread_mutex_init(&g_shm_create_lock, NULL);
}
