/* test_shm.c —— 共享内存 seqlock 用例
 *
 * 覆盖:
 *   创建契约:未创建时发布是空操作;NULL 路径 EINVAL、目录不存在
 *        ENOENT、重复创建 EEXIST(且不破坏已有映射);
 *   跨进程:子进程按路径 open + 只读 mmap 同一文件(外部监控进程的
 *        真实路径),读到父进程 fork 之后发布的新一代——证明发布经
 *        MAP_SHARED 对另一个进程可见,且两进程解析结果逐字节一致;
 *   撕裂压力:一个线程猛分配/释放并持续写事件、一个线程不停发布、
 *        主线程不停读,每一次成功的读都验单代恒等式(见下);
 *   镜像一致:发布一代后,映射里的快照/事件环/TOP 采样表与直接读
 *        内部状态逐字节相同;TOP 表经 wmm_shm_read_prof 按监控端
 *        路径读回,证明采样数据确实随发布代跨进程可见。
 *
 * 读端每代能验什么:快照是"锁内读数拼起来的一代",所以
 *   Σ cls[i].live == live_objects(同一批读数求和,逐字节精确),
 *   小对象字节 ≤ live_bytes(差值即大对象字节),
 *   frag == active − live(active < live 时快照钳成 0)。
 * 这些恒等式单代成立、跨代不成立,因此任何一次"把两代拼起来"的
 * 撕裂都会把它们打破——这正是 seqlock 要挡的东西。epoch 与
 * events_total 只增不减同理。
 *
 * 事件环的顺序检查:环按写入序覆盖最旧,最新 k 条的数组下标是
 * (events_total − k .. events_total − 1) 取模,按这个逻辑序走一遍
 * 时间戳应当不减。时间戳在取锁前采样,多生产者时相邻两条可能逆序;
 * 本组的事件生产者只有 churn 线程,理论上严格不减,仍留每次检查
 * 1 次逆序的余量,免得测试比契约更严。
 *
 * 路径带 pid:并行跑多份测试互不干扰,残留文件也认得出是谁的。
 */
#define _GNU_SOURCE        /* getpid(2) */

#include "wmm_internal.h"
#include "wmm_shm.h"

#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* 事件环镜像按整环 memcpy,两侧容量不等就会读越界或漏发 */
_Static_assert(WMM_EVENT_RING == WMM_SHM_EVENT_RING, "ring size mismatch");

static int  g_fail;
static char g_path[128];

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("  FAIL @%d: %s\n", __LINE__, #cond);               \
            g_fail++;                                                  \
        }                                                              \
    } while (0)

/* 完整读满 n 字节(管道允许短读,只读一次不够) */
static int xread(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* 以监控进程的方式打开共享区:按路径 open + 只读 mmap。读端拿到的是
 * 文件本身,不是库内部那个可写映射——测的才是"照协议能读到"。只读
 * 映射还有一层含义:读端永远写不进去,监控不可能破坏被监控者。 */
static const wmm_shm_t *monitor_open(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    void *p = mmap(NULL, WMM_SHM_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    (void)close(fd);
    return p == MAP_FAILED ? NULL : (const wmm_shm_t *)p;
}

static void monitor_close(const wmm_shm_t *h) {
    (void)munmap((void *)(uintptr_t)h, WMM_SHM_SIZE);
}

/* ---- 创建契约 ---- */
static void check_create_args(void) {
    /* 还没创建:发布必须是空操作,不能崩也不能有副作用 */
    wmm_shm_publish();

    CHECK(wmm_shm_create(NULL) == -1);
    CHECK(errno == EINVAL);

    CHECK(wmm_shm_create("/nonexistent_dir_wmm/x.shm") == -1);
    CHECK(errno == ENOENT);

    CHECK(wmm_shm_create(g_path) == 0);

    /* 一个进程只写一份:第二次被挡下,且不动已建好的映射 */
    CHECK(wmm_shm_create(g_path) == -1);
    CHECK(errno == EEXIST);

    const wmm_shm_t *h = monitor_open(g_path);
    CHECK(h != NULL);
    if (h != NULL) {
        CHECK(h->magic == WMM_SHM_MAGIC);
        CHECK(h->version == WMM_SHM_VERSION);
        wmm_stats_t st;
        uint64_t ts = 0;
        CHECK(wmm_shm_read(h, &st, NULL, &ts) == 0);
        CHECK(st.epoch != 0);          /* 创建时已同步发布过一代 */
        CHECK(ts != 0);                /* 发布时间戳已填 */
        CHECK(wmm_mono_ns() >= ts);    /* 同源时钟,可判新鲜度 */
        monitor_close(h);
    }
}

/* ---- 跨进程:子进程按路径读到父进程新发布的代 ---- */
static void check_cross_process(void) {
    const wmm_shm_t *h = monitor_open(g_path);
    CHECK(h != NULL);
    if (h == NULL)
        return;

    wmm_stats_t ref0;
    uint64_t    total0 = 0;
    CHECK(wmm_shm_read(h, &ref0, &total0, NULL) == 0);

    /* 子进程若在读到同步字节前就死了,父进程的 write 会收到 SIGPIPE;
       默认处置是杀进程,测试会在没有任何诊断的情况下消失。忽略它,
       让失败以 EPIPE/短读的形式走到断言里 */
    (void)signal(SIGPIPE, SIG_IGN);

    int p2c[2], c2p[2];
    if (pipe(p2c) != 0 || pipe(c2p) != 0) {
        CHECK(0);                          /* 管道都建不起来:环境问题 */
        monitor_close(h);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        CHECK(pid >= 0);
        (void)close(p2c[0]); (void)close(p2c[1]);
        (void)close(c2p[0]); (void)close(c2p[1]);
        monitor_close(h);
        return;
    }
    if (pid == 0) {
        /* 子进程 = 外部监控进程:不 boot 分配器、不碰父进程状态,
           只按约定路径打开文件读数据。子进程只许 _exit:不跑 stdio
           刷新与 sanitizer 的退出钩子 */
        (void)close(p2c[1]);
        (void)close(c2p[0]);

        int fd = open(g_path, O_RDONLY);
        if (fd < 0)
            _exit(2);
        const wmm_shm_t *ch = mmap(NULL, WMM_SHM_SIZE, PROT_READ,
                                   MAP_SHARED, fd, 0);
        (void)close(fd);
        if (ch == MAP_FAILED)
            _exit(3);
        if (ch->magic != WMM_SHM_MAGIC || ch->version != WMM_SHM_VERSION)
            _exit(4);

        /* 等父进程再发布一代后才读:证明发布经 MAP_SHARED 跨进程可见,
           读到的不是 fork 那一刻的旧数据 */
        char b;
        if (xread(p2c[0], &b, 1) != 0)
            _exit(6);

        struct {
            wmm_stats_t st;
            uint64_t    total;
            uint64_t    ts;
        } msg;
        if (wmm_shm_read(ch, &msg.st, &msg.total, &msg.ts) != 0)
            _exit(5);
        if (write(c2p[1], &msg, sizeof msg) != (ssize_t)sizeof msg)
            _exit(7);
        _exit(0);
    }

    (void)close(p2c[0]);
    (void)close(c2p[1]);

    wmm_shm_publish();                     /* 新一代 */
    CHECK(write(p2c[1], "g", 1) == 1);

    struct {
        wmm_stats_t st;
        uint64_t    total;
        uint64_t    ts;
    } msg;
    memset(&msg, 0, sizeof msg);
    CHECK(xread(c2p[0], &msg, sizeof msg) == 0);

    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        printf("  (child exit code %d)\n",
               WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        monitor_close(h);
        return;
    }

    wmm_stats_t ref1;
    uint64_t    total1 = 0, ts1 = 0;
    CHECK(wmm_shm_read(h, &ref1, &total1, &ts1) == 0);

    CHECK(ref1.epoch == ref0.epoch + 1);   /* 期间只发布了一次 */
    CHECK(msg.st.epoch == ref1.epoch);     /* 子进程看到的是新代 */
    CHECK(msg.total == total1);
    CHECK(msg.ts == ts1);

    /* 逐字节一致:两个进程对同一代的解析结果必须相同(epoch 是各进程
       各自递增的字段,归零后比对) */
    msg.st.epoch = 0;
    ref1.epoch   = 0;
    CHECK(memcmp(&msg.st, &ref1, sizeof ref1) == 0);

    monitor_close(h);
}

/* ---- 并发压力:写者刷分配 + 发布者刷快照 + 读者刷校验 ---- */
#define STRESS_OPS          150000
#define STRESS_EVENT_EVERY  64     /* 约 2300 条事件:环至少绕一圈 */

typedef struct {
    _Atomic int done;              /* churn 线程跑完置 1 */
} stress_ctx_t;

static void *churn_thread(void *arg) {
    stress_ctx_t *c = arg;
    void         *ring[64];
    memset(ring, 0, sizeof ring);

    uint64_t rng = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < STRESS_OPS; i++) {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        size_t sz = (size_t)((rng >> 33) % 20000) + 1;   /* 跨小/大对象 */
        void  *p  = wmm_alloc(sz);
        if (p == NULL)
            break;                 /* OOM 就提前收工,不该发生 */
        unsigned slot = (unsigned)((rng >> 17) & 63u);
        if (ring[slot] != NULL)
            wmm_free(ring[slot]);
        ring[slot] = p;

        if (i % STRESS_EVENT_EVERY == 0)
            wmm_event_log(0xDA, p);   /* 合成事件:灌环并制造事件流 */
    }
    for (unsigned i = 0; i < 64; i++)
        if (ring[i] != NULL)
            wmm_free(ring[i]);

    atomic_store_explicit(&c->done, 1, memory_order_release);
    return NULL;
}

static void *publisher_thread(void *arg) {
    stress_ctx_t *c = arg;
    /* 不停发布:把写段占空比拉到极限,读者撞上奇数序号的概率最大化 */
    while (!atomic_load_explicit(&c->done, memory_order_acquire))
        wmm_shm_publish();
    return NULL;
}

static void check_stress(void) {
    const wmm_shm_t *h = monitor_open(g_path);
    CHECK(h != NULL);
    if (h == NULL)
        return;

    stress_ctx_t ctx;
    atomic_init(&ctx.done, 0);

    pthread_t tc, tp;
    int rc1 = pthread_create(&tc, NULL, churn_thread, &ctx);
    int rc2 = pthread_create(&tp, NULL, publisher_thread, &ctx);
    CHECK(rc1 == 0);
    CHECK(rc2 == 0);
    if (rc1 != 0 || rc2 != 0) {
        if (rc1 == 0) {                    /* 起得来的照常收干净 */
            atomic_store_explicit(&ctx.done, 1, memory_order_release);
            (void)pthread_join(tc, NULL);
        }
        monitor_close(h);
        return;
    }

    static wmm_event_t evbuf[WMM_SHM_EVENT_RING];

    uint64_t reads = 0, eagain = 0, checks = 0, descents = 0;
    uint64_t gens = 0, last_epoch = 0, last_total = 0;

    while (!atomic_load_explicit(&ctx.done, memory_order_acquire) ||
           reads < 16) {
        wmm_stats_t st;
        uint64_t    total = 0;

        if (wmm_shm_read(h, &st, &total, NULL) != 0) {
            /* 重试耗尽只可能是写者长期占着写段:合法结果,计数即可 */
            CHECK(errno == EAGAIN);
            eagain++;
            continue;
        }
        reads++;

        /* 单代恒等式(逐字节精确,不是估计值) */
        uint64_t objs = 0, small = 0;
        for (int i = 0; i <= WMM_NBINS; i++)
            objs += st.cls[i].live;
        for (int i = 0; i < WMM_NBINS; i++)
            small += st.cls[i].live * st.cls[i].size;
        CHECK(objs == st.live_objects);
        CHECK(small <= st.live_bytes);
        CHECK(st.frag_bytes == (st.active_bytes >= st.live_bytes
                                  ? st.active_bytes - st.live_bytes : 0));

        /* 只增不减:倒退即读到旧代,是撕裂 */
        CHECK(st.epoch >= last_epoch);
        CHECK(total >= last_total);
        if (st.epoch != last_epoch)
            gens++;
        last_epoch = st.epoch;
        last_total = total;

        if (reads % 256 == 0 && wmm_shm_read_events(h, evbuf, &total) == 0) {
            uint64_t n = total < WMM_SHM_EVENT_RING
                           ? total : WMM_SHM_EVENT_RING;
            uint64_t prev = 0, desc = 0;
            for (uint64_t j = 0; j < n; j++) {
                const wmm_event_t *e =
                    &evbuf[(total - n + j) % WMM_SHM_EVENT_RING];
                if (j != 0 && e->ts < prev)
                    desc++;
                prev = e->ts;
            }
            descents += desc;
            checks++;
        }
    }

    CHECK(pthread_join(tc, NULL) == 0);
    CHECK(pthread_join(tp, NULL) == 0);

    printf("  stress: %llu reads (%llu EAGAIN), %llu generations, "
           "%llu ring checks, %llu ts descents\n",
           (unsigned long long)reads, (unsigned long long)eagain,
           (unsigned long long)gens, (unsigned long long)checks,
           (unsigned long long)descents);

    /* 压力必须真的压到:读到多代数据、事件环绕圈;每次检查最多容忍
       1 次时间戳逆序(见文件头) */
    CHECK(gens >= 10);
    CHECK(last_total > WMM_SHM_EVENT_RING);
    CHECK(checks > 0);
    CHECK(descents <= checks);

    monitor_close(h);
}

/* ---- 镜像一致:映射内容 == 内部状态 ---- */
static void check_mirror(void) {
    const wmm_shm_t *h = monitor_open(g_path);
    CHECK(h != NULL);
    if (h == NULL)
        return;

    wmm_shm_publish();                     /* 发布一代后不再动任何状态 */

    wmm_stats_t pub, ref;
    uint64_t    total = 0, ts = 0;
    CHECK(wmm_shm_read(h, &pub, &total, &ts) == 0);
    CHECK(wmm_stats_snapshot(&ref) == 0);

    /* 发布用的快照与下一次快照之间只差一个 epoch(其余字段无人动过) */
    CHECK(pub.epoch + 1 == ref.epoch);
    pub.epoch = 0;
    ref.epoch = 0;
    CHECK(memcmp(&pub, &ref, sizeof ref) == 0);

    /* 发布时间戳就是刚才那次发布的时刻 */
    CHECK(ts != 0 && wmm_mono_ns() - ts < 1000000000ull);

    static wmm_event_t ev_pub[WMM_SHM_EVENT_RING];
    static wmm_event_t ev_ref[WMM_SHM_EVENT_RING];
    uint64_t tp = 0, tr = 0;
    CHECK(wmm_shm_read_events(h, ev_pub, &tp) == 0);
    wmm_event_copy(ev_ref, &tr);
    CHECK(tp == tr);
    CHECK(tp > 0);                         /* 压力阶段写过事件 */
    CHECK(memcmp(ev_pub, ev_ref, sizeof ev_ref) == 0);

    monitor_close(h);
}

/* ---- TOP 采样表随发布代镜像 ---- */
static __attribute__((noinline)) void eat_shm(void) {
    /* 5000 次分配 → 5 个样本 × 权重 1000 → 估算 count = 5000。
       每组测试之间的主线程节拍已在上一组末尾归零 */
    for (unsigned i = 0; i < 5000; i++) {
        void *p = wmm_alloc(512);
        if (p == NULL)
            return;
        wmm_free(p);
    }
}

static void check_prof_table(void) {
    CHECK(wmm_ctl("sampling.enable") == 0);
    eat_shm();
    CHECK(wmm_ctl("sampling.disable") == 0);

    static wmm_prof_entry_t top[WMM_SHM_PROF_N];
    int n = wmm_ctl("prof.top", (int)WMM_SHM_PROF_N, top);
    CHECK(n >= 1);
    if (n < 1)
        return;

    /* 本组的吃内存函数只分配 512B,它的行满足 bytes == count × 512;
       上一组的工作线程也用过 512B,但 count 不同,这里只确认新行
       存在,不依赖表的完整内容 */
    int found = 0;
    for (int i = 0; i < n; i++)
        if (top[i].count != 0 && top[i].bytes == top[i].count * 512)
            found = 1;
    CHECK(found);

    /* 发布一代,再按监控进程的路径只读映射读回 TOP 表:内部取数与
       发布内容必须逐字节一致。发布内部会再取一次 TOP,但这期间
       没有任何新分配,聚合表不变 */
    wmm_shm_publish();

    const wmm_shm_t *h = monitor_open(g_path);
    CHECK(h != NULL);
    if (h == NULL)
        return;

    static wmm_prof_entry_t pub[WMM_SHM_PROF_N];
    CHECK(wmm_shm_read_prof(h, pub) == 0);
    CHECK(memcmp(pub, top, sizeof top) == 0);

    monitor_close(h);
}

int test_shm(void) {
    g_fail = 0;
    wmm_boot();

    /* 本组自己当发布者,必须确保没有第二个写者:后台线程若在跑,它
       每秒也会发布一代,epoch 差、镜像对账都会算不准。停掉是幂等的
       (未运行时也是无操作) */
    CHECK(wmm_ctl("bg.disable") == 0);

    snprintf(g_path, sizeof g_path, "/tmp/wmm_shm_test_%d.shm",
             (int)getpid());

    check_create_args();
    check_cross_process();
    check_stress();
    check_mirror();
    check_prof_table();

    CHECK(unlink(g_path) == 0);

    if (g_fail == 0)
        printf("  all shm checks passed\n");
    return g_fail;
}
