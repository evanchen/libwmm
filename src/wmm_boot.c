/* wmm_boot.c —— 启动、fork 处理、诊断输出与元数据分配 */

#include "wmm_internal.h"

#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

#define WMM_HEAP_VA_MIN ((size_t)4 << 30)       /* 窗口下限 */
#define WMM_BASE_BLOCK  ((size_t)4 << 20)       /* 元数据块初值 */

_Static_assert(WMM_HEAP_VA_SIZE >= WMM_HEAP_VA_MIN,
               "WMM_HEAP_VA_SIZE 小于启动下限 4GB,检查 -D 覆盖值");
_Static_assert((WMM_HEAP_VA_SIZE & (WMM_PAGE - 1)) == 0,
               "WMM_HEAP_VA_SIZE 必须是页大小的整数倍");

void *g_heap_base;
void *g_heap_end;
void *g_heap_cur;

wmm_pte_t *g_pte;

wmm_bin_t g_bins[WMM_NBINS];

pthread_mutex_t g_ext_lock;
wmm_large_stats_t g_large;

wmm_extent_t *g_ar_head;
size_t        g_dirty_pages;
size_t        g_active_bytes;

wmm_tcache_t *g_threads;
pthread_mutex_t g_threads_lock;
_Thread_local wmm_tcache_t *g_tcache;

pthread_key_t g_tcache_key = (pthread_key_t)-1;

wmm_opt_t g_opt = {
    .dirty_max    = (size_t)8 << 20,
    .tcache_max_pages    = WMM_TCACHE_LARGE_DEFAULT_PAGES,
    .tcache_budget_pages = WMM_TCACHE_LARGE_DEFAULT_BUDGET_PAGES,
    .madvise_mode = MADV_DONTNEED,
    .sample_n     = 0,
    .guard        = false,
    .verbose      = false,
    .abort_oom    = false,
};

_Atomic unsigned g_fault_site;
_Atomic unsigned g_fault_after;

/* 故障注入:site 匹配时按 after 倒数,命中一次即关闭;ALWAYS 常开 */
bool wmm_fault_check(unsigned site) {
    unsigned want = atomic_load_explicit(&g_fault_site, memory_order_relaxed);
    if (want == 0 || (want != WMM_FAULT_ANY && want != site))
        return false;

    unsigned n = atomic_load_explicit(&g_fault_after, memory_order_relaxed);
    while (n != 0 && n != WMM_FAULT_ALWAYS) {
        if (atomic_compare_exchange_weak_explicit(&g_fault_after, &n, n - 1,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed))
            return n == 1;
    }
    return n == WMM_FAULT_ALWAYS;
}

static bool g_boot_done = false;

static pthread_mutex_t g_base_lock = PTHREAD_MUTEX_INITIALIZER;

/* 从设定值起对半重试到下限;PROT_NONE 只占地址空间,不占内存 */
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

/* 单线程调用一次;窗口、页表、键、atfork 任一失败直接 abort */
void wmm_boot(void) {
    if (g_boot_done)
        return;

    size_t win = 0;
    void  *base = heap_window_reserve(&win);
    if (base == NULL) {
        wmm_dprintf("fatal: cannot reserve heap VA window (>= 4GB), giving up\n");
        abort();
    }
    g_heap_base = base;
    g_heap_cur  = base;
    g_heap_end  = (char *)base + win;

    size_t pte_bytes = win / WMM_PAGE * sizeof(wmm_pte_t);
    g_pte = mmap(NULL, pte_bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_pte == MAP_FAILED) {
        wmm_dprintf("fatal: page table mmap (%zu MB) failed\n", pte_bytes >> 20);
        abort();
    }

    (void)madvise(g_pte, pte_bytes, MADV_NOHUGEPAGE);

    for (size_t i = 0; i < WMM_NBINS; i++)
        (void)pthread_mutex_init(&g_bins[i].lock, NULL);
    (void)pthread_mutex_init(&g_guard_lock, NULL);
    (void)pthread_mutex_init(&g_ext_lock, NULL);
    (void)pthread_mutex_init(&g_threads_lock, NULL);

    if (pthread_key_create(&g_tcache_key, wmm_tcache_thread_destructor) != 0) {
        wmm_dprintf("fatal: pthread_key_create for thread-cache destructor "
                    "failed, thread caches cannot be reclaimed at exit\n");
        abort();
    }

    int rc = pthread_atfork(wmm_atfork_prepare, wmm_atfork_parent,
                            wmm_atfork_child);
    if (rc != 0) {
        wmm_dprintf("fatal: pthread_atfork failed (%d), cannot guarantee "
                    "fork safety\n", rc);
        abort();
    }

    if (g_opt.verbose) {
        size_t max_b = (size_t)atomic_load_explicit(&g_opt.tcache_max_pages,
                                                    memory_order_relaxed)
                       * WMM_PAGE;
        size_t budget_b =
            (size_t)atomic_load_explicit(&g_opt.tcache_budget_pages,
                                         memory_order_relaxed) * WMM_PAGE;

        wmm_dprintf("boot ok: heap window %zu MB at %p, page table %zu MB, "
                    "%u bins, tcache_max %zu B, tcache_budget %zu B\n",
                    win >> 20, base, pte_bytes >> 20, (unsigned)WMM_NBINS,
                    max_b, budget_b);
    }

    g_boot_done = true;
}

/* fork 前按固定顺序取全部锁:bins -> guard -> ext -> threads -> prof -> base */
void wmm_atfork_prepare(void) {
    for (size_t i = 0; i < WMM_NBINS; i++)
        (void)pthread_mutex_lock(&g_bins[i].lock);
    (void)pthread_mutex_lock(&g_guard_lock);
    (void)pthread_mutex_lock(&g_ext_lock);
    (void)pthread_mutex_lock(&g_threads_lock);
    (void)pthread_mutex_lock(&g_prof_lock);
    (void)pthread_mutex_lock(&g_base_lock);
}

/* 父进程按相反顺序解锁 */
void wmm_atfork_parent(void) {
    (void)pthread_mutex_unlock(&g_base_lock);
    (void)pthread_mutex_unlock(&g_prof_lock);
    (void)pthread_mutex_unlock(&g_threads_lock);
    (void)pthread_mutex_unlock(&g_ext_lock);
    (void)pthread_mutex_unlock(&g_guard_lock);
    for (size_t i = WMM_NBINS; i > 0; i--)
        (void)pthread_mutex_unlock(&g_bins[i - 1].lock);
}

/* 子进程重建锁与后台状态,并丢弃其他线程的缓存 */
void wmm_atfork_child(void) {
    for (size_t i = 0; i < WMM_NBINS; i++)
        (void)pthread_mutex_init(&g_bins[i].lock, NULL);
    (void)pthread_mutex_init(&g_guard_lock, NULL);
    (void)pthread_mutex_init(&g_ext_lock, NULL);
    (void)pthread_mutex_init(&g_threads_lock, NULL);
    (void)pthread_mutex_init(&g_prof_lock, NULL);
    (void)pthread_mutex_init(&g_base_lock, NULL);

    wmm_bg_child_reset();

    (void)pthread_mutex_init(&g_event_lock, NULL);

    wmm_shm_child_reset();

    wmm_tcache_child_cleanup();
}

/* 栈上输出缓冲:write(2) 直出,不经过 stdio,也不分配内存 */
typedef struct {
    char  buf[256];
    char *p;
} wmm_dout_t;

static void dflush(wmm_dout_t *o) {
    if (o->p != o->buf) {
        ssize_t n = write(2, o->buf, (size_t)(o->p - o->buf));
        (void)n;
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

static void dputnum(wmm_dout_t *o, unsigned long long v, unsigned base, bool neg) {
    char tmp[24];
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

/* 支持 %s %c %d %u %x %p %zu %zx %% 的最小格式化输出 */
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
        case '\0': dputc(&o, '%'); f--;
            break;
        default:  dputc(&o, '%'); dputc(&o, *f);                       break;
        }
    }
    va_end(ap);
    dflush(&o);
}

/* 元数据块:块头 + bump 区;块只增不还 */
typedef struct wmm_base_block_s wmm_base_block_t;
struct wmm_base_block_s {
    wmm_base_block_t *next;
    size_t            used;
    size_t            cap;
};

#define WMM_BASE_HDR ((sizeof(wmm_base_block_t) + 7u) & ~(size_t)7)

static wmm_base_block_t *g_base_cur;
static size_t            g_base_map_bytes;

/* 8 字节对齐的 bump 分配;返回 NULL 表示映射失败或注入故障 */
void *wmm_base_alloc(size_t size) {
    size = (size + 7u) & ~(size_t)7;
    if (size == 0)
        size = 8;

    pthread_mutex_lock(&g_base_lock);

    if (wmm_fault_check(WMM_FAULT_BASE)) {
        pthread_mutex_unlock(&g_base_lock);
        return NULL;
    }

    wmm_base_block_t *b = g_base_cur;
    if (b == NULL || size > b->cap - b->used) {
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
            return NULL;
        }
        b = (wmm_base_block_t *)m;
        b->next = g_base_cur;
        b->used = 0;
        b->cap  = cap - WMM_BASE_HDR;
        g_base_cur = b;
        g_base_map_bytes += cap;
    }

    char *p = (char *)b + WMM_BASE_HDR + b->used;
    b->used += size;
    pthread_mutex_unlock(&g_base_lock);
    return p;
}

/* 已映射元数据块总字节数;只增,供验证工具观察元数据开销 */
size_t wmm_base_bytes(void) {
    pthread_mutex_lock(&g_base_lock);
    size_t n = g_base_map_bytes;
    pthread_mutex_unlock(&g_base_lock);
    return n;
}
