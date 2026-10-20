/* wmm_shm.c —— 共享内存监控协议
 *
 * 职责:把统计快照、TOP N 采样表、事件环放进一个内存映射文件,
 * 后台线程每秒用 seqlock 发布一次;外部监控进程只读 mmap 同一
 * 文件即可实时读数——零侵入主逻辑、无 IPC 开销。
 * seqlock:写前把序号置奇、写完 +1 回偶;读者序号首尾一致且为偶
 * 才算读到了完整快照,天然免锁。
 *
 * 单写者契约:一个共享区同时只有一个线程在写——正常运行时是后台
 * 线程(每秒一次,与回收同周期),创建时由调用线程先同步发布一代
 * (让监控在 create 返回后立刻有数,不必等后台线程睡满一个周期)。
 * 创建期的那次发布发生在指针公开之前,与后台线程不可能并行;创建
 * 动作本身由 g_shm_create_lock 串行。多写者会让序号交错,读者可能
 * 把两代数据拼成一份"从未存在过"的快照。
 *
 * 发布路径的分段:快照要逐 bin 持锁合并(几十把锁),事件环拷贝要
 * 持事件锁——这些全部在 seqlock 写段之外做完,写段里只有 memcpy
 * 和序号操作。写段越短,读者撞上奇数序号的概率越低、重试越少。
 *
 * 布局与读端在 wmm_shm.h(线上格式),这里只管写。
 */
#include "wmm_internal.h"
#include "wmm_shm.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

/* 唯一发布目标;NULL = 未创建。改动只发生在 g_shm_create_lock 内
 * (创建)或 fork 子进程的复位里;发布路径不加锁地读它——读到 NULL
 * 就什么都不做,读到指针就是初始化完毕的映射。顺序由创建路径的
 * "先填好、后公开"保证。 */
static wmm_shm_t *g_shm;

/* 只串行创建动作本身(两个线程同时 create 同一路径会互相截断对方
 * 的文件)。发布不走这把锁:单写者契约已经把并发发布排除在外。 */
static pthread_mutex_t g_shm_create_lock = PTHREAD_MUTEX_INITIALIZER;

/* 事件环的发布中转区:环内容必须在 g_event_lock 内拷贝,而 seqlock
 * 写段里不能再取任何锁——否则写段要等别人放锁,读者一直撞奇数
 * 序号。先在锁内拷进本缓冲,写段只做 memcpy。48KB 静态内存换写段
 * 最短,对 64KB 的共享区本身也是同量级开销。 */
static wmm_event_t g_ring_scratch[WMM_SHM_EVENT_RING];

/* 事件环按整环 memcpy 镜像,两侧容量不等就会读越界或漏发 */
_Static_assert(WMM_EVENT_RING == WMM_SHM_EVENT_RING,
               "事件环镜像按整环 memcpy,两侧容量必须相等");

/* TOP 表按整表 memcpy 镜像:内部取数上限与共享区容量必须一致,
   否则多出的项被静默截断或写越界 */
_Static_assert(WMM_PROF_TOP_MAX == WMM_SHM_PROF_N,
               "TOP 表容量与共享区不一致");

/* TOP 表的发布中转区:取 TOP 要合并各线程缓冲并持聚合锁,必须在
   seqlock 写段之外完成;写段里只做 memcpy。8KB 静态内存换写段最短 */
static wmm_prof_entry_t g_prof_scratch[WMM_SHM_PROF_N];

/* 发布一代数据到已映射的共享区。调用者须保证自己是唯一写者。 */
static void shm_publish_to(wmm_shm_t *h) {
    /* 1) 备数据:全在 seqlock 之外完成 */
    wmm_stats_t st;
    (void)wmm_stats_snapshot(&st);   /* 逐 bin 持锁合并 */

    uint64_t ev_total = 0;
    wmm_event_copy(g_ring_scratch, &ev_total);   /* 持事件锁整环拷贝 */

    /* 调用点 TOP 表:内部先合并各线程缓冲(持锁),同样在写段之外。
       返回条数不足 256 时,其余项已被取数函数清零——否则会发布
       上一代的残项,读端把已经跌出 TOP 的调用点当成当前数据 */
    (void)wmm_prof_top(g_prof_scratch, WMM_SHM_PROF_N);

    uint64_t ts = wmm_mono_ns();

    /* 2) 写段:只有 memcpy 与序号操作。
       栅栏方向必须对准 C11 的语义:release 栅栏只约束"它之前的写",
       acquire 栅栏才约束"它之后的读写"。所以开头用 acq_rel——它的
       acquire 面挡住数据写被提到序号置奇之前(否则读者可能拿着旧
       偶数序号读走半新的数据);收尾用 release 栅栏挡住数据写落到
       置偶之后。x86-64 上这两个栅栏都不生成指令,只约束编译器 */
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

int wmm_shm_create(const char *path) {
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g_shm_create_lock);
    if (g_shm != NULL) {
        /* 一个进程只发布一份。要换路径得先重启进程:没有 destroy
           命令,撤掉写权意味着监控可能正读着半截数据,而"换个文件"
           本身用重跑进程更省事 */
        pthread_mutex_unlock(&g_shm_create_lock);
        errno = EEXIST;
        return -1;
    }

    /* 具名文件而非匿名映射/内存文件:监控进程在不知道目标 pid 的
       前提下按约定路径找到它;发布者退出后文件仍在,残留的最后一代
       数据对事后分析有用。0600——监控数据含堆地址,不对其他用户开 */
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        pthread_mutex_unlock(&g_shm_create_lock);
        return -1;                    /* errno 由 open 给出 */
    }

    /* 失败路径都撤掉文件:0 字节或半截的文件对读端是陷阱(映射
       0 字节文件一访问就是 SIGBUS),而 O_TRUNC 已经把原内容毁了,
       撤掉不再损失什么 */
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
    (void)close(fd);   /* 映射独立于 fd;close 失败无补救手段,忽略 */

    /* 显式清零:新扩出的文件区域本就读到零,但清零让"prof 表恒零 =
       采样未启用"这类约定不依赖文件历史,也抹掉路径上残留的旧文件 */
    memset(h, 0, WMM_SHM_SIZE);
    h->magic   = WMM_SHM_MAGIC;
    h->version = WMM_SHM_VERSION;

    /* 先发布一代再公开指针:create 返回后监控立刻读到有效数据。
       此调用发生在 g_shm 提交之前,与后台线程不可能并行——单写者
       契约在创建期同样成立 */
    shm_publish_to(h);

    g_shm = h;
    pthread_mutex_unlock(&g_shm_create_lock);
    return 0;
}

void wmm_shm_child_reset(void) {
    /* 子进程里只剩一个线程:加锁会永久阻塞(父进程里持锁的线程
       不存在),直接复位状态。子进程放弃写权——若父子同时发布,
       序号会交错,读者可能把两代数据拼成一份从未存在过的快照。
       映射一并撤掉:子进程要读监控数据,自己 mmap 文件即可(读端
       天然多读者),不必继承父进程的写映射 */
    if (g_shm != NULL) {
        (void)munmap(g_shm, WMM_SHM_SIZE);
        g_shm = NULL;
    }
    (void)pthread_mutex_init(&g_shm_create_lock, NULL);
}
