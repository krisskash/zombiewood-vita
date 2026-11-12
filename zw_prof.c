/* zw_prof.c - measurement only (ZW_CALLPROF=1). Never ship a build with it on.
 *
 * An instrumenting call profiler for the game's main thread:
 *   - every imported function (GOT jump slots: libc, GL, pthread ...),
 *   - every virtual function reachable through an exported vtable (_ZTV*),
 *   - every direct Thumb `bl` to a C++ function inside the per-frame call tree
 *     (breadth-first from the frame roots and from Update/Render-like virtuals).
 * Slots are pointed at small ARM stubs; `bl` sites go through one veneer inside the .so
 * and are identified by their return address. Both end in the same trampoline, which
 * keeps r0-r3 and the stack untouched, so any signature works (stack args, 64-bit
 * returns). Other threads pass straight through (stack-range check).
 * Cheap functions that are called very often are un-instrumented again after their
 * first frame ("pruned"), so the overhead stays bounded; their call counts are reported.
 * While GSLevel is the current state, a report is written every ZW_PROF_WINDOW frames to
 * ux0:data/zombiewood/prof/prof_NN.txt (exclusive / inclusive ms per frame, calls). */
#ifndef ZW_CALLPROF
#define ZW_CALLPROF 0
#endif
#if ZW_CALLPROF

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <kubridge.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "so_util.h"

extern void zw_log_raw(const char *s);
#define PLOG(...) do { char _b[256]; snprintf(_b, sizeof(_b), __VA_ARGS__); zw_log_raw(_b); } while (0)

#define ZW_PROF_MAXSTAT  16384
#define ZW_PROF_MAXPATCH 24576
#define ZW_PROF_MAXSITE  20480
#define ZW_PROF_DEPTH    512
#define ZW_PROF_WINDOW   600     /* frames per report */
#define ZW_PROF_WARMUP   90      /* level frames before the first window (pruning settles) */
#define ZW_PROF_MAXREP   40
#define ZW_PROF_PRUNE_CALLS 150  /* per frame ... */
#define ZW_PROF_PRUNE_US    4    /* ... and cheaper than this per call (exclusive) */
#define ZW_PROF_BFS_DEPTH   5
#define ZW_PROF_VENEER_SLOT 27   /* in the dead body of ZombiesGame::LoadIconTexture */
#define ZW_ICON_FN_OFF   0x3d7a94u
#define ZW_ICON_FN_SIZE  248u

typedef struct {
    uint32_t target;      /* absolute address incl. Thumb bit */
    const char *name;
    uint32_t incl, excl;  /* us in the current window */
    uint32_t calls, kids;
    uint32_t fexcl;       /* us this frame */
    uint16_t fcalls, depth;
    uint8_t flags;        /* 1 = pruned, 2 = never prune */
    uint8_t pad[3];
    uint32_t prune_calls; /* calls in the frame it was pruned */
    uint32_t fincl;       /* inclusive us this frame (spike mode) */
} zw_pstat;
typedef struct { uint32_t lr, sp; uint64_t t0; uint32_t child; uint32_t stat; } zw_pframe;
typedef struct { uint32_t *slot; uint32_t stat; } zw_ppatch;
typedef struct { uint32_t ret; uint32_t stat; } zw_psite;   /* ret = site + 4, Thumb bit set */

extern uint8_t zw_prof_stubs[];
extern void zw_prof_site(void);

static zw_pstat  g_st[ZW_PROF_MAXSTAT];
static int       g_nst;
static zw_ppatch g_patch[ZW_PROF_MAXPATCH];
static int       g_npatch;
static zw_psite  g_site[ZW_PROF_MAXSITE];
static int       g_nsite;
static zw_pframe g_stk[ZW_PROF_DEPTH];
static int       g_sp;
static volatile int g_on;
static uint32_t  g_main_lo, g_main_size;
static so_module *g_mod;
static uint16_t  g_hash[32768];   /* target -> stat index + 1 */
static unsigned  g_lost;          /* frames dropped by the stale-frame rule */

/* ── hot path ── */
static inline uint64_t zw_prof_push(uint32_t idx, uint32_t lr, uint32_t sp) {
    zw_pstat *s = &g_st[idx];
    uint32_t target = s->target;
    if (!g_on || (uint32_t)(sp - g_main_lo) >= g_main_size || g_sp >= ZW_PROF_DEPTH) return target;
    while (g_sp > 0 && g_stk[g_sp - 1].sp < sp) { g_sp--; if (g_st[g_stk[g_sp].stat].depth) g_st[g_stk[g_sp].stat].depth--; g_lost++; }
    s->depth++;
    if (s->fcalls != 0xffff) s->fcalls++;
    if (g_sp > 0) g_st[g_stk[g_sp - 1].stat].kids++;
    zw_pframe *f = &g_stk[g_sp++];
    f->lr = lr; f->sp = sp; f->stat = idx; f->child = 0;
    f->t0 = sceKernelGetProcessTimeWide();
    return (uint64_t)target | (1ull << 32);
}
uint64_t zw_prof_enter_stub(uint32_t stub, uint32_t lr, uint32_t sp) {
    return zw_prof_push((stub - (uint32_t)(uintptr_t)zw_prof_stubs) >> 3, lr, sp);
}
uint64_t zw_prof_enter_site(uint32_t lr, uint32_t sp) {
    int lo = 0, hi = g_nsite - 1;
    while (lo < hi) { int mid = (lo + hi) >> 1; if (g_site[mid].ret < lr) lo = mid + 1; else hi = mid; }
    return zw_prof_push(g_site[lo].stat, lr, sp);
}
uint32_t zw_prof_exit(uint32_t sp) {
    uint64_t now = sceKernelGetProcessTimeWide();
    while (g_sp > 1 && g_stk[g_sp - 1].sp < sp) { g_sp--; if (g_st[g_stk[g_sp].stat].depth) g_st[g_stk[g_sp].stat].depth--; g_lost++; }
    zw_pframe *f = &g_stk[--g_sp];
    zw_pstat *s = &g_st[f->stat];
    uint32_t el = (uint32_t)(now - f->t0);
    uint32_t ex = el > f->child ? el - f->child : 0;
    s->calls++; s->excl += ex; s->fexcl += ex;
    if (s->depth && --s->depth == 0) { s->incl += el; s->fincl += el; }
    if (g_sp > 0) g_stk[g_sp - 1].child += el;
    return f->lr;
}

/* ── setup ── */
typedef struct { uint32_t addr; uint32_t size; const char *name; } zw_pfn;   /* addr incl. Thumb bit, .so-relative */
static zw_pfn *g_fn; static int g_nfn;
static int zw_pfn_cmp(const void *a, const void *b) {
    uint32_t x = ((const zw_pfn *)a)->addr & ~1u, y = ((const zw_pfn *)b)->addr & ~1u;
    return x < y ? -1 : x > y;
}
static int zw_pfn_find(uint32_t off) {   /* off: .so-relative, Thumb bit ignored; exact start only */
    int lo = 0, hi = g_nfn - 1;
    off &= ~1u;
    while (lo <= hi) { int mid = (lo + hi) >> 1; uint32_t a = g_fn[mid].addr & ~1u;
        if (a == off) return mid; if (a < off) lo = mid + 1; else hi = mid - 1; }
    return -1;
}
static int zw_prof_stat(uint32_t target, const char *name) {
    uint32_t h = (target * 2654435761u) >> 17;
    for (;; h = (h + 1) & 32767) {
        if (!g_hash[h]) break;
        if (g_st[g_hash[h] - 1].target == target) return g_hash[h] - 1;
    }
    if (g_nst >= ZW_PROF_MAXSTAT) return -1;
    g_st[g_nst].target = target; g_st[g_nst].name = name;
    g_hash[h] = (uint16_t)(g_nst + 1);
    return g_nst++;
}
static void zw_prof_poke(void *dst, uint32_t v) { kuKernelCpuUnrestrictedMemcpy(dst, &v, 4); }
static uint32_t zw_prof_bl(uint32_t from, uint32_t to) {
    int32_t off = (int32_t)to - (int32_t)(from + 4);
    uint32_t S = (off >> 24) & 1, I1 = (off >> 23) & 1, I2 = (off >> 22) & 1;
    uint32_t J1 = (~I1 ^ S) & 1, J2 = (~I2 ^ S) & 1;
    uint16_t h1 = 0xf000 | (S << 10) | ((off >> 12) & 0x3ff);
    uint16_t h2 = 0xd000 | (J1 << 13) | (J2 << 11) | ((off >> 1) & 0x7ff);
    return h1 | ((uint32_t)h2 << 16);
}
static int zw_prof_slot(uint32_t *slot, uint32_t target, const char *name) {
    if (g_npatch >= ZW_PROF_MAXPATCH) return 0;
    int s = zw_prof_stat(target, name);
    if (s < 0) return 0;
    g_patch[g_npatch].slot = slot; g_patch[g_npatch].stat = (uint32_t)s; g_npatch++;
    zw_prof_poke(slot, (uint32_t)(uintptr_t)zw_prof_stubs + (uint32_t)s * 8);
    return 1;
}
static int zw_prof_import_ok(const char *n) {
    static const char *const bad[] = { "setjmp", "longjmp", "_setjmp", "_longjmp", "sigsetjmp", "siglongjmp",
        "abort", "exit", "_exit", "pthread_exit", "dl_unwind_find_exidx", NULL };
    if (!strncmp(n, "__", 2) || !strncmp(n, "_Unwind", 7)) return 0;
    for (int i = 0; bad[i]; i++) if (!strcmp(n, bad[i])) return 0;
    return 1;
}
static int zw_prof_is_frame_virtual(const char *n) {
    static const char *const kw[] = { "6UpdateE", "6RenderE", "9OnAnimateE", "6renderE", "6updateE", "7animateE",
        "4DrawE", "4drawE", "4StepE", "7OnEventE", "7advanceE", "7displayE", "7AnimateE", "19OnRegisterSceneNodeE",
        "5SolveE", NULL };
    for (int i = 0; kw[i]; i++) if (strstr(n, kw[i])) return 1;
    return 0;
}
static int zw_psite_cmp(const void *a, const void *b) {
    uint32_t x = ((const zw_psite *)a)->ret, y = ((const zw_psite *)b)->ret;
    return x < y ? -1 : x > y;
}
/* collect the `bl` sites of one function; returns callees through cb queue */
static void zw_prof_scan(int fi, int *queue, int *qn, uint8_t *seen, int qmax) {
    uint32_t base = (uint32_t)g_mod->text_base;
    uint32_t a = g_fn[fi].addr, size = g_fn[fi].size;
    if (!(a & 1) || size < 4) return;
    a &= ~1u;
    if (a + size > ZW_ICON_FN_OFF && a < ZW_ICON_FN_OFF + ZW_ICON_FN_SIZE) return;   /* veneer area */
    const uint16_t *c = (const uint16_t *)(uintptr_t)(base + a);
    for (uint32_t i = 0; i + 4 <= size; ) {
        uint16_t h1 = c[i >> 1];
        if ((h1 & 0xf800) < 0xe800) { i += 2; continue; }
        uint16_t h2 = c[(i >> 1) + 1];
        if ((h1 & 0xf800) == 0xf000 && (h2 & 0xd000) == 0xd000) {
            uint32_t S = (h1 >> 10) & 1, J1 = (h2 >> 13) & 1, J2 = (h2 >> 11) & 1;
            uint32_t I1 = 1 ^ (J1 ^ S), I2 = 1 ^ (J2 ^ S);
            int32_t off = (int32_t)((S << 24) | (I1 << 23) | (I2 << 22) | ((uint32_t)(h1 & 0x3ff) << 12) | ((uint32_t)(h2 & 0x7ff) << 1));
            if (S) off -= 1 << 25;
            uint32_t t = a + i + 4 + (uint32_t)off;
            int ti = zw_pfn_find(t);
            if (ti >= 0 && (g_fn[ti].addr & 1) && g_fn[ti].name[0] == '_' && g_fn[ti].name[1] == 'Z' && g_nsite < ZW_PROF_MAXSITE) {
                int s = zw_prof_stat(base + g_fn[ti].addr, g_fn[ti].name);
                if (s >= 0) {
                    g_site[g_nsite].ret = (base + a + i + 4) | 1; g_site[g_nsite].stat = (uint32_t)s; g_nsite++;
                    if (!seen[ti] && *qn < qmax) { seen[ti] = 1; queue[(*qn)++] = ti; }
                }
            }
        }
        i += 4;
    }
}

static void __attribute__((noinline)) zw_prof_dummy(void) { __asm__ volatile(""); }
static int g_dummy_stat = -1;
static uint32_t g_cal_total_ns, g_cal_inside_ns;

void zw_prof_init(so_module *mod) {
    static const char *const roots[] = {
        "_ZN7GSLevel6UpdateEP11Applicationj", "_ZN7GSLevel6RenderEP11Application", "_ZN6CLevel6UpdateEj",
        "_ZN11Application5_DrawEv", "_ZN11Application6UpdateEv", "_ZN11Application7_UpdateEi",
        "_ZN11ZombiesGame6UpdateEv", "_ZN12FlashManager9SWFRenderEv", "_ZN12FlashManager9SWFUpdateEi",
        "_ZN3glf13AndroidUpdateEv", "_ZN3glf3App6UpdateEv", "_ZN7CPlayer6UpdateEi",
        "_ZN8CFlashFX6RenderEv", "_ZN8CFlashFX6UpdateEi", NULL };
    uint32_t base = (uint32_t)mod->text_base;
    g_mod = mod;
    SceKernelThreadInfo ti; memset(&ti, 0, sizeof(ti)); ti.size = sizeof(ti);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti) < 0 || !ti.stack) { PLOG("  prof: no thread info, off\n"); return; }
    g_main_lo = (uint32_t)(uintptr_t)ti.stack; g_main_size = (uint32_t)ti.stackSize;

    /* function table from .dynsym */
    g_fn = malloc(sizeof(zw_pfn) * (size_t)mod->num_dynsym);
    if (!g_fn) { PLOG("  prof: no memory for the symbol table, off\n"); return; }
    for (int i = 0; i < mod->num_dynsym; i++) {
        Elf32_Sym *s = &mod->dynsym[i];
        if (ELF32_ST_TYPE(s->st_info) != STT_FUNC || !s->st_shndx || !s->st_value) continue;
        g_fn[g_nfn].addr = s->st_value; g_fn[g_nfn].size = s->st_size; g_fn[g_nfn].name = mod->dynstr + s->st_name; g_nfn++;
    }
    qsort(g_fn, (size_t)g_nfn, sizeof(zw_pfn), zw_pfn_cmp);
    {   int w = 0;   /* drop aliases */
        for (int i = 0; i < g_nfn; i++) if (!w || (g_fn[i].addr & ~1u) != (g_fn[w - 1].addr & ~1u)) g_fn[w++] = g_fn[i];
        g_nfn = w; }

    g_dummy_stat = zw_prof_stat((uint32_t)(uintptr_t)zw_prof_dummy, "(profiler calibration)");
    g_st[g_dummy_stat].flags = 2;

    uint8_t *seen = calloc(1, (size_t)g_nfn);
    int qmax = 8192, qn = 0;
    int *queue = malloc(sizeof(int) * (size_t)qmax);
    if (!seen || !queue) { PLOG("  prof: no memory for the scan, off\n"); return; }

    /* 1. imports */
    int nimp = 0;
    for (int i = 0; i < mod->num_relplt; i++) {
        Elf32_Rel *r = &mod->relplt[i];
        if (ELF32_R_TYPE(r->r_info) != R_ARM_JUMP_SLOT) continue;
        Elf32_Sym *s = &mod->dynsym[ELF32_R_SYM(r->r_info)];
        const char *n = mod->dynstr + s->st_name;
        uint32_t *slot = (uint32_t *)(uintptr_t)(base + r->r_offset);
        uint32_t v = *slot;
        if (s->st_shndx || !v || (v >= base && v - base < 0x02000000u) || !zw_prof_import_ok(n)) continue;
        nimp += zw_prof_slot(slot, v, n);
    }
    /* 2. virtual functions */
    int nvt = 0, nvslot = 0;
    for (int i = 0; i < mod->num_dynsym; i++) {
        Elf32_Sym *s = &mod->dynsym[i];
        const char *n = mod->dynstr + s->st_name;
        if (!s->st_shndx || !s->st_value || s->st_size < 12 || strncmp(n, "_ZTV", 4)) continue;
        uint32_t *vt = (uint32_t *)(uintptr_t)(base + s->st_value);
        nvt++;
        for (uint32_t k = 0; k < s->st_size / 4; k++) {
            uint32_t v = vt[k];
            if (v < base || v - base >= (uint32_t)mod->text_size) continue;
            int fi = zw_pfn_find(v - base);
            if (fi < 0 || base + g_fn[fi].addr != v) continue;
            if (!zw_prof_slot(&vt[k], v, g_fn[fi].name)) continue;
            nvslot++;
            if (!seen[fi] && zw_prof_is_frame_virtual(g_fn[fi].name) && qn < qmax) { seen[fi] = 1; queue[qn++] = fi; }
        }
    }
    /* 3. direct call sites, breadth-first */
    for (int i = 0; roots[i]; i++) {
        uintptr_t a = so_symbol(mod, roots[i]);
        int fi = a ? zw_pfn_find((uint32_t)a - base) : -1;
        if (fi < 0) { PLOG("  prof: root %s not found\n", roots[i]); continue; }
        if (!seen[fi] && qn < qmax) { seen[fi] = 1; queue[qn++] = fi; }
    }
    {   int head = 0, level_end = qn, depth = 0, scanned = 0;
        while (head < qn && depth < ZW_PROF_BFS_DEPTH) {
            zw_prof_scan(queue[head++], queue, &qn, seen, qmax); scanned++;
            if (head == level_end) { depth++; level_end = qn; }
        }
        PLOG("  prof: scanned %d functions to depth %d, %d queued\n", scanned, depth, qn);
    }
    qsort(g_site, (size_t)g_nsite, sizeof(zw_psite), zw_psite_cmp);
    {   /* one veneer for all sites: ldr.w pc,[pc]; .word zw_prof_site */
        uint32_t ven = base + ZW_ICON_FN_OFF + 8 + ZW_PROF_VENEER_SLOT * 8;
        uint32_t v[2] = { 0xf000f8df, (uint32_t)(uintptr_t)zw_prof_site };
        kuKernelCpuUnrestrictedMemcpy((void *)(uintptr_t)ven, v, sizeof(v));
        for (int i = 0; i < g_nsite; i++) {
            uint32_t site = (g_site[i].ret & ~1u) - 4;
            zw_prof_poke((void *)(uintptr_t)site, zw_prof_bl(site, ven));
        }
    }
    free(seen); free(queue); free(g_fn); g_fn = NULL;
    so_flush_caches(mod);
    PLOG("  prof: ON - %d imports, %d vtables / %d slots, %d call sites, %d functions tracked (main stack %08x+%x)\n",
         nimp, nvt, nvslot, g_nsite, g_nst, (unsigned)g_main_lo, (unsigned)g_main_size);
    g_on = 1;
}

static void zw_prof_prune(int idx) {
    zw_pstat *s = &g_st[idx];
    uint32_t base = (uint32_t)g_mod->text_base;
    uint32_t ven = base + ZW_ICON_FN_OFF + 8 + ZW_PROF_VENEER_SLOT * 8;
    s->flags |= 1; s->prune_calls = s->fcalls;
    for (int i = 0; i < g_npatch; i++)
        if (g_patch[i].stat == (uint32_t)idx) zw_prof_poke(g_patch[i].slot, s->target);
    for (int i = 0; i < g_nsite; i++)
        if (g_site[i].stat == (uint32_t)idx) {
            uint32_t site = (g_site[i].ret & ~1u) - 4;
            if (*(uint32_t *)(uintptr_t)site == zw_prof_bl(site, ven)) zw_prof_poke((void *)(uintptr_t)site, zw_prof_bl(site, s->target & ~1u));
        }
}

static const zw_pstat *g_sort_base;
static int zw_prof_cmp_excl(const void *a, const void *b) {
    uint32_t x = g_sort_base[*(const uint16_t *)a].excl, y = g_sort_base[*(const uint16_t *)b].excl;
    return x > y ? -1 : x < y;
}
static int zw_prof_cmp_incl(const void *a, const void *b) {
    uint32_t x = g_sort_base[*(const uint16_t *)a].incl, y = g_sort_base[*(const uint16_t *)b].incl;
    return x > y ? -1 : x < y;
}
static int zw_prof_cmp_prune(const void *a, const void *b) {
    uint32_t x = g_sort_base[*(const uint16_t *)a].prune_calls, y = g_sort_base[*(const uint16_t *)b].prune_calls;
    return x > y ? -1 : x < y;
}
static char g_rep[96 * 1024];
static uint16_t g_ord[ZW_PROF_MAXSTAT];
static void zw_prof_report(int rep, int frames, uint64_t rend_us, uint64_t swap_us, uint64_t wall_us, int first_frame) {
    int n = 0, m = 0, np = 0; uint64_t calls = 0;
    double fr = frames ? (double)frames : 1.0;
    for (int i = 0; i < g_nst; i++) { if (g_st[i].calls) { g_ord[m++] = (uint16_t)i; calls += g_st[i].calls; } if (g_st[i].flags & 1) np++; }
#define RP(...) do { if (n < (int)sizeof(g_rep) - 400) n += snprintf(g_rep + n, sizeof(g_rep) - (size_t)n, __VA_ARGS__); } while (0)
    RP("report %d: %d level frames from f%d | per frame: wall %.2f ms, nativeRender %.2f ms, swap %.2f ms | tracked calls %.0f/frame\n",
       rep, frames, first_frame, wall_us / 1000.0 / fr, rend_us / 1000.0 / fr, swap_us / 1000.0 / fr, (double)calls / fr);
    RP("profiler cost per tracked call: %u ns total, %u ns of it inside the callee's own time | %d functions pruned, %u stale frames\n",
       (unsigned)g_cal_total_ns, (unsigned)g_cal_inside_ns, np, g_lost);
    g_sort_base = g_st;
    qsort(g_ord, (size_t)m, sizeof(uint16_t), zw_prof_cmp_excl);
    RP("\n== by exclusive time ==\n excl_ms  incl_ms  calls/f  kids/f  name\n");
    for (int i = 0; i < m && i < 260; i++) { const zw_pstat *s = &g_st[g_ord[i]];
        RP("%8.3f %8.3f %8.1f %7.1f  %s\n", s->excl / 1000.0 / fr, s->incl / 1000.0 / fr, s->calls / fr, s->kids / fr, s->name); }
    qsort(g_ord, (size_t)m, sizeof(uint16_t), zw_prof_cmp_incl);
    RP("\n== by inclusive time ==\n excl_ms  incl_ms  calls/f  kids/f  name\n");
    for (int i = 0; i < m && i < 160; i++) { const zw_pstat *s = &g_st[g_ord[i]];
        RP("%8.3f %8.3f %8.1f %7.1f  %s\n", s->excl / 1000.0 / fr, s->incl / 1000.0 / fr, s->calls / fr, s->kids / fr, s->name); }
    m = 0;
    for (int i = 0; i < g_nst; i++) if (g_st[i].flags & 1) g_ord[m++] = (uint16_t)i;
    qsort(g_ord, (size_t)m, sizeof(uint16_t), zw_prof_cmp_prune);
    RP("\n== pruned (cheap + frequent; calls in the frame they were pruned) ==\n");
    for (int i = 0; i < m && i < 220; i++) RP("%7u  %s\n", (unsigned)g_st[g_ord[i]].prune_calls, g_st[g_ord[i]].name);
#undef RP
    char path[96];
    sceIoMkdir("ux0:data/zombiewood/prof", 0777);
    snprintf(path, sizeof(path), "ux0:data/zombiewood/prof/prof_%02d.txt", rep);
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) { sceIoWrite(fd, g_rep, (SceSize)n); sceIoClose(fd); }
}

/* once per frame from the main loop, after the swap */
void zw_prof_frame(int frame, int in_level, uint32_t rend_us, uint32_t swap_us) {
    static int level_frames, win_frames, win_first, reports, calibrated;
    static uint64_t win_rend, win_swap, win_t0;
    if (!g_on) return;
    if (!calibrated) {
        calibrated = 1;
        void (*viastub)(void) = (void (*)(void))(uintptr_t)(zw_prof_stubs + g_dummy_stat * 8);
        void (*volatile direct)(void) = zw_prof_dummy;
        uint64_t t0 = sceKernelGetProcessTimeWide();
        for (int i = 0; i < 20000; i++) viastub();
        uint64_t t1 = sceKernelGetProcessTimeWide();
        for (int i = 0; i < 20000; i++) direct();
        uint64_t t2 = sceKernelGetProcessTimeWide();
        g_cal_total_ns = (uint32_t)(((t1 - t0) - (t2 - t1)) * 1000 / 20000);
        g_cal_inside_ns = (uint32_t)((uint64_t)g_st[g_dummy_stat].excl * 1000 / 20000);
        PLOG("  prof: cost per tracked call %u ns (%u ns inside)\n", (unsigned)g_cal_total_ns, (unsigned)g_cal_inside_ns);
        memset(&g_st[g_dummy_stat].incl, 0, 5 * sizeof(uint32_t)); g_st[g_dummy_stat].fcalls = 0;
    }
    /* v5.16 spike mode: a level frame much slower than the running average lists the functions
     * whose inclusive time this frame exceeds their own running average the most */
    {   static float ema[ZW_PROF_MAXSTAT]; static float ema_rend; static int nspk, warm;
        static uint16_t top[16]; static float topd[16];
        int spike = in_level && warm > 120 && ema_rend > 0 && rend_us > ema_rend * 1.5f && rend_us > ema_rend + 12000 && nspk < 60;
        if (spike) {
            int nt = 0;
            for (int i = 0; i < g_nst; i++) {
                float d = (float)g_st[i].fincl - ema[i];
                if (d < 1500.0f) continue;
                if (nt == 16 && d <= topd[15]) continue;
                int k = nt < 16 ? nt++ : 15;
                while (k > 0 && topd[k - 1] < d) { top[k] = top[k - 1]; topd[k] = topd[k - 1]; k--; }
                top[k] = (uint16_t)i; topd[k] = d;
            }
            nspk++;
            PLOG("  v5.16 prof spike f%d rend %u ms (avg %.0f)\n", frame, (unsigned)(rend_us / 1000), ema_rend / 1000.0f);
            for (int k = 0; k < nt; k++) { const zw_pstat *s = &g_st[top[k]];
                char b[400]; snprintf(b, sizeof(b), "    +%.1f ms incl %.1f (avg %.1f) excl %.1f calls %u  %s\n", topd[k] / 1000.0f, s->fincl / 1000.0f, ema[top[k]] / 1000.0f, s->fexcl / 1000.0f, (unsigned)s->fcalls, s->name);
                zw_log_raw(b); }
        }
        if (in_level) {
            warm++;
            if (!spike) { ema_rend += ((float)rend_us - ema_rend) * 0.05f;
                for (int i = 0; i < g_nst; i++) ema[i] += ((float)g_st[i].fincl - ema[i]) * 0.05f; }
        } else { warm = 0; ema_rend = 0; }
        for (int i = 0; i < g_nst; i++) g_st[i].fincl = 0;
    }
    int pruned = 0;
    for (int i = 0; i < g_nst; i++) {
        zw_pstat *s = &g_st[i];
        if (!s->fcalls) continue;
        if (s->fcalls > ZW_PROF_PRUNE_CALLS && !(s->flags & 3) && s->fexcl < (uint32_t)s->fcalls * ZW_PROF_PRUNE_US) { zw_prof_prune(i); pruned++; }
        s->fcalls = 0; s->fexcl = 0;
    }
    if (pruned) so_flush_caches(g_mod);
    if (!in_level) { level_frames = 0; win_frames = 0; return; }
    if (++level_frames <= ZW_PROF_WARMUP || reports >= ZW_PROF_MAXREP) return;
    if (!win_frames) {   /* start a window: this frame's numbers are dropped */
        for (int i = 0; i < g_nst; i++) { g_st[i].incl = g_st[i].excl = g_st[i].calls = g_st[i].kids = 0; }
        win_rend = win_swap = 0; win_first = frame + 1; win_t0 = sceKernelGetProcessTimeWide(); win_frames = 1;
        return;
    }
    win_rend += rend_us; win_swap += swap_us;
    if (win_frames++ >= ZW_PROF_WINDOW) {
        zw_prof_report(reports++, ZW_PROF_WINDOW, win_rend, win_swap, sceKernelGetProcessTimeWide() - win_t0, win_first);
        win_frames = 0;
    }
}

/* ── trampolines ── */
__asm__(
"   .pushsection .text\n"
"   .arm\n"
"   .align 3\n"
"   .global zw_prof_stubs\n"
"   .global zw_prof_site\n"
"zw_prof_stubs:\n"
"   .rept 16384\n"
"   mov   ip, pc\n"
"   b     zw_prof_vcommon\n"
"   .endr\n"
"zw_prof_vcommon:\n"
"   push  {r0-r3, ip, lr}\n"
"   sub   r0, ip, #8\n"
"   mov   r1, lr\n"
"   add   r2, sp, #24\n"
"   bl    zw_prof_enter_stub\n"
"   b     zw_prof_tail\n"
"   .type zw_prof_site, %function\n"
"zw_prof_site:\n"
"   push  {r0-r3, ip, lr}\n"
"   mov   r0, lr\n"
"   add   r1, sp, #24\n"
"   bl    zw_prof_enter_site\n"
"zw_prof_tail:\n"
"   mov   ip, r0\n"
"   cmp   r1, #0\n"
"   pop   {r0-r3}\n"
"   ldr   lr, [sp, #4]\n"
"   add   sp, sp, #8\n"
"   adrne lr, zw_prof_ret\n"
"   bx    ip\n"
"zw_prof_ret:\n"
"   push  {r0-r3}\n"
"   add   r0, sp, #16\n"
"   bl    zw_prof_exit\n"
"   mov   lr, r0\n"
"   pop   {r0-r3}\n"
"   bx    lr\n"
"   .popsection\n"
);

#endif /* ZW_CALLPROF */
