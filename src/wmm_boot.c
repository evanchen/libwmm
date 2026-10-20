/* wmm_boot.c —— 启动初始化 + 全局状态定义点
 *
 * wmm_boot() 在单线程、任何分配之前一次做完三件事:
 *   1. 预留 VA 堆区窗口:整段 PROT_NONE + MAP_NORESERVE,只占地址
 *      空间不占物理内存;strict overcommit 的机器上预留可能失败,
 *      于是减半重试,实际大小记入 g_heap_base/g_heap_end;
 *   2. 建立页表:一次 mmap 到位的平坦数组。零值必须是内核零页——
 *      绝不 memset(理由写在 wmm_internal.h 的 g_pte 旁);
 *   3. 初始化全局锁,并注册 fork 钩子。
 *
 * 编码纪律(自举):boot 全过程中只允许 mmap/madvise/write 这类系统
 * 调用,绝不能调用本库的分配函数——那时代码路径要碰的一切元数据
 * 都还没建好,递归下去必死。同理,本文件不用 libc 的 printf/malloc。
 *
 * 本文件同时是全库全局状态的定义点:凡 wmm_internal.h 里 extern 声明
 * 的全局变量,定义都集中在这儿,单点核对初始化纪律。文件下半部还有
 * 两个跨模块小工具:wmm_dprintf(裸 write 的受限 printf)与
 * wmm_base_alloc(bump 型元数据分配器,仿 jemalloc base.c)。
 */

#include "wmm_internal.h"

#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

/* ==================== 启动期常量 ==================== */

/* 窗口减半重试的下限。语义:连 4GB 都预留不出,说明进程的虚拟地址
   空间已近乎耗尽,再减下去得到的堆也撑不起正经服务——不如即刻报错,
   让问题在启动现场就可见。 */
#define WMM_HEAP_VA_MIN ((size_t)4 << 30)

/* 元数据 bump 区每次追加的块大小。4MB 可装十万级 slab 头(每个几十
   字节);块按需追加、永不回收,理由见 wmm_base_alloc。 */
#define WMM_BASE_BLOCK  ((size_t)4 << 20)

_Static_assert(WMM_HEAP_VA_SIZE >= WMM_HEAP_VA_MIN,
               "WMM_HEAP_VA_SIZE 小于启动下限 4GB,检查 -D 覆盖值");
_Static_assert((WMM_HEAP_VA_SIZE & (WMM_PAGE - 1)) == 0,
               "WMM_HEAP_VA_SIZE 必须是页大小的整数倍");

/* ==================== 全局状态定义 ==================== */

void *g_heap_base;   /* VA 窗口基址(实际值:减半重试后可能小于期望) */
void *g_heap_end;    /* 窗口末尾;越界判据 */
void *g_heap_cur;    /* bump 指针:物理层从这里向前申请新映射 */

wmm_pte_t *g_pte;    /* 平坦页表;raw==0 = 该页未分配(零值由内核零页保证) */

wmm_bin_t g_bins[WMM_NBINS];   /* 单 arena 的 36 个 bin;计数零值由静态存储
                                  保证,锁在 boot 内显式 init */

pthread_mutex_t g_ext_lock;    /* 物理层粗粒度锁(extent 链 + 大对象统计) */
wmm_large_stats_t g_large;     /* 大对象统计,与 g_ext_lock 同域 */

wmm_extent_t *g_ar_head;       /* 物理层地址序链头;全部页面记账的根 */
size_t        g_dirty_pages;   /* FREE_DIRTY 总页数;purge 归还决策的依据 */
size_t        g_active_bytes;  /* 已投入服务的窗口字节(快照 active_bytes 的源) */

wmm_tcache_t *g_threads;           /* 线程注册表链头;NULL = 还没有线程建过缓存 */
pthread_mutex_t g_threads_lock;    /* 保护注册表链的增删 */
_Thread_local wmm_tcache_t *g_tcache;  /* 本线程缓存指针;每线程零初始化 */

/* 线程退出析构键。静态哨兵 (pthread_key_t)-1 = 尚未注册;boot 注册失败
   即终止,所以分配路径上再看到 -1,只可能是没先 boot 或内存损坏,同样
   按致命处理。正常路径由 pthread 在线程退出时回调
   wmm_tcache_thread_destructor。定义放在这里而不是 wmm_tcache.c,
   与其余全局状态同点核对初始化纪律。 */
pthread_key_t g_tcache_key = (pthread_key_t)-1;

/* 运行期选项:静态初始化即默认值,在任何 wmm_ctl("config.*") 之前就位。
   特意不做成"boot 时赋值":测试可以在 wmm_boot 之前改这里(如把
   verbose 置真)来观察 boot 自身的日志。 */
wmm_opt_t g_opt = {
    .dirty_max    = (size_t)8 << 20,   /* 脏内存回收阈值 8MB */
    .madvise_mode = MADV_DONTNEED,     /* 默认立即还物理页,RSS 优先 */
    .sample_n     = 0,                 /* 采样默认关;开启时由 wmm_ctl 设 1000 */
    .guard        = false,             /* 内存检测默认关(有内存放大开销) */
    .verbose      = false,
    .abort_oom    = false,             /* OOM 返回 NULL,与 glibc 行为一致 */
};

static bool g_boot_done = false;   /* 幂等标志;契约保证只有单线程碰它 */

/* base 区自身的锁(见文件尾 wmm_base_alloc)。叶子锁:持它期间
 * 绝不拿其他锁;全库锁序里它排在最后(见 fork 三钩子注释)。 */
static pthread_mutex_t g_base_lock = PTHREAD_MUTEX_INITIALIZER;

/* 预留 VA 窗口:整段 PROT_NONE + MAP_NORESERVE,只占地址空间。
   返回窗口基址;全失败返回 NULL(此时 *size_out 小于下限,仅供日志)。
   为什么要重试:strict overcommit(vm.overcommit_memory=2)会把
   PROT_NONE 预留也计入 commit 配额,窗口一大就失败;减半往往就过。 */
static void *heap_window_reserve(size_t *size_out) {
    size_t size = WMM_HEAP_VA_SIZE;
    void  *base = MAP_FAILED;

    while (size >= WMM_HEAP_VA_MIN) {
        base = mmap(NULL, size, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (base != MAP_FAILED)
            break;
        wmm_dprintf("heap window: %zu MB reservation failed, retry with %zu MB\n",
                    size >> 20, (size >> 1) >> 20);
        size >>= 1;
    }
    *size_out = size;
    return base != MAP_FAILED ? base : NULL;
}

void wmm_boot(void) {
    if (g_boot_done)
        return;   /* 幂等:重复调用是无效果操作(契约:单线程调用) */

    /* ---- 1) VA 窗口 ---- */
    size_t win = 0;
    void  *base = heap_window_reserve(&win);
    if (base == NULL) {
        wmm_dprintf("fatal: cannot reserve heap VA window (>= 4GB), giving up\n");
        abort();   /* wmm_boot 返回 void,初始化失败只能就地终止 */
    }
    g_heap_base = base;
    g_heap_cur  = base;
    g_heap_end  = (char *)base + win;

    /* ---- 2) 页表(零值来自内核,严禁 memset,理由见 g_pte 注释) ---- */
    size_t pte_bytes = win / WMM_PAGE * sizeof(wmm_pte_t);
    g_pte = mmap(NULL, pte_bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_pte == MAP_FAILED) {
        wmm_dprintf("fatal: page table mmap (%zu MB) failed\n", pte_bytes >> 20);
        abort();
    }
    /* THP=always 的机器上,khugepaged 可能把零星触碰的页表页撑成 2MB
       大页,把驻留粒度放大 512 倍;明确禁止。失败无碍,只是少个优化。 */
    (void)madvise(g_pte, pte_bytes, MADV_NOHUGEPAGE);

    /* ---- 3) 全局锁 ---- */
    /* 不赌"静态零值即合法 pthread_mutex_t"(那是 glibc 的实现细节,
       不是可移植保证);显式初始化,也让 fork 后的重建(child 钩子)
       复用同一段代码。 */
    for (size_t i = 0; i < WMM_NBINS; i++)
        (void)pthread_mutex_init(&g_bins[i].lock, NULL);
    (void)pthread_mutex_init(&g_guard_lock, NULL);
    (void)pthread_mutex_init(&g_ext_lock, NULL);
    (void)pthread_mutex_init(&g_threads_lock, NULL);

    /* 线程退出析构键:缓存块按线程 mmap,这个键是"线程退出时把缓存还
       回 arena"的唯一自动通道。没有它,线程池里每轮增删线程都会把整块
       缓存连同其中对象永久滞留在 live 账上——线程已不存在,应用也无法
       替它调 wmm_thread_exit,分配器的资源生命周期失去闭环。这种失败
       没有可接受的降级形态:与窗口/页表失败同一处置,就地终止,不带
       着已破的不变量继续跑。键值在 tcache_create 里写入(块指针只作
       "建过缓存"的非 NULL 标记),回调以 g_tcache 为准——显式 exit 之后
       它为 NULL,自动回调与显式回收并存而不重复清理 */
    if (pthread_key_create(&g_tcache_key, wmm_tcache_thread_destructor) != 0) {
        wmm_dprintf("fatal: pthread_key_create for thread-cache destructor "
                    "failed, thread caches cannot be reclaimed at exit\n");
        abort();   /* wmm_boot 返回 void,初始化失败只能就地终止 */
    }

    /* ---- 4) fork 钩子 ---- */
    /* fork 安全也是对外契约的一部分:prepare 持住全部锁、child 重建锁
       并清理幽灵缓存,三者缺一,多线程程序 fork 出的子进程就可能带着
       别的线程持有的锁继续跑——死锁发生在应用代码深处,现场与根因隔
       着整条调用链,正是"带着未知状态跑"。注册失败没有降级形态,与
       上面几步同一处置 */
    int rc = pthread_atfork(wmm_atfork_prepare, wmm_atfork_parent,
                            wmm_atfork_child);
    if (rc != 0) {
        wmm_dprintf("fatal: pthread_atfork failed (%d), cannot guarantee "
                    "fork safety\n", rc);
        abort();
    }

    if (g_opt.verbose)
        wmm_dprintf("boot ok: heap window %zu MB at %p, page table %zu MB, "
                    "%u bins\n", win >> 20, base, pte_bytes >> 20,
                    (unsigned)WMM_NBINS);

    g_boot_done = true;
}

/* ==================== fork 三钩子 ====================
 *
 * fork 只把调用线程复制进子进程:此刻若别的线程持着某把锁,子进程里
 * 这把锁永远等不到释放。三步应对:
 *   prepare:fork 前置,按固定锁序(各 bin 锁 → g_guard_lock →
 *            g_ext_lock → g_threads_lock → g_prof_lock →
 *            g_base_lock)拿住全部锁,即 freeze the world——fork
 *            快照必须处在"没有分配器
 *            操作正在进行"的状态,因此 prepare 会一直阻塞到别的线程
 *            做完并放锁。采样聚合表同样要冻住:后台线程合并到一半
 *            时 fork,子进程会继承一张半更新的表;
 *   parent: 原序反向解锁,原样继续;
 *   child:  不解锁而是把所有锁重新 init——解锁动作依赖锁的内部状态,
 *            对"别人手里的锁"未必可行,重建才是唯一可移植的做法。
 *
 * 其余线程的缓存块:子进程里把死节点刷空(缓存中的对象还回 bin,
 * 否则这些对象永远达不到全空退休、无人归还)并整块 munmap,注册表
 * 只留调用线程——见 wmm_tcache_child_cleanup。死线程仍被应用持有的
 * 指针(不在缓存里的那些)随线程消失,计入存活而无人归还,fork
 * 语义使然,不是本库泄漏。当前线程的 g_tcache 与全部堆内存在子进程
 * 里完好。
 *
 * 锁序约定:凡将来同时需要多把全局锁的新代码,必须沿用 prepare 的
 * 顺序,否则会与 fork 冻结互锁形成死锁。base 锁必须排最后:持 bin
 * 锁的路径(造 slab 切头部)会要 base 锁,若 prepare 先拿 base,
 * 就与"持 bin 等 base"的线程互等。
 */
void wmm_atfork_prepare(void) {
    for (size_t i = 0; i < WMM_NBINS; i++)
        (void)pthread_mutex_lock(&g_bins[i].lock);
    (void)pthread_mutex_lock(&g_guard_lock);
    (void)pthread_mutex_lock(&g_ext_lock);
    (void)pthread_mutex_lock(&g_threads_lock);
    (void)pthread_mutex_lock(&g_prof_lock);
    (void)pthread_mutex_lock(&g_base_lock);
}

void wmm_atfork_parent(void) {
    (void)pthread_mutex_unlock(&g_base_lock);
    (void)pthread_mutex_unlock(&g_prof_lock);
    (void)pthread_mutex_unlock(&g_threads_lock);
    (void)pthread_mutex_unlock(&g_ext_lock);
    (void)pthread_mutex_unlock(&g_guard_lock);
    for (size_t i = WMM_NBINS; i > 0; i--)
        (void)pthread_mutex_unlock(&g_bins[i - 1].lock);
}

void wmm_atfork_child(void) {
    for (size_t i = 0; i < WMM_NBINS; i++)
        (void)pthread_mutex_init(&g_bins[i].lock, NULL);
    (void)pthread_mutex_init(&g_guard_lock, NULL);
    (void)pthread_mutex_init(&g_ext_lock, NULL);
    (void)pthread_mutex_init(&g_threads_lock, NULL);
    (void)pthread_mutex_init(&g_prof_lock, NULL);
    (void)pthread_mutex_init(&g_base_lock, NULL);

    /* fork 只复制调用线程,后台线程在子进程里不存在;继承来的
       控制状态可能是"运行中"或"锁着"。重建为空闲,否则子进程
       调 bg.disable 会去 join 一个不存在的 tid */
    wmm_bg_child_reset();

    /* 事件环锁同理:父进程里若正有线程持着它写事件,fork 出来的
       子进程里这把锁就永远等不到释放——重建为未锁定 */
    (void)pthread_mutex_init(&g_event_lock, NULL);

    /* 共享监控区:子进程撤掉写映射、放弃写权。父子若同时发布,
       seqlock 序号交错,读者会拼出从未存在过的快照 */
    wmm_shm_child_reset();

    /* 锁全部重建之后才能动 bin:死线程的缓存要刷回 bin,这是子进程
       里唯一会碰分配器结构的钩子动作。此刻独占执行 */
    wmm_tcache_child_cleanup();
}

/* ==================== wmm_dprintf:受限 printf,裸 write(2) 输出 ====================
 *
 * 库内禁用标准 printf:它会调 malloc,而日志最需要出现在 OOM 与
 * 初始化未完成这两条路径上——恰恰是 malloc 不可用的时候。这里直写
 * stderr(fd 2),不经过任何缓冲与分配。
 * 支持:%s %c %d %u %x %p %zu %zx %%(其余转换按字面输出)。
 * 输出带 "[wmm] " 前缀;换行由调用方在格式串里给。 */

typedef struct {
    char  buf[256];   /* 攒满一块发一次 write,减少系统调用次数 */
    char *p;          /* 下一个可写位置;恒指向 <= buf + sizeof(buf) */
} wmm_dout_t;

static void dflush(wmm_dout_t *o) {
    if (o->p != o->buf) {
        ssize_t n = write(2, o->buf, (size_t)(o->p - o->buf));
        (void)n;      /* stderr 写失败(如已关闭)没有可做的补救,丢弃 */
        o->p = o->buf;
    }
}

static void dputc(wmm_dout_t *o, char c) {
    if (o->p == o->buf + sizeof(o->buf))
        dflush(o);
    *o->p++ = c;
}

static void dputs(wmm_dout_t *o, const char *s) {
    if (s == NULL)
        s = "(null)";
    for (; *s != '\0'; s++)
        dputc(o, *s);
}

/* 数字反转输出:先低位入栈再倒出,避免递归或临时分配。 */
static void dputnum(wmm_dout_t *o, unsigned long long v, unsigned base, bool neg) {
    char tmp[24];   /* 2^64 十进制 20 位 + 符号 + 余量 */
    int  n = 0;

    if (v == 0)
        tmp[n++] = '0';
    while (v != 0) {
        unsigned d = (unsigned)(v % base);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
        v /= base;
    }
    if (neg)
        dputc(o, '-');
    while (n > 0)
        dputc(o, tmp[--n]);
}

void wmm_dprintf(const char *fmt, ...) {
    wmm_dout_t o;
    o.p = o.buf;

    dputs(&o, "[wmm] ");
    va_list ap;
    va_start(ap, fmt);
    for (const char *f = fmt; *f != '\0'; f++) {
        if (*f != '%') {
            dputc(&o, *f);
            continue;
        }
        f++;
        switch (*f) {
        case 's': dputs(&o, va_arg(ap, const char *));                 break;
        case 'c': dputc(&o, (char)va_arg(ap, int));                    break;
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0)
                dputnum(&o, (unsigned long long)(-(long long)v), 10, true);
            else
                dputnum(&o, (unsigned long long)v, 10, false);
            break;
        }
        case 'u': dputnum(&o, va_arg(ap, unsigned), 10, false);        break;
        case 'x': dputnum(&o, va_arg(ap, unsigned), 16, false);        break;
        case 'p':
            dputs(&o, "0x");
            dputnum(&o, (unsigned long long)(uintptr_t)va_arg(ap, void *), 16, false);
            break;
        case 'z':
            f++;
            if (*f == 'u')
                dputnum(&o, va_arg(ap, size_t), 10, false);
            else if (*f == 'x')
                dputnum(&o, va_arg(ap, size_t), 16, false);
            else {
                dputc(&o, '%');
                dputc(&o, 'z');
                if (*f != '\0')
                    dputc(&o, *f);
            }
            break;
        case '%': dputc(&o, '%');                                      break;
        case '\0': dputc(&o, '%'); f--;   /* 尾部孤立 %:原样输出,别越界 */
            break;
        default:  dputc(&o, '%'); dputc(&o, *f);                       break;
        }
    }
    va_end(ap);
    dflush(&o);
}

/* ==================== wmm_base_alloc:元数据 bump 分配器 ====================
 *
 * slab 头、extent 结点这类长命元数据不能向分配器自己要(自举死锁),
 * 逐个 mmap 又浪费内核内存。做法(仿 jemalloc base.c):按 4MB 追加块,
 * 块内 8 字节对齐地线性切割;块用完追加新块,旧块永不回收——元数据
 * 与堆同寿,根本不存在"回收元数据"的问题。随对象生灭反复申请的东西
 * (extent 结点)不适用这里,走显式空闲链复用(见 wmm_phys.c)。
 *
 * 块布局:[ 块头 | 8 字节对齐的分配区... ];块头记录切割进度,并把块
 * 串成单链(调试遍历用,回收从不存在故为单向)。 */

typedef struct wmm_base_block_s wmm_base_block_t;
struct wmm_base_block_s {
    wmm_base_block_t *next;   /* 全部已建块的单链(供观测/调试遍历) */
    size_t            used;   /* 分配区已用字节 */
    size_t            cap;    /* 分配区总容量 */
};

/* 分配区起点:块头之后按 8 字节对齐。 */
#define WMM_BASE_HDR ((sizeof(wmm_base_block_t) + 7u) & ~(size_t)7)

static wmm_base_block_t *g_base_cur;   /* 正在切割的块;NULL = 还没建过 */

/* 为什么要有锁:bin 锁只各管各的 bin,两个线程在不同 bin 里同时造
 * slab 都会来 base 切头部,bump 指针无锁推进 = 两次分配拿到同一块。
 * 叶子锁纪律:持锁期间绝不调用任何可能拿锁的代码(mmap/wmm_dprintf
 * 都无锁),因此它可以安全嵌在任何持锁路径里(bin → base)。
 * 静态 PTHREAD_MUTEX_INITIALIZER 是 POSIX 明确规定的常量初始化,
 * 与"全零字节不是合法互斥量"的可移植性顾虑是两回事。 */
void *wmm_base_alloc(size_t size) {
    size = (size + 7u) & ~(size_t)7;   /* 8 字节对齐(块头里只有 ≤8 字节成员) */
    if (size == 0)
        size = 8;                      /* 调用方不该要 0;给最小块而非 NULL */

    pthread_mutex_lock(&g_base_lock);
    wmm_base_block_t *b = g_base_cur;
    if (b == NULL || size > b->cap - b->used) {
        /* 追加新块:容量取 max(默认 4MB, 本次需求)——单个大请求不会被
           拒绝,只是独占一个大块;旧块的零头(不足一次请求)留在原地。 */
        size_t need = size + WMM_BASE_HDR;
        size_t cap  = WMM_BASE_BLOCK;
        if (need > cap)
            cap = (need + WMM_PAGE - 1) & ~(WMM_PAGE - 1);

        void *m = mmap(NULL, cap, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) {
            pthread_mutex_unlock(&g_base_lock);
            wmm_dprintf("fatal: metadata block mmap (%zu KB) failed, errno %d\n",
                        cap >> 10, errno);
            return NULL;   /* 元数据耗尽 = 分配器不可用,调用方按致命处理 */
        }
        b = (wmm_base_block_t *)m;
        b->next = g_base_cur;
        b->used = 0;
        b->cap  = cap - WMM_BASE_HDR;
        g_base_cur = b;
    }

    char *p = (char *)b + WMM_BASE_HDR + b->used;
    b->used += size;
    pthread_mutex_unlock(&g_base_lock);
    return p;
}
