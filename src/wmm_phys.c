/* wmm_phys.c —— 物理层:页从哪来、还到哪去、如何合并
 *
 * 对主路径的交接单位:slab(小对象,pgs 页)与 extent(大对象,
 * ceil(size/页) 页)。两条路径最终都汇到同一套 extent 记账上:
 *   取页 extent_get:first-fit 沿地址序链找第一个够大的空闲块切出
 *         (复用零系统调用);没有才从窗口 bump 处 mmap 新页;
 *   还页 extent_free:页表立即清零(raw=0),extent 转 FREE_DIRTY,
 *         物理页留在映射里(攒够阈值由 purge 真正还内核),然后沿地址
 *         序链向两侧合并——相邻空闲区间永远融成一块,这就是对抗
 *         外部碎片的全部机关。
 *
 * 两个关键设计(理由同时写在 wmm_internal.h 的 extent 段):
 *   页表语义 = "当前归属":释放即清零。不清的话,复用窗口期内对
 *   已释放地址的 double-free 会反查到还在空闲链上的 extent,把它
 *   再 free 一遍,直接毁链;清零后 double-free 反查 raw==0,与
 *   "从未分配"统一为同一条诊断路径。
 *   物理页不立即还:FREE_DIRTY 复用是零系统调用的;立即还(每次
 *   mmap PROT_NONE / madvise)会让"释放→再分配"的循环反复吃缺页,
 *   这正是 jemalloc 攒 dirty 再批量 purge 的动机。脏页总量超过
 *   g_opt.dirty_max 时,由本文件后半部分的 purge 统一 madvise,
 *   状态转 FREE_RETAINED;回收动作可以同步触发(ctl "purge"),
 *   也可以交给后台线程每 1s 巡检一次。
 *
 * 锁:g_ext_lock 包住全部共享结构(地址序链、bump 指针、回收链、
 * dirty 计数、g_large 统计)。锁内会调 base 锁(叶子),锁序
 * bin → ext → base,与 fork prepare 一致。purge 的遍历与状态迁移
 * 用同一把锁;后台线程的控制状态另有 g_bg_lock,但线程总是先放掉
 * 它再拿 g_ext_lock,不存在 ext → bg 的反向路径(见后半部分)。
 *
 * extent 结点壳复用、slab 头不复用:大对象生灭频繁,结点(64B)
 * 逐块新拿会吃穿 base 区,走回收链;slab 头的生命通常与该类流量
 * 同长,退休时页已入空闲,72B 头部留存不是泄漏主项——若统计显示
 * 不然,同款回收链照加。
 */

#include "wmm_internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

static wmm_extent_t *g_ar_tail;        /* 地址序链尾(最高地址)。bump 出的
                                          新页地址恒最大,尾插 O(1),故
                                          缓存尾指针而不再扫链 */
static wmm_extent_t *g_ext_free_nodes; /* 空闲 extent 结点壳的回收链(LIFO) */

/* ==================== 结点壳:回收链 ==================== */

/* 以下内部函数全部假定调用方已持 g_ext_lock(对外接口统一拿锁,
   避免自嵌套死锁——pthread_mutex 非递归)。 */

static wmm_extent_t *ext_node_get(void) {
    wmm_extent_t *e = g_ext_free_nodes;
    if (e != NULL)
        g_ext_free_nodes = e->next;
    else
        e = (wmm_extent_t *)wmm_base_alloc(sizeof(wmm_extent_t));
    /* base 耗尽返回 NULL:与窗口耗尽同为分配器不可用,上抛 */
    return e;
}

static void ext_node_put(wmm_extent_t *e) {
    e->next = g_ext_free_nodes;
    g_ext_free_nodes = e;
}

/* ==================== 地址序双向链 ==================== */

/* 在 pos 之后插入 e(pos==NULL 表示链头)。新 bump 的页尾插,
   分裂出的余段插在原位之后——两种场景都是 O(1),链从不需要
   按地址扫描找插入点。 */
static void ar_insert_after(wmm_extent_t *pos, wmm_extent_t *e) {
    e->ar_prev = pos;
    if (pos != NULL) {
        e->ar_next = pos->ar_next;
        if (pos->ar_next != NULL)
            pos->ar_next->ar_prev = e;
        else
            g_ar_tail = e;
        pos->ar_next = e;
    } else {
        e->ar_next = g_ar_head;
        if (g_ar_head != NULL)
            g_ar_head->ar_prev = e;
        else
            g_ar_tail = e;
        g_ar_head = e;
    }
}

static void ar_unlink(wmm_extent_t *e) {
    if (e->ar_prev != NULL)
        e->ar_prev->ar_next = e->ar_next;
    else
        g_ar_head = e->ar_next;
    if (e->ar_next != NULL)
        e->ar_next->ar_prev = e->ar_prev;
    else
        g_ar_tail = e->ar_prev;
    e->ar_prev = NULL;
    e->ar_next = NULL;
}

/* ==================== 取页/还页核心 ==================== */

/* 窗口内 bump 出 npages 可写页(已持 ext 锁)。
   窗口耗尽 = 撞人为上限,不是机器没内存:dprintf 把两个数都打
   出来供区分,并记 0xDB OOM 事件。abort_oom 为真时快速失败。 */
static void *window_map(size_t npages) {
    size_t bytes = npages * WMM_PAGE;
    if ((char *)g_heap_cur + bytes > (char *)g_heap_end) {
        size_t used = (size_t)((char *)g_heap_cur - (char *)g_heap_base);
        size_t win  = (size_t)((char *)g_heap_end - (char *)g_heap_base);
        wmm_dprintf("OOM: heap window exhausted, %zu MB in use (window %zu MB)\n",
                    used >> 20, win >> 20);
        wmm_event_log(0xDB, NULL);   /* OOM:窗口耗尽(撞人为上限) */
        if (g_opt.abort_oom)
            abort();
        return NULL;
    }

    void *p = mmap(g_heap_cur, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        /* 窗口是自己的 VA,MAP_FIXED 失败基本只剩"内核整体内存
           耗尽"一种解释;同样按 OOM 上抛。 */
        wmm_dprintf("OOM: mmap %zu pages failed, errno %d\n", npages, errno);
        wmm_event_log(0xDB, NULL);   /* OOM:mmap 失败 */
        if (g_opt.abort_oom)
            abort();
        return NULL;
    }
    g_heap_cur = (char *)p + bytes;
    g_active_bytes += bytes;   /* 新页投入服务:计入 active */
    return p;
}

/* 取 npages 页的 extent(已持 ext 锁)。first-fit 沿地址序链扫:
   从最低地址起找第一个够大的空闲块,切出低段交付,余段(若够
   一页)留在链上。没有命中才 bump 新页。返回 INUSE 的 extent。
   偏好低地址的副作用正好是期望行为:低地址先被复用、先腾空,
   高地址的空闲块更容易保持大块连续。

   两趟:先只在 FREE_DIRTY 里找(物理页还在,复用零系统调用),
   没有再接受 FREE_RETAINED(页已被 purge,复用要吃缺页)。没有
   purge 过的进程里第一趟就是全集,行为与单趟完全一致。 */
static wmm_extent_t *extent_get(size_t npages) {
    for (int pass = 0; pass < 2; pass++) {
        for (wmm_extent_t *e = g_ar_head; e != NULL; e = e->ar_next) {
            if (e->state == WMM_EXT_INUSE || e->npages < npages)
                continue;
            if (pass == 0 && e->state != WMM_EXT_FREE_DIRTY)
                continue;

            bool   was_dirty  = (e->state == WMM_EXT_FREE_DIRTY);
            size_t old_npages = e->npages;
            bool   split_done = false;

            if (e->npages > npages) {
                /* 分裂:低段给调用方,高段余段留在原链位。余段继承
                   原状态(同为 FREE;dirty 计数里它从未离开,无须改)。 */
                wmm_extent_t *rem = ext_node_get();
                if (rem != NULL) {
                    rem->addr   = (char *)e->addr + npages * WMM_PAGE;
                    rem->npages = e->npages - npages;
                    rem->state  = e->state;
                    rem->slab   = NULL;
                    rem->next   = NULL;
                    ar_insert_after(e, rem);
                    e->npages = npages;
                    split_done = true;
                }
                /* rem 耗尽(元数据枯竭):不分裂,整块交付——多给的页
                   无主但记账在 e 里,free 时整体归还,只浪费不损坏。 */
            }

            e->state = WMM_EXT_INUSE;
            e->slab  = NULL;
            if (was_dirty) {
                /* 计数只减"离开空闲态"的页:分裂成功 = 交付段 npages,
                   余段仍是 FREE_DIRTY、仍要计数;整块交付(恰合或分裂
                   失败)才减 old_npages。两者在恰合时相等。 */
                g_dirty_pages -= split_done ? npages : old_npages;
            } else {
                /* RETAINED 块被切出:交付段重新投入服务(访问要吃
                   缺页),计入 active;余段仍是 RETAINED,不动。 */
                g_active_bytes += split_done ? npages * WMM_PAGE
                                             : old_npages * WMM_PAGE;
            }
            return e;
        }
    }

    /* 无可复用:窗口 bump 新页,尾插地址序链 */
    void *addr = window_map(npages);
    if (addr == NULL)
        return NULL;

    wmm_extent_t *e = ext_node_get();
    if (e == NULL) {
        wmm_event_log(0xDB, NULL);   /* OOM:extent 元数据耗尽 */
        return NULL;   /* 页已花掉但不追回:留在 bump 指针之后,
                          下次 bump 会越过它;纯浪费不损坏,极端场景 */
    }

    e->addr   = addr;
    e->npages = npages;
    e->state  = WMM_EXT_INUSE;
    e->slab   = NULL;
    e->next   = NULL;
    ar_insert_after(g_ar_tail, e);
    return e;
}

/* 释放 extent:页表清零、转 FREE_DIRTY、向两侧合并(已持 ext 锁)。
   合并只看"地址相接且都空闲",方向两侧各推到头;被吞的结点壳
   进回收链。

   跨状态合并:purge 产出 RETAINED 之后才可能出现,RETAINED 邻居并进
   DIRTY 块时,整块必须按 DIRTY 记账——那些页的内容不再承诺已
   丢失,页数补回 dirty 计数,下一轮 purge 连同它们一起 madvise
   (对已经 DONTNEED 过的页重复 madvise 只是页表遍历,无 I/O)。
   反过来,存活结点若是 RETAINED 也必须改成 DIRTY:状态标签与
   dirty 计数是一体两面,不同步的话,页数守恒断言会立刻炸,
   而漏记的页永远不会再被 purge。 */
static void extent_release(wmm_extent_t *e) {
    /* 页表清零:raw=0 即"当前无归属"。物理页留在映射里不动——
       FREE_DIRTY 复用零系统调用,真正还物理页是 purge 的事。 */
    wmm_pte_t zero;
    zero.raw = 0;
    for (size_t pg = 0; pg < e->npages; pg++)
        wmm_pte_set((char *)e->addr + pg * WMM_PAGE, zero);

    e->state = WMM_EXT_FREE_DIRTY;
    e->slab  = NULL;
    g_dirty_pages += e->npages;

    /* 向低地址吞并。存活结点是 prev,状态归一到 DIRTY;RETAINED
       的页并入后重新计入 dirty,同时重新投入服务(active)——否则
       整块转 DIRTY 后,将来 purge 会把这块的页数从 active 里整块
       扣掉,而其中 RETAINED 的部分从未加进过,账就对不上了。 */
    wmm_extent_t *prev = e->ar_prev;
    while (prev != NULL && prev->state != WMM_EXT_INUSE &&
           (char *)prev->addr + prev->npages * WMM_PAGE == (char *)e->addr) {
        wmm_extent_t *p2 = prev->ar_prev;
        if (prev->state == WMM_EXT_FREE_RETAINED) {
            g_dirty_pages += prev->npages;
            g_active_bytes += prev->npages * WMM_PAGE;
        }
        prev->state = WMM_EXT_FREE_DIRTY;
        prev->npages += e->npages;
        ar_unlink(e);
        ext_node_put(e);
        e = prev;
        prev = p2;
    }

    /* 向高地址吞并。存活结点 e 已是 DIRTY,只需把 RETAINED 邻居
       的页补进 dirty 与 active 计数。 */
    wmm_extent_t *next = e->ar_next;
    while (next != NULL && next->state != WMM_EXT_INUSE &&
           (char *)e->addr + e->npages * WMM_PAGE == (char *)next->addr) {
        wmm_extent_t *n2 = next->ar_next;
        if (next->state == WMM_EXT_FREE_RETAINED) {
            g_dirty_pages += next->npages;
            g_active_bytes += next->npages * WMM_PAGE;
        }
        e->npages += next->npages;
        ar_unlink(next);
        ext_node_put(next);
        next = n2;
    }
}

/* ==================== 对外接口 ==================== */

wmm_slab_t *wmm_phys_alloc_slab(szind_t i) {
    const wmm_bin_info_t *bi = &wmm_bin_info[i];

    wmm_slab_t *sl = (wmm_slab_t *)wmm_base_alloc(sizeof(wmm_slab_t));
    if (sl == NULL) {
        wmm_event_log(0xDB, NULL);   /* OOM:slab 头元数据耗尽 */
        return NULL;
    }

    pthread_mutex_lock(&g_ext_lock);
    wmm_extent_t *e = extent_get(bi->slab_size >> WMM_LG_PAGE);
    pthread_mutex_unlock(&g_ext_lock);
    if (e == NULL)
        return NULL;   /* 头已花掉但不追回:base 只进不出,不构成泄漏 */

    wmm_slab_init(sl, e->addr, i);   /* 注意:init 会把 ext 置 NULL */
    sl->ext = e;                     /* 双向接线:退休时由此找回 extent */
    e->slab = sl;
    return sl;
}

void wmm_phys_free_slab(wmm_slab_t *sl) {
    wmm_extent_t *e = (wmm_extent_t *)sl->ext;
    /* NULL 防御:slab 未走物理层(测试自备页建的),无页可还。
       产品路径不会出现——phys_alloc_slab 必然接线。 */
    if (e == NULL)
        return;

    pthread_mutex_lock(&g_ext_lock);
    extent_release(e);
    pthread_mutex_unlock(&g_ext_lock);
    /* slab 头留存不复用(见文件头);页已在空闲链,可被任何路径复用 */
}

wmm_extent_t *wmm_phys_alloc_extent(size_t size) {
    /* 防回绕挡板:npages 的向上取整在 size 接近 SIZE_MAX 时会溢出
       成小值,把天文数字的请求伪装成小分配。窗口编译期上限以内
       绝不会溢出;挡在上限之外,真把窗口用满的走正常 OOM 路径。 */
    if (size > WMM_HEAP_VA_SIZE) {
        wmm_dprintf("OOM: request %zu MB exceeds heap window\n", size >> 20);
        wmm_event_log(0xDB, NULL);   /* OOM:请求本身超出窗口上限 */
        if (g_opt.abort_oom)
            abort();
        return NULL;
    }

    size_t npages = (size + WMM_PAGE - 1) >> WMM_LG_PAGE;

    pthread_mutex_lock(&g_ext_lock);
    wmm_extent_t *e = extent_get(npages);
    if (e != NULL) {
        /* 页表登记:tag=1 = extent(最低位置 1;结点来自 base、
           8 字节对齐,高位干净)。extent 每一页都登记同一结点,
           free 从任意页进入都能反查到整体。 */
        wmm_pte_t pte;
        pte.raw = (uintptr_t)e | 1u;
        for (size_t pg = 0; pg < npages; pg++)
            wmm_pte_set((char *)e->addr + pg * WMM_PAGE, pte);

        g_large.live++;
        g_large.live_bytes += npages * WMM_PAGE;
    }
    pthread_mutex_unlock(&g_ext_lock);
    return e;
}

void wmm_phys_free_extent(wmm_extent_t *ext) {
    pthread_mutex_lock(&g_ext_lock);
    g_large.live--;
    g_large.live_bytes -= ext->npages * WMM_PAGE;
    g_large.ndalloc++;
    extent_release(ext);   /* 含页表清零 + 两侧合并 */
    pthread_mutex_unlock(&g_ext_lock);
}

/* ==================== purge:把物理页真正还给内核 ====================
 *
 * FREE_DIRTY 攒着是为了复用零系统调用,代价是 RSS 只涨不落。回收
 * 时机用简单阈值(脏页总字节 > g_opt.dirty_max 才动手),不学
 * jemalloc 的毫秒级 decay:阈值的行为一眼可预期,代码少两个数量级。
 * 代价是回收呈锯齿而不是平滑曲线——对"小而精"的定位,这是划算的。
 *
 * madvise 成功才改状态:失败(内核不认某种 advice 等)时页面内容
 * 仍在,继续按 DIRTY 记账,下一轮再试。宁可多试,也不谎报"已还"。
 *
 * 锁:遍历与状态迁移都在 g_ext_lock 内,madvise 也在锁内。单 arena
 * 下把"移出链→解锁→批量 madvise→再挂回"做对需要暂存数组与重挂
 * 逻辑,复杂度换来的并发收益在千级 extent 的规模上不成立;真要拆,
 * 先拆 arena。后台线程一轮回收是毫秒量级的锁持有,慢路径可以接受。
 */
static size_t purge_dirty_locked(void) {
    size_t purged = 0;

    for (wmm_extent_t *e = g_ar_head; e != NULL; e = e->ar_next) {
        if (e->state != WMM_EXT_FREE_DIRTY)
            continue;
        /* DONTNEED:内容立刻丢弃、RSS 立即回落;FREE:内容保留,
           内核缺内存时才收。两种模式都归 RETAINED,语义是"内核
           可随时回收",不承诺页面内容还在。 */
        if (madvise(e->addr, e->npages * WMM_PAGE, g_opt.madvise_mode) != 0)
            continue;
        e->state = WMM_EXT_FREE_RETAINED;
        g_dirty_pages -= e->npages;
        g_active_bytes -= e->npages * WMM_PAGE;   /* 物理页还给内核 */
        purged += e->npages;
    }
    if (purged != 0)
        wmm_event_log(0xDC, NULL);   /* 执行了一次物理内存回收 */
    return purged;
}

void wmm_phys_purge_if_needed(void) {
    pthread_mutex_lock(&g_ext_lock);
    /* 脏页字节数直接比:窗口上限 64GB,页数 × 页大小仍在 size_t
       范围内,不会回绕 */
    if (g_dirty_pages * WMM_PAGE > g_opt.dirty_max) {
        size_t purged = purge_dirty_locked();
        if (g_opt.verbose && purged != 0)
            wmm_dprintf("purge: %zu MB returned to kernel\n",
                        (purged * WMM_PAGE) >> 20);
    }
    pthread_mutex_unlock(&g_ext_lock);
}

void wmm_phys_purge_now(void) {
    pthread_mutex_lock(&g_ext_lock);
    (void)purge_dirty_locked();
    pthread_mutex_unlock(&g_ext_lock);
}

/* ==================== 后台线程:周期回收 ====================
 *
 * 为什么用线程而不是在 free 里顺手判断阈值:回收代价必须与释放者
 * 的时间线解耦——谁释放谁掏 madvise 的开销,尾延迟就随机落在大块
 * 释放上。固定周期巡检把代价集中到后台,也顺带获得"多线程的脏页
 * 一起批量回收"的机会。
 *
 * 状态机:running = 线程已创建且尚未 join;stop = 已请求退出。
 * 两个标志与条件变量都由 g_bg_lock 保护。线程醒来后先放掉控制锁
 * 再 purge:既没有持控制锁去等物理锁的必要,也让 stop 的唤醒不被
 * 一轮长回收挡住(stop 只负责通知,真正的等待在 join)。
 *
 * 周期等待用 CLOCK_REALTIME + pthread_cond_timedwait:1s 粒度下,
 * 系统时钟跳变最多让某一轮早到或晚到一次,不值得为
 * CLOCK_MONOTONIC 引入 condattr 设置。内层 while 吸收虚假唤醒,
 * 不会把周期重置。
 *
 * fork:后台线程只存在于父进程。子进程里它凭空消失,继承来的锁和
 * 状态可能停在"锁着/运行中"的任意时刻,atfork child 钩子用
 * wmm_bg_child_reset 重建为空闲状态。prepare 不必拿 g_bg_lock:
 * 线程对分配器共享结构的全部触碰都在 g_ext_lock 内,而 prepare
 * 已经把它冻住。
 */
#define WMM_BG_PERIOD_SEC 1

static pthread_t       g_bg_tid;
static pthread_mutex_t g_bg_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_bg_cond = PTHREAD_COND_INITIALIZER;
static bool            g_bg_running;   /* 已创建、尚未 join */
static bool            g_bg_stop;      /* 要求线程在下次醒来时退出 */

static void *wmm_bg_thread(void *arg) {
    (void)arg;

    pthread_mutex_lock(&g_bg_lock);
    for (;;) {
        struct timespec ts;
        (void)clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += WMM_BG_PERIOD_SEC;

        int rc = 0;
        while (!g_bg_stop && rc != ETIMEDOUT)
            rc = pthread_cond_timedwait(&g_bg_cond, &g_bg_lock, &ts);
        if (g_bg_stop)
            break;

        pthread_mutex_unlock(&g_bg_lock);
        wmm_phys_purge_if_needed();
        /* 共享监控区与回收同周期发布:先回收再发布,读到的
           active/frag 就是回收之后的状态,不会把已交还内核的页
           又算进"在用"。未创建共享区时本调用是空操作 */
        wmm_shm_publish();
        pthread_mutex_lock(&g_bg_lock);
    }
    pthread_mutex_unlock(&g_bg_lock);
    return NULL;
}

int wmm_bg_start(void) {
    int rc = 0;

    pthread_mutex_lock(&g_bg_lock);
    if (!g_bg_running) {
        g_bg_stop = false;
        rc = pthread_create(&g_bg_tid, NULL, wmm_bg_thread, NULL);
        if (rc == 0)
            g_bg_running = true;
    }
    pthread_mutex_unlock(&g_bg_lock);

    if (rc != 0) {
        /* pthread 接口把错误码作返回值,不写 errno;控制面的失败
           不是启动期契约设施失败:没有后台线程照样能分配,只是
           不自动回收,所以返回错误而不是 abort */
        errno = rc;
        return -1;
    }
    return 0;
}

int wmm_bg_stop(void) {
    pthread_t tid;

    pthread_mutex_lock(&g_bg_lock);
    if (!g_bg_running) {
        pthread_mutex_unlock(&g_bg_lock);
        return 0;   /* 幂等:没在运行就是目标状态 */
    }
    g_bg_stop = true;
    tid = g_bg_tid;
    pthread_cond_broadcast(&g_bg_cond);
    pthread_mutex_unlock(&g_bg_lock);

    /* join 必须在锁外:线程退出前还要拿 g_bg_lock 看 stop。
       同一时刻只应由一个控制线程调用启停(与 config 命令同纪律);
       两次并发 stop 会 join 同一个 tid,其中一个必然失败 */
    int rc = pthread_join(tid, NULL);

    pthread_mutex_lock(&g_bg_lock);
    g_bg_running = false;
    pthread_mutex_unlock(&g_bg_lock);

    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}

void wmm_bg_child_reset(void) {
    /* fork 只复制调用线程:父进程的后台线程在子进程里不存在,
       join 它等于 join 一个不存在的 tid。锁与条件变量可能停在
       任意中间状态(例如线程正持着 g_bg_lock 时 fork),重建是
       唯一可移植的做法。由 atfork child 钩子在分配器锁全部重建
       之后调用;此刻子进程单线程,无并发访问 */
    (void)pthread_mutex_init(&g_bg_lock, NULL);
    (void)pthread_cond_init(&g_bg_cond, NULL);
    g_bg_running = false;
    g_bg_stop    = false;
    memset(&g_bg_tid, 0, sizeof(g_bg_tid));
}
