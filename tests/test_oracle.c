/* test_oracle.c —— oracle 属性测试:随机百万步对照参考模型(单线程总验收)
 *
 * 思路:测试自带参考模型(活指针集合 + 请求大小),按固定种子驱动
 * alloc/free/realloc 一百万步,每一步都拿模型对账:
 *   ① 对齐:指针 16 字节对齐;唯一豁免 = 8 字节最小类(usable==8)
 *      ——契约明文允许它只对齐到 8,断言比契约严就是测试错;
 *   ② usable_size(p) ≥ 当初请求;
 *   ③ 释放只打集合里的指针:模型 free 后立即摘除,双释放没有
 *      "合法发生"的通道;释放侧的计数错误(少减一次)由 ④ 当场揪出;
 *   ④ Σ 各 bin live + 大对象 live − Σ 线程缓存 head == 模型活指针数。
 *      白盒直读 g_bins / g_large / g_tcache——逐对象计数只在对应
 *      锁内增减,单线程下读数即精确值。缓存里的对象仍计 live(见
 *      wmm_internal.h 的计数纪律),所以等式左边要扣掉缓存这一层,
 *      剩下的才是"应用持有"。字节版同一等式:模型按每次分配实测的
 *      usable 累计字节,逐步对账"Σ live*reg_size + 大对象字节 −
 *      缓存字节"——live_bytes 的累加链因此被钉在模型观测上,这是
 *      OBS1 恒等式的逐步版;快照路径自身的对账(快照 vs 直读)在
 *      检查点与终局各取一次——快照要 37 把锁加 5KB 清零,每步做会
 *      把 oracle 时长拉长数倍,而计数/字节两条逐步等式已经读的是
 *      快照所读的同一批全局变量。
 *
 * 每步另有两条深检查,每次只碰几十个字节,抓的却是最难查的错:
 *   - 存储指纹:分配后往头 8 字节写本代指纹,释放前校验原样还在。
 *     若分配器把仍在使用的存储又发了出去(重复交付),后一次写
 *     指纹会毁掉前一次的,释放时立刻暴露;
 *   - realloc 保内容:realloc 前在头部铺 128 字节已知模式,realloc
 *     后逐字节校验——拷贝偏移错、忘拷贝,都会在这里现形。
 *
 * 每 10 万步一次收敛检查:全量 free → Σ live 必须回到进场基线
 * (无泄漏语义);全量 alloc → Σ live 应恰好等于槽数;再全量 free
 * 复位,下一个十万步从空世界起步,波峰波谷反复压。核对前先刷空
 * 本线程缓存——缓存对象会把 Σ live 抬在基线上方,而收敛检查问的
 * 恰是"应用持有的部分归零没有"。
 *
 * glibc 对照模式:同一条随机序列(同种子、同代码路径 = 同一串随机
 * 数)再喂给真 malloc 家族跑一遍,验证的是行为兼容——同样的一百万
 * 步在 glibc 侧也必须无失败完成。对齐断言在此模式放宽到 8:debug
 * 库带 ASan,glibc malloc 实际由 ASan 分配器接管,它只承诺 8 字节
 * 对齐、usable 口径也是它自己的记账——对照不断言"与 wmm 相同",
 * 只断言"glibc 同样吃得下这串序列"。测试可执行文件不带 --wrap
 * 链接 libwmm.a,普通 malloc 名字仍落到系统家族,这是对照可行的
 * 前提。深检查同理关闭:ASan 对 malloc(1) 只放行 1 字节,头 8 字节
 * 指纹写法在它那里是越界;存储质量检查是给 wmm 的,glibc 是参照系。
 */
#include "wmm_internal.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>   /* malloc/free/realloc:glibc 对照模式用的就是它们 */
#include <malloc.h>   /* malloc_usable_size:glibc 侧的 usable 口径 */
#include <string.h>

/* mingw(msys2 本机语法自检环境)的 malloc.h 不声明 malloc_usable_size,
   glibc 在 <malloc.h> 里声明——这里只为本地 -fsyntax-only 补个原型,
   Linux 侧编译走系统声明,看不到这段 */
#if defined(__MINGW32__)
size_t malloc_usable_size(void *p);
#endif

static int g_fail;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("  FAIL @%d: %s\n", __LINE__, #cond);               \
            g_fail++;                                                  \
        }                                                              \
    } while (0)

/* 固定种子(黄金分割的十六进制前缀),失败可复现 */
static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;

static uint32_t rng(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 32);
}

/* 尺寸生成:覆盖 1..1MB 全区间,但按带宽加权、小对象为主。
   若按区间均匀取值,99% 的请求落进大对象路径,bin/slab 机制几乎
   得不到随机压测——而它恰是前面全部里程碑的主体。带宽边界取尺寸
   体系的关键刻度:64(16 的倍数类上界)、4096(查表线)、14336
   (小对象上界)、262144(中大型分界)。 */
static size_t gen_size(void) {
    uint32_t r = rng() % 100;
    if (r < 35) return 1 + rng() % 64;
    if (r < 65) return 65 + rng() % (4096 - 64);
    if (r < 80) return 4097 + rng() % (14336 - 4096);
    if (r < 95) return 14337 + rng() % (262144 - 14336);
    return 262145 + rng() % (1048576 - 262144);
}

/* Σ live:36 个常规 bin + 36 个 guard bin + 大对象。单线程测试直接
   读全局,无并发写者。guard bin 的 live 与常规 bin 同口径(对象在
   应用手里,含"已分配但尚未释放"),快照也把它们并进 cls[],两侧
   对账必须用同一口径。 */
static uint64_t total_live(void) {
    uint64_t sum = 0;
    for (int i = 0; i < WMM_NBINS; i++)
        sum += g_bins[i].live + g_guard_bins[i].live;
    return sum + g_large.live;
}

/* Σ live 的字节口径:直读全局重算,应与快照的 live_bytes 逐字节一致 */
static uint64_t live_bytes_direct(void) {
    uint64_t sum = 0;
    for (int i = 0; i < WMM_NBINS; i++)
        sum += (g_bins[i].live + g_guard_bins[i].live) *
               (uint64_t)wmm_bin_info[i].reg_size;
    return sum + g_large.live_bytes;
}

/* 线程缓存里的对象字节数。与 wmm_tcache_cached_total 同口径同锁,
   另把每类缓存对象折算成字节——"含缓存"的 live_bytes 对账要扣掉
   这一层(见文件头 ④) */
static uint64_t cached_bytes_total(void) {
    uint64_t sum = 0;
    pthread_mutex_lock(&g_threads_lock);
    for (wmm_tcache_t *t = g_threads; t != NULL; t = t->next)
        for (int i = 0; i < WMM_NBINS; i++)
            sum += t->bins[i].head * (uint64_t)wmm_bin_info[i].reg_size;
    pthread_mutex_unlock(&g_threads_lock);
    return sum;
}

enum { WMM_ORACLE_SLOTS = 512, WMM_ORACLE_STEPS = 1000000,
       WMM_ORACLE_GUARD_STEPS = 100000, WMM_ORACLE_CKPT = 100000 };

/* 模型槽位:非空指针即"活"。tag = 本代存储指纹(见文件头);
   只需按槽位区分:发现指纹被毁即"存储被重复交付或踩踏",
   代际序号并不参与判定。 */
typedef struct {
    unsigned char *p;
    size_t         req;
    uint64_t       tag;
} wmm_oref_t;
static wmm_oref_t g_slot[WMM_ORACLE_SLOTS];

/* 模型活对象的 usable 字节累计(仅 wmm 模式维护):每次分配/释放按
   实测 usable 增减,realloc 记新旧 usable 之差。对账见文件头 ④ 的
   字节版——它把分配器的 live_bytes 累加链逐步钉在模型观测上 */
static int64_t g_model_bytes;

#define WMM_TAG_MAGIC 0x4F5241434C455277ULL   /* "ORACLEw" 化身常数 */

/* 保内容模式的单字节:由指纹与偏移确定性生成,读回可直接对照 */
static uint8_t pat_byte(uint64_t tag, size_t off) {
    return (uint8_t)((tag >> ((off & 7u) * 8)) ^ off ^ 0xA5u);
}

/* 引擎表:同一套驱动逻辑跑两种分配器。走函数指针而不是 if/else,
   是为了让"序列生成"与"对账"两种关注点彻底分开。随机流的一致性
   是结构保证:每步抽取次数恒定(分支、槽位、尺寸各固定次数,与
   槽位占用无关——尺寸在分支前预抽,释放步抽了不用也照抽),两种
   引擎看到的是同一条序列;个别分配失败只让两侧 CHECK 记罪,
   不会让流错位。 */
typedef struct {
    void  *(*alloc)(size_t);
    void  *(*realloc)(void *, size_t);
    void   (*free)(void *);
    size_t (*usable)(void *);
    bool    strict16;    /* 真 = wmm 契约口径(16 对齐,8B 类豁免);
                            假 = glibc/ASan 口径(只断 8) */
    bool    can_count;   /* 真 = 可白盒读内部计数,执行 ④;glibc 无可见性 */
    bool    deep;        /* 真 = 指纹/保内容深检查(仅 wmm:见文件头) */
    const char *name;
} wmm_oops_t;

static size_t usable_wmm(void *p)   { return wmm_usable_size(p); }
static size_t usable_glibc(void *p) { return malloc_usable_size(p); }

/* 从 start 起环形找下一个"空"(want_live 假)或"活"(真)槽位。
   占用均衡在 75%(推导见 run_mode),空/活槽位都不缺,期望两三次
   探测即命中;找不到返回 -1。 */
static int find_slot(bool want_live, size_t start) {
    for (size_t k = 0; k < WMM_ORACLE_SLOTS; k++) {
        size_t s = (start + k) % WMM_ORACLE_SLOTS;
        if ((g_slot[s].p != NULL) == want_live)
            return (int)s;
    }
    return -1;
}

/* 分配并登账。返回对 n_live 的增量:成功 1;失败 0(已 CHECK 记罪,
   不入模型——两种引擎只在成功处分岔,随机流才不至错位)。 */
static int op_alloc(const wmm_oops_t *o, size_t s, size_t req) {
    unsigned char *p = (unsigned char *)o->alloc(req);
    CHECK(p != NULL);
    if (p == NULL)
        return 0;

    size_t u = o->usable(p);
    CHECK(u >= req);                                    /* ② */
    if (o->strict16)                                    /* ① */
        CHECK(((uintptr_t)p & 15u) == 0 || u == 8u);
    else
        CHECK(((uintptr_t)p & 7u) == 0);

    if (o->deep) {
        uint64_t tag = WMM_TAG_MAGIC ^ ((uint64_t)s << 32);
        memcpy(p, &tag, sizeof tag);   /* 头 8 字节落在 usable 内:
                                          最小类 usable 也恰为 8 */
        g_slot[s].tag = tag;
    }
    g_slot[s].p   = p;
    g_slot[s].req = req;
    if (o->can_count)
        g_model_bytes += (int64_t)u;   /* 字节版模型:按实测 usable 记账 */
    return 1;
}

/* 释放并销账。返回增量 -1;槽位为空 = 断言失败,返回 0。 */
static int op_free(const wmm_oops_t *o, size_t s) {
    CHECK(g_slot[s].p != NULL);                         /* ③ */
    if (g_slot[s].p == NULL)
        return 0;
    if (o->deep) {
        uint64_t tag;
        memcpy(&tag, g_slot[s].p, sizeof tag);
        CHECK(tag == g_slot[s].tag);   /* 指纹被毁 = 存储重复交付/被踩 */
    }
    if (o->can_count)
        g_model_bytes -= (int64_t)o->usable(g_slot[s].p);   /* 释放前取值 */
    o->free(g_slot[s].p);
    g_slot[s].p = NULL;
    return -1;
}

/* realloc 并对保内容契约对账。槽位数不变,返回 0。 */
static int op_realloc(const wmm_oops_t *o, size_t s, size_t nreq) {
    unsigned char *old = g_slot[s].p;
    size_t         oreq = g_slot[s].req;
    size_t         old_u = 0;

    if (o->deep || o->can_count)
        old_u = o->usable(old);   /* realloc 可能搬移,旧块信息先取 */

    if (o->deep) {
        size_t fill = oreq < 128 ? oreq : 128;
        for (size_t k = 0; k < fill; k++)
            old[k] = pat_byte(g_slot[s].tag, k);
    }

    unsigned char *np = (unsigned char *)o->realloc(old, nreq);
    CHECK(np != NULL);
    if (np == NULL)
        return 0;   /* 契约:失败时旧块原封不动,模型保持原状 */

    size_t u = o->usable(np);
    CHECK(u >= nreq);                                   /* ② */
    if (o->strict16)                                    /* ① */
        CHECK(((uintptr_t)np & 15u) == 0 || u == 8u);
    else
        CHECK(((uintptr_t)np & 7u) == 0);

    if (o->deep) {
        /* 契约:新大小不超旧 usable 时原地返回,不搬移 */
        if (nreq <= old_u)
            CHECK(np == old);

        /* 保内容:契约承诺保留 min(旧可用, 新大小) 字节;检查范围
           取两个请求的较小者再封顶 128——旧请求 ≤ 旧可用,以模型
           确知的旧请求为界是稳妥侧,也把开销钉死在百字节级 */
        size_t keep = oreq < nreq ? oreq : nreq;
        if (keep > 128)
            keep = 128;
        for (size_t k = 0; k < keep; k++)
            CHECK(np[k] == pat_byte(g_slot[s].tag, k));

        memcpy(np, &g_slot[s].tag, sizeof g_slot[s].tag);   /* 新代指纹 */
    }
    g_slot[s].p   = np;
    g_slot[s].req = nreq;
    if (o->can_count)
        g_model_bytes += (int64_t)u - (int64_t)old_u;
    return 0;
}

/* OBS1 快照钩子:取一次快照,与白盒直读逐项对账。快照口径与直读同为
   "含线程缓存"(见 wmm_internal.h),两侧一致,无需扣缓存层 */
static void obs1_snapshot_check(void) {
    wmm_stats_t st;
    CHECK(wmm_stats_snapshot(&st) == 0);
    CHECK(st.live_bytes == live_bytes_direct());
    CHECK(st.live_objects == total_live());
    CHECK(st.active_bytes >= st.live_bytes);                  /* 口径不变式 */
    CHECK(st.frag_bytes == st.active_bytes - st.live_bytes);
}

/* 收敛检查:全量 free → 全量 alloc → 全量 free,三段各自对账 */
static void run_checkpoint(const wmm_oops_t *o, size_t step,
                           long *n_live, uint64_t live0, uint64_t live_bytes0) {
    int fail0 = g_fail;

    for (size_t s = 0; s < WMM_ORACLE_SLOTS; s++)
        if (g_slot[s].p != NULL)
            *n_live += op_free(o, s);
    if (o->can_count) {
        wmm_tcache_flush_all();            /* 缓存层剥掉,才谈得上归零 */
        CHECK(total_live() == live0);      /* 回到进场基线 = 无泄漏 */
        CHECK(live_bytes_direct() == live_bytes0);   /* 字节口径同归 */
        CHECK(g_model_bytes == 0);         /* 模型自身也要对得上 */
    }

    /* 无条件逐槽分配:抽取次数与状态无关,两引擎的检查点序列对齐 */
    for (size_t s = 0; s < WMM_ORACLE_SLOTS; s++)
        *n_live += op_alloc(o, s, gen_size());
    if (o->can_count) {
        wmm_tcache_flush_all();
        CHECK(total_live() - live0 == WMM_ORACLE_SLOTS);   /* 拉满恰为槽数 */
        /* 字节版:缓存已空,直接对模型累计。快照钩子取在满槽世界——
           这是 live_bytes 最大、尺寸类分布最广的采样点 */
        CHECK(live_bytes_direct() - live_bytes0 == (uint64_t)g_model_bytes);
        obs1_snapshot_check();
    }

    for (size_t s = 0; s < WMM_ORACLE_SLOTS; s++)
        if (g_slot[s].p != NULL)
            *n_live += op_free(o, s);
    if (o->can_count) {
        wmm_tcache_flush_all();
        CHECK(total_live() == live0);      /* 复位:下一循环空世界起步 */
    }

    if (o->can_count && g_fail == fail0)
        printf("  checkpoint at step %zu: free 0 / full %d / free 0, OK\n",
               step, WMM_ORACLE_SLOTS);
}

/* 一轮完整模式:复位种子后跑满 STEPS 步。
 * 操作配比(rng 低两位):0 = 分配,1/2 = 释放,3 = realloc。
 * 占用率均衡点 q=3/4,推导:分配支每步期望 +1/4(未满必成功);
 * 释放支打随机槽位,活则释放、空则改分配,期望 (1-2q)/2;realloc
 * 净 0。合计 3/4 - q = 0。活指针数因此在 384 上下随机游走,④ 对账
 * 的是一个宽变化区间上的数;每个十万步段从 0 爬回均衡,波峰波谷
 * 反复压满。
 * 释放为什么必须打"随机槽位"而不能"扫到任意活槽就释放"(首轮
 * 实跑 peak live 20/512 暴露的坑):扫描版的成功率与占用率无关
 * ——表非空必命中,期望净漂移恒 -1/4/步,占用率被钉死在 0 附近。
 * churn 依然全覆盖,但活集合压力形同虚设,长活短活混杂的碎片
 * 形态根本测不到;打点版成功率随占用率升高,负反馈天然形成,
 * 均衡才是内点。 */
static void run_mode(const wmm_oops_t *o, size_t steps) {
    g_rng = 0x9E3779B97F4A7C15ULL;   /* 两种模式同种子 = 同一条序列 */
    memset(g_slot, 0, sizeof g_slot);
    g_model_bytes = 0;

    /* 进场先刷空本线程缓存:④ 的基线 live0 里若混着缓存对象,
       "Σ live − 缓存"的对账从第一步起就偏。刷出只把对象从缓存
       搬回 slab,不创造也不销毁;glibc 模式无缓存,不会走到这里 */
    if (o->can_count)
        wmm_tcache_flush_all();

    /* ④ 的对账基线取进场快照而非绝对零:本文件跑在 boot/slab/
       api/phys 之后,它们留下的计数史不归本测试管。绝对零只允许
       断在测试自己强制出来的状态上——那正是收敛检查在断的东西 */
    uint64_t live0 = o->can_count ? total_live() : 0;
    uint64_t live_bytes0 = o->can_count ? live_bytes_direct() : 0;
    long     n_live = 0, peak = 0;
    uint64_t n_a = 0, n_f = 0, n_r = 0;

    for (size_t step = 1; step <= steps; step++) {
        uint32_t r = rng() & 3u;
        size_t   st = rng() % WMM_ORACLE_SLOTS;
        /* 尺寸在分支前预抽、每步恰抽一次:抽取次数与槽位占用、与
           分支走向都无关,两种引擎看到的随机流才逐位相同 */
        size_t   sz = gen_size();

        if (r == 0) {                    /* 分配:优先空槽;表满先腾位 */
            int s = find_slot(false, st);
            if (s < 0) {
                int v = find_slot(true, 0);
                if (v >= 0) {
                    n_live += op_free(o, (size_t)v);
                    n_f++;
                }
                s = v;
            }
            if (s >= 0) {
                n_live += op_alloc(o, (size_t)s, sz);
                n_a++;
            }
        } else if (r <= 2) {             /* 释放:打中活槽才释放;打中
                                            空槽回退分配(该槽必空,
                                            直接用)。打点而非扫描,
                                            均衡点推导见上 */
            if (g_slot[st].p != NULL) {
                n_live += op_free(o, st);
                n_f++;
            } else {
                n_live += op_alloc(o, st, sz);
                n_a++;
            }
        } else {                         /* realloc:优先活槽;表空改分配 */
            int s = find_slot(true, st);
            if (s < 0) {
                n_live += op_alloc(o, st, sz);
                n_a++;
            } else {
                n_live += op_realloc(o, (size_t)s, sz);
                n_r++;
            }
        }

        if (o->can_count) {              /* ④ 逐步对账:扣掉缓存层 */
            CHECK(total_live() - live0 - wmm_tcache_cached_total()
                      == (uint64_t)n_live);
            /* 字节版同一对账:live_bytes 的累加链逐步钉在模型实测上 */
            CHECK(live_bytes_direct() - live_bytes0 - cached_bytes_total()
                      == (uint64_t)g_model_bytes);
        }
        if (n_live > peak)
            peak = n_live;

        if (step % WMM_ORACLE_CKPT == 0)
            run_checkpoint(o, step, &n_live, live0, live_bytes0);
    }

    /* 终局:全量释放后,世界必须与进场时一样空 */
    for (size_t s = 0; s < WMM_ORACLE_SLOTS; s++)
        if (g_slot[s].p != NULL)
            n_live += op_free(o, s);
    CHECK(n_live == 0);                  /* 模型自身也要对得上 */
    if (o->can_count) {
        wmm_tcache_flush_all();
        CHECK(total_live() == live0);
        CHECK(live_bytes_direct() == live_bytes0);   /* 字节口径同归基线 */
        CHECK(g_model_bytes == 0);
        obs1_snapshot_check();           /* 终局快照:与直读一致 */
    }

    printf("  %s mode: %zu steps, %llu allocs / %llu frees / %llu reallocs, "
           "peak live %ld\n",
           o->name, steps,
           (unsigned long long)n_a, (unsigned long long)n_f,
           (unsigned long long)n_r, peak);
}

int test_oracle(void) {
    g_fail = 0;
    wmm_boot();

    printf("  %d steps, %d slots, checkpoint every %d, fixed seed\n",
           WMM_ORACLE_STEPS, WMM_ORACLE_SLOTS, WMM_ORACLE_CKPT);

    static const wmm_oops_t wmm_ops = {
        .alloc = wmm_alloc, .realloc = wmm_realloc, .free = wmm_free,
        .usable = usable_wmm, .strict16 = true, .can_count = true,
        .deep = true, .name = "wmm",
    };
    static const wmm_oops_t libc_ops = {
        .alloc = malloc, .realloc = realloc, .free = free,
        .usable = usable_glibc, .strict16 = false, .can_count = false,
        .deep = false, .name = "glibc",
    };
    static const wmm_oops_t guard_ops = {
        .alloc = wmm_alloc, .realloc = wmm_realloc, .free = wmm_free,
        .usable = usable_wmm, .strict16 = true, .can_count = true,
        .deep = true, .name = "wmm+guard",
    };

    run_mode(&wmm_ops, WMM_ORACLE_STEPS);
    run_mode(&libc_ops, WMM_ORACLE_STEPS);

    /* guard 通道再用同一条随机序列压一段(步数少一个量级:每个小
       对象独占 slab 且走全局锁,状态机分支在十万步内已反复覆盖)。
       对账口径不变——guard bin 的 live 已并入 total_live/字节和,
       这正是对"两套布局共享同一套计数语义"的验证。关闭时隔离区、
       回收池与仍在应用的 guard 对象都会被正确收尾,随后的终局
       对账要求世界回到进场基线。 */
    CHECK(wmm_ctl("guard.enable") == 0);
    run_mode(&guard_ops, WMM_ORACLE_GUARD_STEPS);
    CHECK(wmm_ctl("guard.disable") == 0);

    if (g_fail == 0)
        printf("  all oracle checks passed\n");
    return g_fail;
}
