/* zw_glq.c - v5.15: GL command queue + GL thread.
 *
 * Measured (v5.14, busy level): the game thread spends 34 ms per frame, about 10 ms of it
 * inside VitaGL (draw calls, uniform uploads, vertex copies), while two CPU cores idle.
 * With the queue on, the game's GL calls are only RECORDED on the game thread; a second
 * thread replays them through the loader's existing GL wrappers (dynlib.c) and VitaGL, one
 * frame behind. The game thread starts its next frame while the previous one is drawn.
 *
 * How a call is handled (decided by name when the import table is patched):
 *  - plain state calls (<= 6 integer args, softfp floats included): recorded as is;
 *  - calls that pass data by pointer (uniform arrays, buffer data): the data is copied into
 *    the frame buffer and the pointer is redirected to the copy;
 *  - draw calls: client-side vertex arrays and indices are copied (the game may overwrite
 *    them before the GL thread reads them) and the attribute pointers re-issued to the copy;
 *  - everything else (anything that returns a value, creates objects, uploads textures,
 *    compiles shaders): the queue is drained and the call runs directly, as before.
 * Anything the recorder cannot describe safely also takes that last path, so the fallback
 * is always the old behaviour.
 *
 * ZW_GLQ 0 compiles the queue out of use (zw_glq_install does nothing). */
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/cpu.h>
#include <kubridge.h>
#include <vitaGL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "so_util.h"

#ifndef ZW_GLQ
#define ZW_GLQ 0   /* v5.15 work in progress: measured SLOWER than direct (see HANDOFF_v5.15.md); 1 = on */
#endif

extern void zw_log_raw(const char *s);
#define QLOG(...) do { char _b[256]; snprintf(_b, sizeof(_b), __VA_ARGS__); zw_log_raw(_b); } while (0)

typedef uintptr_t (*glq_fn6)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef uintptr_t (*glq_fn10)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

#define GLQ_BUF_BYTES (2u * 1024u * 1024u)
#define GLQ_MAXW 64
#define GLQ_OP_CALL 1u
#define GLQ_OP_CB   2u

typedef struct { uint32_t *base; uint32_t words; uint32_t n; uint32_t blob; } glq_buf;   /* commands grow up from 0, blobs down from the end */
static glq_buf g_buf[2]; static int g_rec;          /* buffer being recorded */
static volatile glq_buf *g_play;
static SceUID g_sem_work = -1, g_sem_idle = -1, g_thread = -1;
static volatile int g_on;                           /* recording active */
static int g_ready;
static uint32_t g_game_lo, g_game_size;   /* game thread stack: cheap "am I the game thread" test */
static glq_fn6 g_plain_orig[GLQ_MAXW]; static int g_nplain;
static glq_fn10 g_sync_orig[GLQ_MAXW]; static int g_nsync;
static const char *g_sync_name[GLQ_MAXW];
unsigned g_zw_glq_sync_n, g_zw_glq_sync_fallback_draws, g_zw_glq_overflow, g_zw_glq_calls, g_zw_glq_blob_bytes;
uint64_t g_zw_glq_wait_us, g_zw_glq_play_us, g_zw_glq_syncwait_us;
/* why draws fell back to direct: 0 ebuf+client arrays, 1 client indices ptr<64K, 2 max index too big / no room,
 * 3 DrawArrays no room; plus the last such draw for the log */
static unsigned g_dd_why[4]; static uint32_t g_dd_last[6];
static int g_game_cpu = -1, g_gl_cpu = -1; static volatile int g_gl_cpu_seen = -1;
static unsigned g_sync_by[GLQ_MAXW];

/* originals of the calls with their own recorder */
static glq_fn6 o_VertexAttribPointer, o_BindBuffer, o_EnableVAA, o_DisableVAA, o_DrawElements, o_DrawArrays,
    o_Uniform1fv, o_Uniform2fv, o_Uniform3fv, o_Uniform4fv, o_Uniform1iv, o_Uniform2iv, o_Uniform3iv, o_Uniform4iv,
    o_UniformMatrix4fv, o_BufferData, o_BufferSubData, o_GetError;

/* shadow of the vertex array state, kept on the game thread */
static struct { const void *ptr; GLint sz; GLenum ty; GLsizei st; GLboolean nrm; GLuint vbo; } s_attr[16];
static unsigned s_enabled; static GLuint s_abuf, s_ebuf;

/* ── queue plumbing ── */
static void glq_run(glq_buf *b) {
    uint32_t *p = b->base, *e = b->base + b->n;
    while (p < e) {
        uint32_t h = *p++;
        if ((h >> 24) == GLQ_OP_CALL) { ((glq_fn6)(uintptr_t)p[0])(p[1], p[2], p[3], p[4], p[5], p[6]); p += 7; }
        else { ((void (*)(uintptr_t))(uintptr_t)p[0])(p[1]); p += 2; }
    }
}
static int glq_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    for (;;) {
        sceKernelWaitSema(g_sem_work, 1, NULL);
        glq_buf *b = (glq_buf *)g_play;
        uint64_t t0 = sceKernelGetProcessTimeWide();
        if (b) glq_run(b);
        g_gl_cpu_seen = sceKernelGetCpuId();
        g_zw_glq_play_us += sceKernelGetProcessTimeWide() - t0;
        g_play = NULL;
        sceKernelSignalSema(g_sem_idle, 1);
    }
    return 0;
}
/* hand the recorded buffer to the GL thread (waits first if it is still drawing the previous one) */
static void glq_submit(void) {
    glq_buf *b = &g_buf[g_rec];
    if (!b->n) return;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    sceKernelWaitSema(g_sem_idle, 1, NULL);
    g_zw_glq_wait_us += sceKernelGetProcessTimeWide() - t0;
    g_play = b;
    g_rec ^= 1;
    g_buf[g_rec].n = 0; g_buf[g_rec].blob = g_buf[g_rec].words;
    sceKernelSignalSema(g_sem_work, 1);
}
static void glq_wait_idle(void) {
    sceKernelWaitSema(g_sem_idle, 1, NULL);
    sceKernelSignalSema(g_sem_idle, 1);
}
/* everything recorded so far has been executed when this returns */
void zw_glq_sync(void) {
    if (!g_ready) return;
    if (g_on) glq_submit();
    uint64_t t0 = sceKernelGetProcessTimeWide();
    glq_wait_idle();
    g_zw_glq_syncwait_us += sceKernelGetProcessTimeWide() - t0;
}
static inline int glq_recording(void) { int here; return g_on && (uint32_t)((uintptr_t)&here - g_game_lo) < g_game_size; }
/* make room for one complete command sequence (its data copies + its calls) so that it never
 * straddles two buffers; 0 = does not fit even in an empty buffer */
static int glq_reserve(uint32_t bytes) {
    glq_buf *b = &g_buf[g_rec];
    uint32_t need = ((bytes + 3) >> 2) + 512;
    if (need > b->words) return 0;
    if (b->n + need > b->blob) { g_zw_glq_overflow++; glq_submit(); }
    return 1;
}
static uint32_t *glq_cmd(uint32_t words) {
    glq_buf *b = &g_buf[g_rec];   /* space was reserved by the caller */
    uint32_t *p = b->base + b->n; b->n += words;
    return p;
}
static void *glq_blob(const void *src, uint32_t bytes) {
    uint32_t w = (bytes + 3) >> 2;
    glq_buf *b = &g_buf[g_rec];
    if (b->n + w + 64 > b->blob) return NULL;
    b->blob -= w;
    void *d = b->base + b->blob;
    if (src) memcpy(d, src, bytes);
    g_zw_glq_blob_bytes += bytes;
    return d;
}
static void glq_call(glq_fn6 fn, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5) {
    uint32_t *p = glq_cmd(8);
    p[0] = GLQ_OP_CALL << 24; p[1] = (uint32_t)(uintptr_t)fn;
    p[2] = a0; p[3] = a1; p[4] = a2; p[5] = a3; p[6] = a4; p[7] = a5;
    g_zw_glq_calls++;
}
/* run fn(arg) on the GL thread, in order with the recorded calls (directly if the queue is off) */
void zw_glq_callback(void (*fn)(uintptr_t), uintptr_t arg) {
    if (!glq_recording()) { fn(arg); return; }
    glq_reserve(0);
    uint32_t *p = glq_cmd(3);
    p[0] = GLQ_OP_CB << 24; p[1] = (uint32_t)(uintptr_t)fn; p[2] = arg;
}
/* end of a frame: everything recorded goes to the GL thread */
void zw_glq_frame_submit(void) { if (g_on) glq_submit(); }
int zw_glq_active(void) { return g_on; }
/* switch recording on/off at a frame boundary (off = every call direct again) */
void zw_glq_set(int on) {
    if (!g_ready || on == g_on) return;
    if (!on) {
        zw_glq_sync(); g_on = 0;
        /* the wrappers below still hold pointers into the last frame's copies: give them the game's own again */
        for (int i = 0; i < 16; i++)
            if ((s_enabled & (1u << i)) && !s_attr[i].vbo && (uintptr_t)s_attr[i].ptr >= 0x10000u) {
                if (s_abuf) o_BindBuffer(GL_ARRAY_BUFFER, 0, 0, 0, 0, 0);
                o_VertexAttribPointer((uintptr_t)i, (uintptr_t)s_attr[i].sz, s_attr[i].ty, s_attr[i].nrm, (uintptr_t)s_attr[i].st, (uintptr_t)s_attr[i].ptr);
                if (s_abuf) o_BindBuffer(GL_ARRAY_BUFFER, s_abuf, 0, 0, 0, 0);
            }
    }
    else {
        extern volatile int g_zw_vcache_on; g_zw_vcache_on = 0;
        /* both threads were created with affinity 0, so the GL thread could share the game thread's
         * core: pin the game thread where it is and the GL thread to another user core */
        if (g_game_cpu < 0) {
            int c = sceKernelGetCpuId(); if (c < 0 || c > 2) c = 0;
            g_game_cpu = c; g_gl_cpu = c == 1 ? 2 : 1;
            sceKernelChangeThreadCpuAffinityMask(0, 0x10000 << g_game_cpu);
            sceKernelChangeThreadCpuAffinityMask(g_thread, 0x10000 << g_gl_cpu);
        }
        g_on = 1;
    }
}

/* ── generic recorders ── */
#define GLQ_PLAIN(k) static uintptr_t glq_p##k(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5) { \
    if (!glq_recording()) return g_plain_orig[k](a0, a1, a2, a3, a4, a5); \
    glq_reserve(0); glq_call(g_plain_orig[k], a0, a1, a2, a3, a4, a5); return 0; }
#define GLQ_SYNC(k) static uintptr_t glq_s##k(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9) { \
    if (g_on) { zw_glq_sync(); g_zw_glq_sync_n++; g_sync_by[k]++; } \
    return g_sync_orig[k](a0, a1, a2, a3, a4, a5, a6, a7, a8, a9); }
#define GLQ_X16(M, b) M(b##0) M(b##1) M(b##2) M(b##3) M(b##4) M(b##5) M(b##6) M(b##7) M(b##8) M(b##9)
GLQ_PLAIN(0) GLQ_PLAIN(1) GLQ_PLAIN(2) GLQ_PLAIN(3) GLQ_PLAIN(4) GLQ_PLAIN(5) GLQ_PLAIN(6) GLQ_PLAIN(7) GLQ_PLAIN(8) GLQ_PLAIN(9)
GLQ_X16(GLQ_PLAIN, 1) GLQ_X16(GLQ_PLAIN, 2) GLQ_X16(GLQ_PLAIN, 3) GLQ_X16(GLQ_PLAIN, 4) GLQ_X16(GLQ_PLAIN, 5)
GLQ_PLAIN(60) GLQ_PLAIN(61) GLQ_PLAIN(62) GLQ_PLAIN(63)
GLQ_SYNC(0) GLQ_SYNC(1) GLQ_SYNC(2) GLQ_SYNC(3) GLQ_SYNC(4) GLQ_SYNC(5) GLQ_SYNC(6) GLQ_SYNC(7) GLQ_SYNC(8) GLQ_SYNC(9)
GLQ_X16(GLQ_SYNC, 1) GLQ_X16(GLQ_SYNC, 2) GLQ_X16(GLQ_SYNC, 3) GLQ_X16(GLQ_SYNC, 4) GLQ_X16(GLQ_SYNC, 5)
GLQ_SYNC(60) GLQ_SYNC(61) GLQ_SYNC(62) GLQ_SYNC(63)
#define GLQ_REF10(pre, b) pre##b##0, pre##b##1, pre##b##2, pre##b##3, pre##b##4, pre##b##5, pre##b##6, pre##b##7, pre##b##8, pre##b##9
static const glq_fn6 g_plain_w[GLQ_MAXW] = { glq_p0, glq_p1, glq_p2, glq_p3, glq_p4, glq_p5, glq_p6, glq_p7, glq_p8, glq_p9,
    GLQ_REF10(glq_p, 1), GLQ_REF10(glq_p, 2), GLQ_REF10(glq_p, 3), GLQ_REF10(glq_p, 4), GLQ_REF10(glq_p, 5), glq_p60, glq_p61, glq_p62, glq_p63 };
static const glq_fn10 g_sync_w[GLQ_MAXW] = { glq_s0, glq_s1, glq_s2, glq_s3, glq_s4, glq_s5, glq_s6, glq_s7, glq_s8, glq_s9,
    GLQ_REF10(glq_s, 1), GLQ_REF10(glq_s, 2), GLQ_REF10(glq_s, 3), GLQ_REF10(glq_s, 4), GLQ_REF10(glq_s, 5), glq_s60, glq_s61, glq_s62, glq_s63 };

/* ── calls with their own recorder ── */
static uintptr_t r_BindBuffer(uintptr_t t, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) {
    if (t == GL_ARRAY_BUFFER) s_abuf = (GLuint)b; else if (t == GL_ELEMENT_ARRAY_BUFFER) s_ebuf = (GLuint)b;
    if (!glq_recording()) return o_BindBuffer(t, b, c, d, e, f);
    glq_reserve(0); glq_call(o_BindBuffer, t, b, 0, 0, 0, 0); return 0;
}
static uintptr_t r_EnableVAA(uintptr_t i, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) {
    if (i < 16) s_enabled |= 1u << i;
    if (!glq_recording()) return o_EnableVAA(i, b, c, d, e, f);
    glq_reserve(0); glq_call(o_EnableVAA, i, 0, 0, 0, 0, 0); return 0;
}
static uintptr_t r_DisableVAA(uintptr_t i, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) {
    if (i < 16) s_enabled &= ~(1u << i);
    if (!glq_recording()) return o_DisableVAA(i, b, c, d, e, f);
    glq_reserve(0); glq_call(o_DisableVAA, i, 0, 0, 0, 0, 0); return 0;
}
static uintptr_t r_VertexAttribPointer(uintptr_t i, uintptr_t sz, uintptr_t ty, uintptr_t nrm, uintptr_t st, uintptr_t ptr) {
    if (i < 16) { s_attr[i].ptr = (const void *)ptr; s_attr[i].sz = (GLint)sz; s_attr[i].ty = (GLenum)ty; s_attr[i].st = (GLsizei)st; s_attr[i].nrm = (GLboolean)nrm; s_attr[i].vbo = s_abuf; }
    if (!glq_recording()) return o_VertexAttribPointer(i, sz, ty, nrm, st, ptr);
    if (s_abuf || i >= 16) { glq_reserve(0); glq_call(o_VertexAttribPointer, i, sz, ty, nrm, st, ptr); }   /* VBO offset: safe to replay later */
    /* client pointer: issued with each draw, pointing at that draw's copy */
    return 0;
}
static unsigned glq_type_size(GLenum ty) {
    switch (ty) { case GL_BYTE: case GL_UNSIGNED_BYTE: return 1; case GL_SHORT: case GL_UNSIGNED_SHORT: return 2; default: return 4; }
}
/* copy the client-side arrays the next draw reads (vertices 0..nverts-1) and point the attributes at
 * the copies. 0 = cannot be described; the caller runs the draw synchronously instead */
static int glq_emit_client_arrays(unsigned nverts) {
    const uint8_t *lo = NULL, *hi = NULL; int n = 0, one_block = 1; GLsizei st0 = -1;
    for (int i = 0; i < 16; i++) {
        if (!(s_enabled & (1u << i)) || s_attr[i].vbo) continue;
        const uint8_t *p = (const uint8_t *)s_attr[i].ptr;
        if ((uintptr_t)p < 0x10000u) continue;   /* never set: VitaGL would not read it either */
        unsigned el = (unsigned)s_attr[i].sz * glq_type_size(s_attr[i].ty);
        unsigned st = s_attr[i].st ? (unsigned)s_attr[i].st : el;
        const uint8_t *end = p + (size_t)(nverts - 1) * st + el;
        if (st0 < 0) st0 = (GLsizei)st; else if ((GLsizei)st != st0) one_block = 0;
        if (!lo || p < lo) lo = p;
        if (!hi || end > hi) hi = end;
        n++;
    }
    if (!n) return 1;
    if (one_block) for (int i = 0; i < 16; i++)   /* interleaved: every pointer within one stride of the lowest */
        if ((s_enabled & (1u << i)) && !s_attr[i].vbo && (uintptr_t)s_attr[i].ptr >= 0x10000u &&
            (const uint8_t *)s_attr[i].ptr - lo >= st0) { one_block = 0; break; }
    const uint8_t *copy = NULL;
    if (one_block) {
        size_t bytes = (size_t)(hi - lo);
        if (bytes > GLQ_BUF_BYTES / 3) return 0;
        copy = glq_blob(lo, (uint32_t)bytes);
        if (!copy) return 0;
    }
    if (s_abuf) glq_call(o_BindBuffer, GL_ARRAY_BUFFER, 0, 0, 0, 0, 0);
    for (int i = 0; i < 16; i++) {
        if (!(s_enabled & (1u << i)) || s_attr[i].vbo || (uintptr_t)s_attr[i].ptr < 0x10000u) continue;
        const uint8_t *p = (const uint8_t *)s_attr[i].ptr, *q;
        if (one_block) q = copy + (p - lo);
        else {
            unsigned el = (unsigned)s_attr[i].sz * glq_type_size(s_attr[i].ty);
            unsigned st = s_attr[i].st ? (unsigned)s_attr[i].st : el;
            size_t bytes = (size_t)(nverts - 1) * st + el;
            if (bytes > GLQ_BUF_BYTES / 3) return 0;
            q = glq_blob(p, (uint32_t)bytes);
            if (!q) return 0;
        }
        glq_call(o_VertexAttribPointer, (uintptr_t)i, (uintptr_t)s_attr[i].sz, s_attr[i].ty, s_attr[i].nrm, (uintptr_t)s_attr[i].st, (uintptr_t)q);
    }
    if (s_abuf) glq_call(o_BindBuffer, GL_ARRAY_BUFFER, s_abuf, 0, 0, 0, 0);
    return 1;
}
/* upper bound of what glq_emit_client_arrays(nverts) copies */
static uint32_t glq_client_bytes(unsigned nverts) {
    uint32_t t = 0;
    for (int i = 0; i < 16; i++) {
        if (!(s_enabled & (1u << i)) || s_attr[i].vbo || (uintptr_t)s_attr[i].ptr < 0x10000u) continue;
        unsigned el = (unsigned)s_attr[i].sz * glq_type_size(s_attr[i].ty);
        unsigned st = s_attr[i].st ? (unsigned)s_attr[i].st : el;
        t += nverts * st + el + 4;
    }
    return t;
}
static int glq_has_client_arrays(void) {
    for (int i = 0; i < 16; i++) if ((s_enabled & (1u << i)) && !s_attr[i].vbo && (uintptr_t)s_attr[i].ptr >= 0x10000u) return 1;
    return 0;
}
/* CPU copies of small element buffers (game thread only), so a draw with indices in a VBO and
 * client-side vertices can find its vertex range without a drain */
#define GLQ_ESH_N 512
#define GLQ_ESH_MAX (64u * 1024u)
static struct { uint32_t name; uint32_t size; uint8_t *data; } s_esh[GLQ_ESH_N];
static inline int glq_on_game_thread(void) { int here; return (uint32_t)((uintptr_t)&here - g_game_lo) < g_game_size; }
static int glq_esh_find(uint32_t name, int add) {
    unsigned h = (name * 2654435761u) >> 23;   /* 9 bits */
    for (unsigned k = 0; k < GLQ_ESH_N; k++, h = (h + 1) & (GLQ_ESH_N - 1)) {
        if (s_esh[h].name == name) return (int)h;
        if (!s_esh[h].name) { if (!add) return -1; s_esh[h].name = name; return (int)h; }
    }
    return -1;
}
static void glq_esh_data(uintptr_t size, const void *data) {
    if (!s_ebuf || !glq_on_game_thread()) return;
    int h = glq_esh_find(s_ebuf, size && size <= GLQ_ESH_MAX);
    if (h < 0) return;
    if (!size || size > GLQ_ESH_MAX) { s_esh[h].size = 0; return; }   /* too big: forget it, draws fall back */
    if (s_esh[h].size != size) { free(s_esh[h].data); s_esh[h].data = malloc(size); s_esh[h].size = s_esh[h].data ? (uint32_t)size : 0; }
    if (s_esh[h].size) { if (data) memcpy(s_esh[h].data, data, size); else memset(s_esh[h].data, 0, size); }
}
static void glq_esh_sub(uintptr_t off, uintptr_t size, const void *data) {
    if (!s_ebuf || !data || !glq_on_game_thread()) return;
    int h = glq_esh_find(s_ebuf, 0);
    if (h < 0 || !s_esh[h].size) return;
    if (off + size > s_esh[h].size) { s_esh[h].size = 0; return; }
    memcpy(s_esh[h].data + off, data, size);
}
static uintptr_t r_DrawElements(uintptr_t mode, uintptr_t count, uintptr_t type, uintptr_t indices, uintptr_t e, uintptr_t f) {
    if (!glq_recording()) return o_DrawElements(mode, count, type, indices, e, f);
    if ((int)count <= 0) return 0;
    unsigned isz = type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_INT ? 4 : 1;
    const void *idx = (const void *)indices;
    int ok = 1, why = 2;
    if (!s_ebuf) {
        if (indices < 0x10000u) { ok = 0; why = 1; }
        else {
            unsigned mx = 0;
            if (glq_has_client_arrays()) {
                if (isz == 2) { const uint16_t *ix = idx; for (unsigned k = 0; k < count; k++) if (ix[k] > mx) mx = ix[k]; }
                else if (isz == 4) { const uint32_t *ix = idx; for (unsigned k = 0; k < count; k++) if (ix[k] > mx) mx = ix[k]; }
                else { const uint8_t *ix = idx; for (unsigned k = 0; k < count; k++) if (ix[k] > mx) mx = ix[k]; }
                ok = mx < 0x100000u && glq_reserve(glq_client_bytes(mx + 1) + (uint32_t)count * isz) && glq_emit_client_arrays(mx + 1);
            } else ok = glq_reserve((uint32_t)count * isz);
            if (ok) { idx = glq_blob(idx, (uint32_t)count * isz); if (!idx) ok = 0; }
        }
    } else if (glq_has_client_arrays()) {   /* indices in a VBO, vertices not: range from the CPU copy */
        int h = glq_esh_find(s_ebuf, 0);
        if (h < 0 || !s_esh[h].size || indices + (uintptr_t)count * isz > s_esh[h].size) { ok = 0; why = 0; }
        else {
            unsigned mx = 0; const void *sp = s_esh[h].data + indices;
            if (isz == 2) { const uint16_t *ix = sp; for (unsigned k = 0; k < count; k++) if (ix[k] > mx) mx = ix[k]; }
            else if (isz == 4) { const uint32_t *ix = sp; for (unsigned k = 0; k < count; k++) if (ix[k] > mx) mx = ix[k]; }
            else { const uint8_t *ix = sp; for (unsigned k = 0; k < count; k++) if (ix[k] > mx) mx = ix[k]; }
            ok = mx < 0x100000u && glq_reserve(glq_client_bytes(mx + 1)) && glq_emit_client_arrays(mx + 1);
            if (!ok) why = 0;
        }
    }
    else glq_reserve(0);
    if (!ok) {   /* the old way: drain the queue, draw directly from the game's memory */
        g_dd_why[why]++;
        g_dd_last[0] = mode; g_dd_last[1] = count; g_dd_last[2] = type; g_dd_last[3] = indices; g_dd_last[4] = s_ebuf; g_dd_last[5] = s_enabled | (s_abuf << 16);
        zw_glq_sync(); g_zw_glq_sync_fallback_draws++;
        for (int i = 0; i < 16; i++)
            if ((s_enabled & (1u << i)) && !s_attr[i].vbo) {
                if (s_abuf) o_BindBuffer(GL_ARRAY_BUFFER, 0, 0, 0, 0, 0);
                o_VertexAttribPointer((uintptr_t)i, (uintptr_t)s_attr[i].sz, s_attr[i].ty, s_attr[i].nrm, (uintptr_t)s_attr[i].st, (uintptr_t)s_attr[i].ptr);
                if (s_abuf) o_BindBuffer(GL_ARRAY_BUFFER, s_abuf, 0, 0, 0, 0);
            }
        return o_DrawElements(mode, count, type, indices, 0, 0);
    }
    glq_call(o_DrawElements, mode, count, type, (uintptr_t)idx, 0, 0);
    return 0;
}
static uintptr_t r_DrawArrays(uintptr_t mode, uintptr_t first, uintptr_t count, uintptr_t d, uintptr_t e, uintptr_t f) {
    if (!glq_recording()) return o_DrawArrays(mode, first, count, d, e, f);
    if ((int)count <= 0 || (int)first < 0) return 0;
    if (!glq_reserve(glq_client_bytes((unsigned)first + (unsigned)count)) || !glq_emit_client_arrays((unsigned)first + (unsigned)count)) {
        g_dd_why[3]++;
        g_dd_last[0] = mode; g_dd_last[1] = count; g_dd_last[2] = first; g_dd_last[3] = 0; g_dd_last[4] = s_ebuf; g_dd_last[5] = s_enabled | (s_abuf << 16);
        zw_glq_sync(); g_zw_glq_sync_fallback_draws++;
        for (int i = 0; i < 16; i++)
            if ((s_enabled & (1u << i)) && !s_attr[i].vbo) {
                if (s_abuf) o_BindBuffer(GL_ARRAY_BUFFER, 0, 0, 0, 0, 0);
                o_VertexAttribPointer((uintptr_t)i, (uintptr_t)s_attr[i].sz, s_attr[i].ty, s_attr[i].nrm, (uintptr_t)s_attr[i].st, (uintptr_t)s_attr[i].ptr);
                if (s_abuf) o_BindBuffer(GL_ARRAY_BUFFER, s_abuf, 0, 0, 0, 0);
            }
        return o_DrawArrays(mode, first, count, 0, 0, 0);
    }
    glq_call(o_DrawArrays, mode, first, count, 0, 0, 0);
    return 0;
}
/* glUniformNfv / Niv (loc, n, ptr): copy n * comps words */
static uintptr_t glq_unif(glq_fn6 orig, unsigned comps, uintptr_t loc, uintptr_t n, uintptr_t ptr) {
    if (!glq_recording()) return orig(loc, n, ptr, 0, 0, 0);
    if (!ptr || (int)n <= 0 || n > 1024) return 0;
    void *c = glq_reserve((uint32_t)n * comps * 4) ? glq_blob((const void *)ptr, (uint32_t)n * comps * 4) : NULL;
    if (!c) { zw_glq_sync(); return orig(loc, n, ptr, 0, 0, 0); }
    glq_call(orig, loc, n, (uintptr_t)c, 0, 0, 0);
    return 0;
}
#define GLQ_UNIF(name, comps) static uintptr_t r_##name(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) { (void)d; (void)e; (void)f; return glq_unif(o_##name, comps, a, b, c); }
GLQ_UNIF(Uniform1fv, 1) GLQ_UNIF(Uniform2fv, 2) GLQ_UNIF(Uniform3fv, 3) GLQ_UNIF(Uniform4fv, 4)
GLQ_UNIF(Uniform1iv, 1) GLQ_UNIF(Uniform2iv, 2) GLQ_UNIF(Uniform3iv, 3) GLQ_UNIF(Uniform4iv, 4)
static uintptr_t r_UniformMatrix4fv(uintptr_t loc, uintptr_t n, uintptr_t tp, uintptr_t ptr, uintptr_t e, uintptr_t f) {
    if (!glq_recording()) return o_UniformMatrix4fv(loc, n, tp, ptr, e, f);
    if (!ptr || (int)n <= 0 || n > 256) return 0;
    void *c = glq_reserve((uint32_t)n * 64) ? glq_blob((const void *)ptr, (uint32_t)n * 64) : NULL;
    if (!c) { zw_glq_sync(); return o_UniformMatrix4fv(loc, n, tp, ptr, 0, 0); }
    glq_call(o_UniformMatrix4fv, loc, n, tp, (uintptr_t)c, 0, 0);
    return 0;
}
static uintptr_t r_BufferData(uintptr_t target, uintptr_t size, uintptr_t data, uintptr_t usage, uintptr_t e, uintptr_t f) {
    if (target == GL_ELEMENT_ARRAY_BUFFER) glq_esh_data(size, (const void *)data);
    if (!glq_recording()) return o_BufferData(target, size, data, usage, e, f);
    void *c = NULL;
    if (data && (int)size > 0) { c = (size <= GLQ_BUF_BYTES / 4 && glq_reserve((uint32_t)size)) ? glq_blob((const void *)data, (uint32_t)size) : NULL;
        if (!c) { zw_glq_sync(); g_zw_glq_sync_n++; return o_BufferData(target, size, data, usage, 0, 0); } }
    if (!c) glq_reserve(0);
    glq_call(o_BufferData, target, size, (uintptr_t)c, usage, 0, 0);
    return 0;
}
static uintptr_t r_BufferSubData(uintptr_t target, uintptr_t off, uintptr_t size, uintptr_t data, uintptr_t e, uintptr_t f) {
    if (target == GL_ELEMENT_ARRAY_BUFFER) glq_esh_sub(off, size, (const void *)data);
    if (!glq_recording()) return o_BufferSubData(target, off, size, data, e, f);
    if (!data || (int)size <= 0) return 0;
    void *c = (size <= GLQ_BUF_BYTES / 4 && glq_reserve((uint32_t)size)) ? glq_blob((const void *)data, (uint32_t)size) : NULL;
    if (!c) { zw_glq_sync(); g_zw_glq_sync_n++; return o_BufferSubData(target, off, size, data, 0, 0); }
    glq_call(o_BufferSubData, target, off, size, (uintptr_t)c, 0, 0);
    return 0;
}
static uintptr_t r_GetError(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) {
    if (!glq_recording()) return o_GetError(a, b, c, d, e, f);
    return 0;   /* GL_NO_ERROR: the real state is one frame behind and not worth a full drain */
}

/* ── install ── */
static const struct { const char *name; glq_fn6 rec; glq_fn6 *orig; } g_special[] = {
    { "glVertexAttribPointer", r_VertexAttribPointer, &o_VertexAttribPointer }, { "glBindBuffer", r_BindBuffer, &o_BindBuffer },
    { "glEnableVertexAttribArray", r_EnableVAA, &o_EnableVAA }, { "glDisableVertexAttribArray", r_DisableVAA, &o_DisableVAA },
    { "glDrawElements", r_DrawElements, &o_DrawElements }, { "glDrawArrays", r_DrawArrays, &o_DrawArrays },
    { "glUniform1fv", r_Uniform1fv, &o_Uniform1fv }, { "glUniform2fv", r_Uniform2fv, &o_Uniform2fv },
    { "glUniform3fv", r_Uniform3fv, &o_Uniform3fv }, { "glUniform4fv", r_Uniform4fv, &o_Uniform4fv },
    { "glUniform1iv", r_Uniform1iv, &o_Uniform1iv }, { "glUniform2iv", r_Uniform2iv, &o_Uniform2iv },
    { "glUniform3iv", r_Uniform3iv, &o_Uniform3iv }, { "glUniform4iv", r_Uniform4iv, &o_Uniform4iv },
    { "glUniformMatrix4fv", r_UniformMatrix4fv, &o_UniformMatrix4fv },
    { "glBufferData", r_BufferData, &o_BufferData }, { "glBufferSubData", r_BufferSubData, &o_BufferSubData },
    { "glGetError", r_GetError, &o_GetError },
};
static const char *const g_plain_names[] = {
    "glActiveTexture", "glBindFramebuffer", "glBindTexture", "glBlendColor", "glBlendEquation", "glBlendFunc", "glClear",
    "glClearColor", "glClearDepthf", "glClearStencil", "glColorMask", "glCullFace", "glDepthFunc", "glDepthMask",
    "glDepthRangef", "glDisable", "glEnable", "glFlush", "glFrontFace", "glLineWidth", "glPolygonOffset",
    "glSampleCoverage", "glScissor", "glStencilFunc", "glStencilMask", "glStencilOp", "glTexParameterf", "glTexParameteri",
    "glUniform1f", "glUniform1i", "glUseProgram", "glVertexAttrib4f", "glViewport", NULL };

void zw_glq_install(so_module *mod) {
    if (!ZW_GLQ) return;
    uint32_t base = (uint32_t)mod->text_base;
    for (int i = 0; i < 2; i++) {
        g_buf[i].base = malloc(GLQ_BUF_BYTES);
        if (!g_buf[i].base) { QLOG("  v5.15 glq: no memory for the frame buffers, queue off\n"); return; }
        g_buf[i].words = GLQ_BUF_BYTES / 4; g_buf[i].n = 0; g_buf[i].blob = g_buf[i].words;
    }
    int nspec = 0;
    for (int i = 0; i < mod->num_relplt; i++) {
        Elf32_Rel *r = &mod->relplt[i];
        if (ELF32_R_TYPE(r->r_info) != R_ARM_JUMP_SLOT) continue;
        Elf32_Sym *s = &mod->dynsym[ELF32_R_SYM(r->r_info)];
        const char *n = mod->dynstr + s->st_name;
        uint32_t *slot = (uint32_t *)(uintptr_t)(base + r->r_offset);
        uint32_t v = *slot, w = 0;
        if (s->st_shndx || !v || n[0] != 'g' || n[1] != 'l' || n[2] < 'A' || n[2] > 'Z') continue;
        for (unsigned k = 0; k < sizeof(g_special) / sizeof(g_special[0]) && !w; k++)
            if (!strcmp(n, g_special[k].name)) { *g_special[k].orig = (glq_fn6)(uintptr_t)v; w = (uint32_t)(uintptr_t)g_special[k].rec; nspec++; }
        for (int k = 0; g_plain_names[k] && !w; k++)
            if (!strcmp(n, g_plain_names[k]) && g_nplain < GLQ_MAXW) { g_plain_orig[g_nplain] = (glq_fn6)(uintptr_t)v; w = (uint32_t)(uintptr_t)g_plain_w[g_nplain++]; }
        if (!w) {
            if (g_nsync >= GLQ_MAXW) { QLOG("  v5.15 glq: too many GL imports (%s), queue off\n", n); return; }
            g_sync_orig[g_nsync] = (glq_fn10)(uintptr_t)v; g_sync_name[g_nsync] = n; w = (uint32_t)(uintptr_t)g_sync_w[g_nsync++];
        }
        kuKernelCpuUnrestrictedMemcpy(slot, &w, 4);
    }
    if (nspec != (int)(sizeof(g_special) / sizeof(g_special[0]))) QLOG("  v5.15 glq: only %d special imports found\n", nspec);
    g_sem_work = sceKernelCreateSema("zw_glq_work", 0, 0, 1, NULL);
    g_sem_idle = sceKernelCreateSema("zw_glq_idle", 0, 1, 1, NULL);
    g_thread = sceKernelCreateThread("zw_gl", glq_thread, 0x10000100, 0x100000, 0, 0, NULL);
    if (g_sem_work < 0 || g_sem_idle < 0 || g_thread < 0) { QLOG("  v5.15 glq: thread setup failed, queue off\n"); return; }
    sceKernelStartThread(g_thread, 0, NULL);
    {   SceKernelThreadInfo ti; memset(&ti, 0, sizeof(ti)); ti.size = sizeof(ti);
        if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti) < 0 || !ti.stack) { QLOG("  v5.15 glq: no thread info, queue off\n"); return; }
        g_game_lo = (uint32_t)(uintptr_t)ti.stack; g_game_size = (uint32_t)ti.stackSize; }
    g_ready = 1;
    QLOG("  v5.15 glq: ready - %d recorded, %d with data, %d direct GL imports\n", g_nplain, nspec, g_nsync);
}
/* measurement: counters since the last call */
void zw_glq_stats_log(int frame, unsigned frames) {
    char buf[640]; int n = 0; double fr = frames ? (double)frames : 1.0;
    n += snprintf(buf, sizeof(buf), "  v5.15 glq %s f%d per frame: %.0f calls, %.0f KB copied, GL thread %.1f ms, game waited %.1f ms, drains %.2f, direct draws %.2f, overflows %u |",
                  g_on ? "ON" : "OFF", frame, g_zw_glq_calls / fr, g_zw_glq_blob_bytes / fr / 1024.0, g_zw_glq_play_us / 1000.0 / fr,
                  g_zw_glq_wait_us / 1000.0 / fr, g_zw_glq_sync_n / fr, g_zw_glq_sync_fallback_draws / fr, g_zw_glq_overflow);
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, " syncwait %.1f ms, cpu game %d gl %d, dd why %u/%u/%u/%u last %x %u %x %x eb%u m%x |",
                  g_zw_glq_syncwait_us / 1000.0 / fr, sceKernelGetCpuId(), g_gl_cpu_seen,
                  g_dd_why[0], g_dd_why[1], g_dd_why[2], g_dd_why[3],
                  (unsigned)g_dd_last[0], (unsigned)g_dd_last[1], (unsigned)g_dd_last[2], (unsigned)g_dd_last[3], (unsigned)g_dd_last[4], (unsigned)g_dd_last[5]);
    g_zw_glq_syncwait_us = 0; memset(g_dd_why, 0, sizeof(g_dd_why));
    for (int k = 0; k < g_nsync && n < (int)sizeof(buf) - 48; k++)
        if (g_sync_by[k]) { n += snprintf(buf + n, sizeof(buf) - (size_t)n, " %s %u", g_sync_name[k] + 2, g_sync_by[k]); g_sync_by[k] = 0; }
    snprintf(buf + n, sizeof(buf) - (size_t)n, "\n");
    zw_log_raw(buf);
    g_zw_glq_calls = g_zw_glq_blob_bytes = g_zw_glq_sync_n = g_zw_glq_sync_fallback_draws = g_zw_glq_overflow = 0;
    g_zw_glq_play_us = g_zw_glq_wait_us = 0;
}
