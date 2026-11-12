/* Auto-generated. Hook table for libZombieWood.so 356 PLT imports. */

#include <vitasdk.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <math.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <time.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <vitaGL.h>
#include "so_util.h"

struct iovec {
    void *iov_base;
    size_t iov_len;
};

extern FILE *vita_fopen(const char *path, const char *mode);
extern size_t vita_fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
extern size_t vita_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
extern int vita_fflush(FILE *stream);
extern int vita_fprintf(FILE *stream, const char *fmt, ...);
extern int vita_fputc(int c, FILE *stream);
extern int vita_putc(int c, FILE *stream);
extern int vita_fputs(const char *s, FILE *stream);
extern int vita_vfprintf(FILE *stream, const char *fmt, va_list ap);
extern int vita_fseek(FILE *stream, long offset, int whence);
extern long vita_ftell(FILE *stream);
extern int vita_fclose(FILE *stream);
extern int vita_feof(FILE *stream);
extern char *vita_fgets(char *s, int size, FILE *stream);
extern int vita_fgetc(FILE *stream);
extern int vita_getc(FILE *stream);
extern void vita_rewind(FILE *stream);
extern int vita_ungetc(int c, FILE *stream);
extern wint_t vita_getwc(FILE *stream);
extern wint_t vita_ungetwc(wint_t wc, FILE *stream);
extern void vita_clearerr(FILE *stream);
extern int vita_ferror(FILE *stream);
extern int vita_fileno(FILE *stream);
extern void vita_qsort(void *base, size_t nmemb, size_t size,
                       int (*compar)(const void *, const void *));
extern char *vita_strstr(const char *haystack, const char *needle);
extern int vita_open(const char *path, int flags, ...);
extern int vita_close(int fd);
extern const void *zw_SL_IID_ENGINE, *zw_SL_IID_PLAY, *zw_SL_IID_BUFFERQUEUE;   /* v4.99 */
extern int vita_stat(const char *path, void *st);
extern int vita_fstat(int fd, void *st);
extern DIR *vita_opendir(const char *path);
extern void *vita_readdir(DIR *dir);
extern int vita_remove(const char *path);
extern int vita_rename(const char *oldpath, const char *newpath);
extern int vita_unlink(const char *path);
extern int vita_mkdir(const char *path, mode_t mode);
extern char *vita_setlocale(int category, const char *locale);
extern unsigned int slCreateEngine(void *pEngine, unsigned int numOptions,
                                   const void *pEngineOptions,
                                   unsigned int numInterfaces,
                                   const void *pInterfaceIds,
                                   const unsigned char *pInterfaceRequired);

jmp_buf g_android_exit_jmp;
int g_android_exit_active = 0;
int g_android_exit_status = 0;
unsigned int g_gl_draw_arrays_count = 0;
unsigned int g_gl_draw_elements_count = 0;

static void dynlib_logf(const char *fmt, ...){
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

#ifdef VITA3K_BUILD
    sceClibPrintf("[ZW/DYNLIB] %s\n", buf);
#else
    /* v4.50: go through main.c's single log fd. Reopening the log in append
     * mode here raced main's own fd offset, so main overwrote these lines. */
    extern void zw_log_raw(const char *s);
    zw_log_raw("[ZW/DYNLIB] ");
    zw_log_raw(buf);
    zw_log_raw("\n");
#endif
}

/* v4.50: one-frame GL call trace, toggled by main.c's frame loop. */
volatile int g_zw_gl_trace = 0;
#define GLT(...) do { if (g_zw_gl_trace) dynlib_logf(__VA_ARGS__); } while (0)

static int  stub_int_neg1(void){ return -1; }
static int  stub_int_zero(void){ return 0; }
static void *stub_ptr_null(void){ return NULL; }
static void stub_void(void){ }

static int my_cxa_atexit(void (*func)(void *), void *arg, void *dso_handle){
    (void)func; (void)arg; (void)dso_handle; return 0;
}
static void my_cxa_finalize(void *dso_handle){ (void)dso_handle; }
/* ARM EABI uses a 4-byte guard variable. Reading/writing as uint64_t
   would overwrite 4 bytes past the guard — those 4 bytes are typically
   the start of the static-local storage (the constructed object), so
   a release would stomp the object's first member with 0.
   That's how we ended up with `glf::Debugger.mutex_ == NULL` and the
   crash at glf::DebuggerMutex::Lock with this == NULL.
   Use uint8_t* — only the low byte's bit 0 is the "initialized" flag. */
static void my_cxa_guard_abort(uint8_t *guard){ (void)guard; }
static int my_cxa_guard_acquire(uint8_t *guard){
    if (!guard) return 0;
    if (*guard) return 0;
    return 1;
}
static void my_cxa_guard_release(uint8_t *guard){
    if (guard) *guard = 1;
}
static uintptr_t my_stack_chk_guard = 0x00C0FFEE;
static FILE g_android_sF[3];

#ifdef VITA3K_BUILD
static void park_vita3k_fatal_thread(void) {
    dynlib_logf("Vita3K fatal path parked; close/restart the emulator for next run");
    for (;;) {
        sceKernelDelayThread(1000000);
    }
}
#endif

#define ANDROID_CTYPE_U 0x01
#define ANDROID_CTYPE_L 0x02
#define ANDROID_CTYPE_N 0x04
#define ANDROID_CTYPE_S 0x08
#define ANDROID_CTYPE_P 0x10
#define ANDROID_CTYPE_C 0x20
#define ANDROID_CTYPE_X 0x40
#define ANDROID_CTYPE_B 0x80

static unsigned char g_android_ctype[257];
static unsigned char *g_android_ctype_ptr = g_android_ctype;

/* v4.96: bionic `const short *_tolower_tab_/_toupper_tab_` (257 entries, index c+1,
 * [0] = EOF). They were missing from the import table, so the GOT held NULL and
 * gameswf::html_reader::parse crashed parsing a "#rrggbb" colour on the post-tutorial
 * screen (tolower() inlined as _tolower_tab_[c+1]). */
static short g_android_tolower_tab[257], g_android_toupper_tab[257];
static const short *g_android_tolower_tab_ptr = g_android_tolower_tab;
static const short *g_android_toupper_tab_ptr = g_android_toupper_tab;

void dynlib_init_android_ctype(void){
    memset(g_android_ctype, 0, sizeof(g_android_ctype));
    g_android_tolower_tab[0] = g_android_toupper_tab[0] = -1;
    for (int c = 0; c < 256; c++) {
        g_android_tolower_tab[c + 1] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        g_android_toupper_tab[c + 1] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }

    g_android_ctype[0] = 0;
    for (int c = 0; c < 256; c++) {
        unsigned char flags = 0;

        if (c < 0x20 || c == 0x7f) flags |= ANDROID_CTYPE_C;
        if (c == ' ' || c == '\t') flags |= ANDROID_CTYPE_S | ANDROID_CTYPE_B;
        if (c == '\n' || c == '\r' || c == '\f' || c == '\v') flags |= ANDROID_CTYPE_S;
        if (c >= '0' && c <= '9') flags |= ANDROID_CTYPE_N;
        if (c >= 'A' && c <= 'Z') flags |= ANDROID_CTYPE_U;
        if (c >= 'a' && c <= 'z') flags |= ANDROID_CTYPE_L;
        if ((c >= '0' && c <= '9') ||
            (c >= 'A' && c <= 'F') ||
            (c >= 'a' && c <= 'f')) flags |= ANDROID_CTYPE_X;
        if (c >= 0x21 && c <= 0x7e &&
            !(flags & (ANDROID_CTYPE_U | ANDROID_CTYPE_L |
                       ANDROID_CTYPE_N | ANDROID_CTYPE_S))) {
            flags |= ANDROID_CTYPE_P;
        }

        g_android_ctype[c + 1] = flags;
    }

    dynlib_logf("Android _ctype_ table initialized at %p (slot %p)",
                g_android_ctype, &g_android_ctype_ptr);

    if (stdin) memcpy(&g_android_sF[0], stdin, sizeof(FILE));
    if (stdout) memcpy(&g_android_sF[1], stdout, sizeof(FILE));
    if (stderr) memcpy(&g_android_sF[2], stderr, sizeof(FILE));
    dynlib_logf("Android __sF compatibility table initialized at %p", g_android_sF);
}

static void my_abort(void){
    void *caller = __builtin_return_address(0);
    dynlib_logf("abort() called by Android .so caller=%p", caller);
#ifdef VITA3K_BUILD
    park_vita3k_fatal_thread();
#else
    { extern void zw_log_flush(void); zw_log_flush(); }
    sceKernelExitProcess(1);
#endif
}
static void my_exit(int status){
    void *caller = __builtin_return_address(0);
    dynlib_logf("exit(%d) called by Android .so caller=%p", status, caller);
    g_android_exit_status = status;
    if (g_android_exit_active) {
        longjmp(g_android_exit_jmp, 1);
    }
    if (status == 0) {
        dynlib_logf("exit(0) ignored outside JNI_OnLoad");
        return;
    }
#ifdef VITA3K_BUILD
    park_vita3k_fatal_thread();
#else
    { extern void zw_log_flush(void); zw_log_flush(); }
    sceKernelExitProcess(status);
#endif
}
static void my_stack_chk_fail(void){
    void *caller = __builtin_return_address(0);
    dynlib_logf("__stack_chk_fail() called by Android .so caller=%p", caller);
#ifdef VITA3K_BUILD
    park_vita3k_fatal_thread();
#else
    { extern void zw_log_flush(void); zw_log_flush(); }
    sceKernelExitProcess(1);
#endif
}

static ssize_t my_writev(int fd, const struct iovec *iov, int iovcnt){
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t written = write(fd, iov[i].iov_base, iov[i].iov_len);
        if (written < 0) return written;
        total += written;
        if ((size_t)written != iov[i].iov_len) break;
    }
    return total;
}

static void my_gl_noop(void){ }
static void *my_gl_map_buffer_oes(void){ return NULL; }
static int my_gl_unmap_buffer_oes(void){ return 0; }
extern volatile GLuint g_zw_cur_prog;
extern volatile int g_zw_skip_quads;
extern volatile GLuint g_zw_quad_progs[4];
/* v4.66 diag: per-frame list of programs that drew, and "draw only program N" */
volatile GLuint g_zw_only_prog = 0;
volatile GLuint g_zw_frame_progs[40];
volatile unsigned g_zw_frame_prog_draws[40];
volatile int g_zw_frame_nprogs = 0;
/* v4.78: texture bookkeeping + one-shot dump of the palm-tree mesh */
static struct { uint16_t w, h; uint16_t ifmt; uint8_t levels, compressed; } zw_tex_info[1024];
static GLuint zw_unit_tex[8]; static int zw_active_unit;
static struct { const void *ptr; GLint sz; GLenum ty; GLsizei st; GLboolean nrm; GLuint vbo; } zw_attr[16];   /* v4.71 */
static unsigned zw_attr_enabled;   /* v4.79: bitmask of enabled vertex attrib arrays */
static GLuint zw_bound_array_buf, zw_bound_elem_buf;
/* v4.72: palm trees. Their canopies sit between the camera and the player; the
 * game normally swaps them to the CAMERA_FADE technique through level trigger
 * zones (CTriggerComponent::OnEvent -> CMeshComponent::SwitchFadeInInstant).
 * That never happens here yet, so the opaque TREE program hides the whole
 * scene under the game's real LEQUAL depth test. Skip its draws for now. */
volatile int g_zw_hide_trees = 0;   /* v4.79: palms render correctly now (attribute-order fix) */
static uint8_t zw_prog_is_tree[512];
extern char *g_zw_shader_src[512];
static void zw_note_tree_shader(GLuint prog, GLuint shader) {
    const char *src = shader < 512 ? g_zw_shader_src[shader] : NULL;
    if (prog < 512 && src && strstr(src, "#define TREE") && strstr(src, "trunkSway") && !strstr(src, "#define CAMERA_FADE")) {
        zw_prog_is_tree[prog] = 1;
        dynlib_logf("v4.72 program %u uses the opaque TREE shader (draws hidden while g_zw_hide_trees)", prog);
    }
}
/* v4.79: SDK VitaGL's fast path for interleaved client-side vertex arrays
 * assumes the program's first attribute has the LOWEST address in the vertex
 * (it copies from that pointer and addresses the others relative to it). The
 * tree/flag sway shader declares TexCoord0 first (offset 24) and Position
 * third (offset 0), so positions were read from garbage and the palms exploded
 * across the screen. For such draws, upload the interleaved block to a scratch
 * VBO and point the attributes at offsets inside it (the VBO path is correct).
 * Returns 1 if the fix is active and zw_fix_attr_order_end() must be called. */
static int zw_fix_attr_order_begin(unsigned nverts) {
    static GLuint vbo;
    int first = -1; const uint8_t *minp = NULL; GLsizei st = 0;
    for (int i = 0; i < 16; i++) {
        if (!(zw_attr_enabled & (1u << i))) continue;
        if (zw_attr[i].vbo || (uintptr_t)zw_attr[i].ptr < 0x10000u || zw_attr[i].st <= 0) return 0;
        if (first < 0) { first = i; st = zw_attr[i].st; }
        else if (zw_attr[i].st != st) return 0;
        if (!minp || (const uint8_t *)zw_attr[i].ptr < minp) minp = (const uint8_t *)zw_attr[i].ptr;
    }
    if (first < 0 || minp == (const uint8_t *)zw_attr[first].ptr) return 0;   /* already in the order VitaGL expects */
    for (int i = 0; i < 16; i++)
        if ((zw_attr_enabled & (1u << i)) && (const uint8_t *)zw_attr[i].ptr - minp >= st) return 0;   /* not one interleaved block */
    if (!vbo) glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nverts * st, minp, GL_DYNAMIC_DRAW);
    for (int i = 0; i < 16; i++)
        if (zw_attr_enabled & (1u << i))
            glVertexAttribPointer((GLuint)i, zw_attr[i].sz, zw_attr[i].ty, zw_attr[i].nrm, st, (const void *)((const uint8_t *)zw_attr[i].ptr - minp));
    return 1;
}
static void zw_fix_attr_order_end(void) {
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    for (int i = 0; i < 16; i++)
        if (zw_attr_enabled & (1u << i))
            glVertexAttribPointer((GLuint)i, zw_attr[i].sz, zw_attr[i].ty, zw_attr[i].nrm, zw_attr[i].st, zw_attr[i].ptr);
    if (zw_bound_array_buf) glBindBuffer(GL_ARRAY_BUFFER, zw_bound_array_buf);
}
static void tr_glEnableVertexAttribArray(GLuint i) { if (i < 16) zw_attr_enabled |= 1u << i; glEnableVertexAttribArray(i); }
static void tr_glDisableVertexAttribArray(GLuint i) { if (i < 16) zw_attr_enabled &= ~(1u << i); glDisableVertexAttribArray(i); }
static void zw_dump_tree_draw(GLsizei count, GLenum type, const void *indices) {
    static int dumped;
    if (dumped >= 2 || type != GL_UNSIGNED_SHORT || (uintptr_t)indices < 0x10000u) return;
    dumped++;
    for (int u = 0; u < 3; u++) {
        GLuint t = zw_unit_tex[u];
        if (t < 1024) dynlib_logf("v4.78 tree draw unit%d tex=%u %ux%u ifmt=0x%x levels=%u compressed=%u", u, t,
                                  zw_tex_info[t].w, zw_tex_info[t].h, zw_tex_info[t].ifmt, zw_tex_info[t].levels, zw_tex_info[t].compressed);
    }
    if (dumped > 1) return;
    const uint16_t *ix = (const uint16_t *)indices; unsigned mx = 0;
    for (int k = 0; k < count; k++) if (ix[k] > mx) mx = ix[k];
    FILE *f = fopen("ux0:data/zombiewood/shots/tree_mesh.bin", "wb");
    if (!f) return;
    uint32_t hdr[4] = { (uint32_t)count, mx + 1, 0, 0 };
    fwrite(hdr, 4, 4, f);
    fwrite(ix, 2, (size_t)count, f);
    for (int a = 0; a < 4; a++) {   /* attribs 0..3: uv0(2f) color(4ub) pos(3f) uv1(2f) */
        uint32_t ah[4] = { (uint32_t)zw_attr[a].sz, (uint32_t)zw_attr[a].ty, (uint32_t)zw_attr[a].st, 0 };
        fwrite(ah, 4, 4, f);
        int bytes = zw_attr[a].sz * (zw_attr[a].ty == GL_FLOAT ? 4 : 1);
        for (unsigned v = 0; v <= mx; v++) fwrite((const uint8_t *)zw_attr[a].ptr + (size_t)v * zw_attr[a].st, 1, (size_t)bytes, f);
    }
    fclose(f);
    dynlib_logf("v4.78 tree mesh dumped: %d indices, %u verts", (int)count, mx + 1);
}
static int zw_diag_draw_filter(void) {
    GLuint pr = g_zw_cur_prog; int i, n = g_zw_frame_nprogs;
    for (i = 0; i < n; i++) if (g_zw_frame_progs[i] == pr) break;
    if (i == n && n < 40) { g_zw_frame_progs[n] = pr; g_zw_frame_prog_draws[n] = 0; g_zw_frame_nprogs = n + 1; }
    if (i < 40) g_zw_frame_prog_draws[i]++;
    if (g_zw_hide_trees && pr < 512 && zw_prog_is_tree[pr]) return 1;
    return g_zw_only_prog && pr != g_zw_only_prog;
}
static void my_gl_draw_arrays(GLenum mode, GLint first, GLsizei count){
    g_gl_draw_arrays_count++;
    if (zw_diag_draw_filter()) return;
    if (mode == GL_TRIANGLE_STRIP && first == 0 && count == 4) {   /* v4.55 */
        GLuint pr = g_zw_cur_prog;
        int k;
        for (k = 0; k < 4 && g_zw_quad_progs[k] != pr; k++) ;
        if (k == 4) { g_zw_quad_progs[3] = g_zw_quad_progs[2]; g_zw_quad_progs[2] = g_zw_quad_progs[1];
                      g_zw_quad_progs[1] = g_zw_quad_progs[0]; g_zw_quad_progs[0] = pr; }
        if (g_zw_skip_quads) return;
    }
#ifdef VITA3K_BUILD
    if (mode == GL_POINTS && count <= 1) {
        static int logged;
        if (logged < 8) {
            dynlib_logf("Vita3K probe: glDrawArrays tiny GL_POINTS skipped first=%d count=%d",
                        (int)first, (int)count);
            logged++;
        }
        return;
    }
#else
    /* v4.43: hardware log stopped right where Vita3K skips this tiny draw.
     * Trace it (not skipped) to see whether glDrawArrays itself hangs. */
    if (mode == GL_POINTS && count <= 1) {
        static int hw_logged;
        if (hw_logged < 8) {
            hw_logged++;
            dynlib_logf("hw probe: glDrawArrays tiny GL_POINTS begin #%d first=%d count=%d",
                        hw_logged, (int)first, (int)count);
            glDrawArrays(mode, first, count);
            dynlib_logf("hw probe: glDrawArrays tiny GL_POINTS end #%d", hw_logged);
            return;
        }
    }
#endif
    GLT("gl DrawArrays mode=%u first=%d count=%d", (unsigned)mode, (int)first, (int)count);
    { int fixed = (first >= 0 && count > 0) ? zw_fix_attr_order_begin((unsigned)(first + count)) : 0;
      glDrawArrays(mode, first, count);
      if (fixed) zw_fix_attr_order_end(); }
}
/* ── v5.14: static vertex cache ──
 * glitch draws nearly all level geometry from client-side vertex arrays. VitaGL then copies
 * the vertices of every such draw into GPU memory, every frame: measured 0.7-1.4 MB and
 * ~4.5 ms of memcpy per gameplay frame, most of it for meshes that never change.
 * A client-side interleaved vertex block that stays byte-identical for a few frames is
 * uploaded once into a VBO and drawn from there (VitaGL's VBO path copies nothing).
 * It is keyed by its address. Every frame it is used, a sparse sample of it is re-checked,
 * and the whole block about once every 32 frames; any difference drops the VBO and the draw
 * goes back to the normal path. Blocks that keep changing (skinned meshes, particles, UI)
 * are left alone and only re-examined every 300 frames. */
#ifndef ZW_VCACHE
#define ZW_VCACHE 1
#endif
#define ZW_VC_N        512
#define ZW_VC_PROBE    8
#define ZW_VC_MIN      2048u            /* bytes: smaller blocks are not worth a VBO */
#define ZW_VC_MAX      (512u * 1024u)
#define ZW_VC_BUDGET   (6u << 20)       /* total VBO bytes */
#define ZW_VC_STABLE   3                /* identical frames before upload */
typedef struct {
    const uint8_t *ptr; uint32_t bytes, full, samp; GLuint vbo;
    int last_frame, check_frame, retry_frame; uint8_t stable, strikes;
} zw_vc;
static zw_vc zw_vcs[ZW_VC_N];
static unsigned zw_vc_total; static int zw_vc_frame;
volatile int g_zw_vcache_on = ZW_VCACHE;
static unsigned zw_vc_st_hit, zw_vc_st_drop, zw_vc_st_up, zw_vc_st_up_us; static int zw_vc_last_up;
static uint32_t zw_vc_hash_full(const uint8_t *p, uint32_t n) {
    const uint32_t *w = (const uint32_t *)p; uint32_t k = n >> 2, i = 0;
    uint32_t a = n, b = 0x9e3779b9u, c = 0x85ebca6bu, d = 0xc2b2ae35u;
    for (; i + 4 <= k; i += 4) {
        a = (a ^ w[i]) * 0x01000193u; b = (b ^ w[i + 1]) * 0x01000193u;
        c = (c ^ w[i + 2]) * 0x01000193u; d = (d ^ w[i + 3]) * 0x01000193u;
    }
    for (; i < k; i++) a = (a ^ w[i]) * 0x01000193u;
    return a ^ (b << 7 | b >> 25) ^ (c << 13 | c >> 19) ^ (d << 19 | d >> 13);
}
static uint32_t zw_vc_hash_samp(const uint8_t *p, uint32_t n) {
    const uint32_t *w = (const uint32_t *)p; uint32_t k = n >> 2, h = n;
    for (uint32_t i = 0; i < k; i += 97) h = (h ^ w[i]) * 0x01000193u;   /* one word in ~388 bytes, drifting through the vertex layout */
    if (k) h = (h ^ w[k - 1]) * 0x01000193u;
    return h;
}
static void zw_vc_drop(zw_vc *e) {
    if (e->vbo) { glDeleteBuffers(1, &e->vbo); zw_vc_total -= e->bytes; e->vbo = 0; }
}
void zw_vcache_frame(void) {   /* once per frame from the main loop */
    zw_vc_frame++;
    if ((zw_vc_frame & 63) == 0)
        for (int i = 0; i < ZW_VC_N; i++)
            if (zw_vcs[i].ptr && zw_vcs[i].last_frame + 180 < zw_vc_frame) { zw_vc_drop(&zw_vcs[i]); zw_vcs[i].ptr = NULL; }
}
/* 1 = the attributes now point into a cached VBO; call zw_fix_attr_order_end() after the draw */
static int zw_vcache_begin(GLsizei count, GLenum type, const void *indices) {
    if (!g_zw_vcache_on || type != GL_UNSIGNED_SHORT || (uintptr_t)indices < 0x10000u || zw_bound_elem_buf || count <= 0) return 0;
    int first = -1; const uint8_t *minp = NULL; GLsizei st = 0;
    for (int i = 0; i < 16; i++) {
        if (!(zw_attr_enabled & (1u << i))) continue;
        if (zw_attr[i].vbo || (uintptr_t)zw_attr[i].ptr < 0x10000u || zw_attr[i].st <= 0) return 0;
        if (first < 0) { first = i; st = zw_attr[i].st; }
        else if (zw_attr[i].st != st) return 0;
        if (!minp || (const uint8_t *)zw_attr[i].ptr < minp) minp = (const uint8_t *)zw_attr[i].ptr;
    }
    if (first < 0 || ((uintptr_t)minp & 3) || (st & 3)) return 0;
    for (int i = 0; i < 16; i++)
        if ((zw_attr_enabled & (1u << i)) && (const uint8_t *)zw_attr[i].ptr - minp >= st) return 0;   /* not one interleaved block */
    const uint16_t *ix = (const uint16_t *)indices; unsigned mx = 0;
    for (int k = 0; k < count; k++) if (ix[k] > mx) mx = ix[k];
    uint32_t bytes = (mx + 1) * (uint32_t)st;
    if (bytes < ZW_VC_MIN || bytes > ZW_VC_MAX) return 0;

    unsigned h = (unsigned)(((uintptr_t)minp >> 4) * 2654435761u) >> 23;   /* 9 bits */
    zw_vc *e = NULL, *spare = NULL;
    for (int p = 0; p < ZW_VC_PROBE; p++) {
        zw_vc *c = &zw_vcs[(h + (unsigned)p) & (ZW_VC_N - 1)];
        if (c->ptr == minp) { e = c; break; }
        if (!spare || !c->ptr || (spare->ptr && c->last_frame < spare->last_frame)) { if (!spare || spare->ptr) spare = c; }
    }
    if (!e) {
        if (spare->ptr && spare->last_frame + 2 >= zw_vc_frame) return 0;   /* neighbourhood full of live entries */
        zw_vc_drop(spare);
        memset(spare, 0, sizeof(*spare));
        e = spare; e->ptr = minp; e->bytes = bytes; e->check_frame = -1;
    }
    e->last_frame = zw_vc_frame;
    if (zw_vc_frame < e->retry_frame) return 0;
    if (bytes > e->bytes) {   /* a draw reaching further into the same block: start over with the larger range */
        zw_vc_drop(e); e->bytes = bytes; e->stable = 0; e->check_frame = -1;
    }
    if (e->check_frame != zw_vc_frame) {   /* once per frame per block */
        e->check_frame = zw_vc_frame;
        if (e->vbo) {
            int bad = zw_vc_hash_samp(minp, e->bytes) != e->samp;
            if (!bad && ((zw_vc_frame + (int)(e - zw_vcs)) & 31) == 0) bad = zw_vc_hash_full(minp, e->bytes) != e->full;
            if (bad) { zw_vc_drop(e); e->stable = 0; zw_vc_st_drop++; if (++e->strikes >= 3) { e->strikes = 0; e->retry_frame = zw_vc_frame + 300; } return 0; }
        } else {
            uint32_t f = zw_vc_hash_full(minp, e->bytes);
            if (e->stable && f == e->full) e->stable++;
            else {
                if (e->stable || e->full) { if (++e->strikes >= 3) { e->strikes = 0; e->stable = 0; e->full = 0; e->retry_frame = zw_vc_frame + 300; return 0; } }
                e->full = f; e->stable = 1;
            }
            /* at most one upload every 4 frames, so a new area does not upload dozens of blocks in one frame */
            if (e->stable > ZW_VC_STABLE && zw_vc_total + e->bytes <= ZW_VC_BUDGET && zw_vc_frame - zw_vc_last_up >= 4) {
                SceUInt64 tu = sceKernelGetProcessTimeWide();
                zw_vc_last_up = zw_vc_frame;
                glGenBuffers(1, &e->vbo);
                if (e->vbo) {
                    glBindBuffer(GL_ARRAY_BUFFER, e->vbo);
                    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)e->bytes, minp, GL_STATIC_DRAW);
                    glBindBuffer(GL_ARRAY_BUFFER, zw_bound_array_buf);
                    e->samp = zw_vc_hash_samp(minp, e->bytes);
                    zw_vc_total += e->bytes; e->strikes = 0; zw_vc_st_up++;
                    zw_vc_st_up_us += (unsigned)(sceKernelGetProcessTimeWide() - tu);
                }
            }
        }
    }
    if (!e->vbo) return 0;
    glBindBuffer(GL_ARRAY_BUFFER, e->vbo);
    for (int i = 0; i < 16; i++)
        if (zw_attr_enabled & (1u << i))
            glVertexAttribPointer((GLuint)i, zw_attr[i].sz, zw_attr[i].ty, zw_attr[i].nrm, st, (const void *)((const uint8_t *)zw_attr[i].ptr - minp));
    zw_vc_st_hit++;
    return 1;
}

#ifndef ZW_DRAW_STATS
#define ZW_DRAW_STATS 0   /* 1 = measurement: time every glDrawElements by class; never ship 1 */
#endif
#if ZW_DRAW_STATS
static struct { unsigned n, us, post_us, idx, vbytes, client_idx, hist[6], max_us, max_idx, max_vb; GLuint max_prog; } zw_dstat[4];
#endif
static void my_gl_draw_elements(GLenum mode, GLsizei count, GLenum type, const void *indices){
    g_gl_draw_elements_count++;
    if (g_zw_cur_prog < 512 && zw_prog_is_tree[g_zw_cur_prog]) zw_dump_tree_draw(count, type, indices);
    if (zw_diag_draw_filter()) return;
    GLT("gl DrawElements mode=%u count=%d type=0x%x idx=%p", (unsigned)mode, (int)count, (unsigned)type, indices);
    if (g_zw_gl_trace && type == GL_UNSIGNED_SHORT && (uintptr_t)indices > 0x10000u && count >= 3) {   /* v4.71 */
        const uint16_t *ix = (const uint16_t *)indices;
        unsigned mn = 0xFFFF, mx = 0;
        for (int k = 0; k < count; k++) { if (ix[k] < mn) mn = ix[k]; if (ix[k] > mx) mx = ix[k]; }
        dynlib_logf("   idx min=%u max=%u first=%u,%u,%u abuf=%u ebuf=%u", mn, mx, ix[0], ix[1], ix[2], zw_bound_array_buf, zw_bound_elem_buf);
        for (int a = 0; a < 16; a++) {
            if (zw_attr[a].ty != GL_FLOAT || zw_attr[a].sz != 3 || (uintptr_t)zw_attr[a].ptr < 0x10000u || zw_attr[a].st <= 0) continue;
            const unsigned pick[4] = { ix[0], ix[1], ix[2], ix[count / 2] };
            for (int k = 0; k < 4; k++) {
                const float *f = (const float *)((const uint8_t *)zw_attr[a].ptr + (size_t)pick[k] * zw_attr[a].st);
                dynlib_logf("   a%d[%u]: %g %g %g", a, pick[k], (double)f[0], (double)f[1], (double)f[2]);
            }
            float bmn[3] = { 1e30f, 1e30f, 1e30f }, bmx[3] = { -1e30f, -1e30f, -1e30f };
            for (int k = 0; k < count; k++) {
                const float *f = (const float *)((const uint8_t *)zw_attr[a].ptr + (size_t)ix[k] * zw_attr[a].st);
                for (int c = 0; c < 3; c++) { if (f[c] < bmn[c]) bmn[c] = f[c]; if (f[c] > bmx[c]) bmx[c] = f[c]; }
            }
            dynlib_logf("   a%d bbox: [%g %g %g] .. [%g %g %g]", a, (double)bmn[0], (double)bmn[1], (double)bmn[2], (double)bmx[0], (double)bmx[1], (double)bmx[2]);
            break;
        }
    }
    {
        int fixed = 0;
        if (type == GL_UNSIGNED_SHORT && (uintptr_t)indices > 0x10000u && !zw_bound_elem_buf && count > 0) {
            int need = 0, first = -1;   /* cheap pre-check before scanning indices */
            for (int i = 0; i < 16 && !need; i++)
                if (zw_attr_enabled & (1u << i)) { if (first < 0) first = i; else if (zw_attr[i].ptr < zw_attr[first].ptr) need = 1; }
            if (need) {
                const uint16_t *ix = (const uint16_t *)indices; unsigned mx = 0;
                for (int k = 0; k < count; k++) if (ix[k] > mx) mx = ix[k];
                fixed = zw_fix_attr_order_begin(mx + 1);
            }
        }
#if ZW_DRAW_STATS
        {
            int cls = 2, st = 0;   /* 0 = attr-order fix, 1 = client-side vertices, 2 = all VBO, 3 = vertex cache */
            for (int i = 0; i < 16; i++) if (zw_attr_enabled & (1u << i)) { if (!st) st = zw_attr[i].st; if (!zw_attr[i].vbo) cls = 1; }
            if (fixed) cls = 0;
            SceUInt64 tc0 = sceKernelGetProcessTimeWide();
            if (!fixed && cls == 1 && zw_vcache_begin(count, type, indices)) { fixed = 1; cls = 3; }
            zw_dstat[cls].post_us += (unsigned)(sceKernelGetProcessTimeWide() - tc0);
            unsigned vbytes = 0;
            if (cls != 2 && type == GL_UNSIGNED_SHORT && (uintptr_t)indices > 0x10000u && !zw_bound_elem_buf) {
                const uint16_t *ix = (const uint16_t *)indices; unsigned mx = 0;
                for (int k = 0; k < count; k++) if (ix[k] > mx) mx = ix[k];
                vbytes = (mx + 1) * (unsigned)st;
            }
#if ZW_GXMSTAT
            extern volatile int g_zw_in_draw; g_zw_in_draw = 1;
#endif
            SceUInt64 t1 = sceKernelGetProcessTimeWide();
            glDrawElements(mode, count, type, indices);
            SceUInt64 t2 = sceKernelGetProcessTimeWide();
#if ZW_GXMSTAT
            g_zw_in_draw = 0;
#endif
            if (fixed) zw_fix_attr_order_end();
            SceUInt64 t3 = sceKernelGetProcessTimeWide();
            unsigned us = (unsigned)(t2 - t1);
            zw_dstat[cls].n++; zw_dstat[cls].us += us; zw_dstat[cls].post_us += (unsigned)(t3 - t2);
            zw_dstat[cls].idx += (unsigned)count; zw_dstat[cls].vbytes += vbytes; if (!zw_bound_elem_buf) zw_dstat[cls].client_idx++;
            zw_dstat[cls].hist[us < 10 ? 0 : us < 25 ? 1 : us < 50 ? 2 : us < 100 ? 3 : us < 250 ? 4 : 5]++;
            if (us > zw_dstat[cls].max_us) { zw_dstat[cls].max_us = us; zw_dstat[cls].max_idx = (unsigned)count; zw_dstat[cls].max_vb = vbytes; zw_dstat[cls].max_prog = g_zw_cur_prog; }
        }
#else
        if (!fixed) fixed = zw_vcache_begin(count, type, indices);
        glDrawElements(mode, count, type, indices);
        if (fixed) zw_fix_attr_order_end();
#endif
    }
}
#if ZW_DRAW_STATS
void zw_draw_stats_log(int frame) {
    static const char *const nm[4] = { "attrfix", "client", "vbo", "vcache" };
    dynlib_logf("  stats vcache %s f%d: %u KB in VBOs, per frame %.1f cached draws, last 60 frames: %u uploads (%u us), %u drops",
                g_zw_vcache_on ? "ON" : "OFF", frame, zw_vc_total >> 10, zw_vc_st_hit / 60.0, zw_vc_st_up, zw_vc_st_up_us, zw_vc_st_drop);
    zw_vc_st_hit = zw_vc_st_up = zw_vc_st_drop = zw_vc_st_up_us = 0;
    for (int c = 0; c < 4; c++) {
        if (zw_dstat[c].n)
            dynlib_logf("  stats draws f%d %-7s per frame: %.1f draws %.2f ms (+%.2f ms after), %.0f idx, %.1f KB verts, client-idx %.1f | us <10:%u <25:%u <50:%u <100:%u <250:%u more:%u | max %u us (idx %u, %u vert bytes, prog %u)",
                        frame, nm[c], zw_dstat[c].n / 60.0, zw_dstat[c].us / 60000.0, zw_dstat[c].post_us / 60000.0, zw_dstat[c].idx / 60.0,
                        zw_dstat[c].vbytes / 60.0 / 1024.0, zw_dstat[c].client_idx / 60.0,
                        zw_dstat[c].hist[0], zw_dstat[c].hist[1], zw_dstat[c].hist[2], zw_dstat[c].hist[3], zw_dstat[c].hist[4], zw_dstat[c].hist[5],
                        zw_dstat[c].max_us, zw_dstat[c].max_idx, zw_dstat[c].max_vb, (unsigned)zw_dstat[c].max_prog);
        memset(&zw_dstat[c], 0, sizeof(zw_dstat[c]));
    }
}
#endif

/* v4.50 trace wrappers (log only while g_zw_gl_trace is set) */
/* v4.57: GL only clears depth/stencil bits that are write-enabled. The game
 * ends frames with DepthMask(0) and (glitch caches state) never re-enables it
 * before the next glClear, so VitaGL never cleared depth/stencil: the world
 * vanished once depth accumulated and shadows kept darkening. Force full
 * write masks for the clear, then restore the game's masks. */
static GLboolean zw_depth_mask = GL_TRUE;
static GLuint zw_stencil_mask = 0xFFFFFFFFu;
static void tr_glClear(GLbitfield m) {
    GLT("gl Clear mask=0x%x", (unsigned)m);
    /* v4.63: also enable the depth/stencil TESTS for the clear (SDK VitaGL
     * appears to skip depth/stencil clears while the tests are disabled). */
    GLboolean dt = glIsEnabled(GL_DEPTH_TEST), stt = glIsEnabled(GL_STENCIL_TEST);
    if (m & GL_DEPTH_BUFFER_BIT) { glDepthMask(GL_TRUE); glEnable(GL_DEPTH_TEST); }
    if (m & GL_STENCIL_BUFFER_BIT) { glStencilMask(0xFFFFFFFFu); glEnable(GL_STENCIL_TEST); }
    glClear(m);
    if (m & GL_DEPTH_BUFFER_BIT) { glDepthMask(zw_depth_mask); if (!dt) glDisable(GL_DEPTH_TEST); }
    if (m & GL_STENCIL_BUFFER_BIT) { glStencilMask(zw_stencil_mask); if (!stt) glDisable(GL_STENCIL_TEST); }
}
static void tr_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { GLT("gl Viewport %d,%d %dx%d", x, y, w, h); glViewport(x, y, w, h); }
static void tr_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { GLT("gl Scissor %d,%d %dx%d", x, y, w, h); glScissor(x, y, w, h); }
static void tr_glBindFramebuffer(GLenum t, GLuint fb) { GLT("gl BindFramebuffer 0x%x fb=%u", (unsigned)t, fb); glBindFramebuffer(t, fb); }
volatile GLuint g_zw_cur_prog = 0;          /* v4.55 */
volatile int g_zw_skip_quads = 0;           /* v4.55: L toggles */
volatile GLuint g_zw_quad_progs[4];         /* last programs that drew fullscreen quads */
static void tr_glVertexAttribPointer(GLuint i, GLint sz, GLenum ty, GLboolean nrm, GLsizei st, const void *ptr) {
    if (i < 16) { zw_attr[i].ptr = ptr; zw_attr[i].sz = sz; zw_attr[i].ty = ty; zw_attr[i].st = st; zw_attr[i].nrm = nrm; zw_attr[i].vbo = zw_bound_array_buf; }
    GLT("gl VertexAttribPointer idx=%u size=%d type=0x%x norm=%d stride=%d ptr=%p", i, sz, (unsigned)ty, nrm, st, ptr);
    if (g_zw_gl_trace && (uintptr_t)ptr > 0x10000u && st > 0) {   /* v4.67: sample vertices 0,1,100 */
        static const int vi[3] = { 0, 1, 100 };
        for (int k = 0; k < 3; k++) {
            const uint8_t *q = (const uint8_t *)ptr + (size_t)vi[k] * st;
            if (ty == GL_FLOAT) { const float *f = (const float *)q;
                dynlib_logf("   v%d: %g %g %g %g", vi[k], (double)f[0], sz > 1 ? (double)f[1] : 0.0, sz > 2 ? (double)f[2] : 0.0, sz > 3 ? (double)f[3] : 0.0); }
            else dynlib_logf("   v%d: %02x %02x %02x %02x", vi[k], q[0], q[1], q[2], q[3]);
        }
    }
    glVertexAttribPointer(i, sz, ty, nrm, st, ptr);
}
static void tr_glUseProgram(GLuint p) { GLT("gl UseProgram %u", p); g_zw_cur_prog = p; glUseProgram(p); }
static void tr_glBindTexture(GLenum t, GLuint tex) { GLT("gl BindTexture 0x%x tex=%u", (unsigned)t, tex); if (t == GL_TEXTURE_2D) zw_unit_tex[zw_active_unit & 7] = tex; glBindTexture(t, tex); }
/* v5.16: glTexSubImage2D on a texture drawn in the last 4 frames makes VitaGL copy the WHOLE texture to
 * new memory first (textures.c, !TEXTURES_SPEEDHACK). The HUD updates a large atlas this way while
 * firing: ~17 ms per call on hardware. Marking the texture unused skips the copy; the cost is that
 * the GPU may still be reading the old pixels of the updated rectangle for up to a frame.
 * Layout checked against the linked libvitaGL.a: 56-byte slots, last_frame at offset 0, 16384 slots. */
#ifndef ZW_TEXSUB_FAST
#define ZW_TEXSUB_FAST 1
#endif
#ifndef ZW_TEXSUB_AB
#define ZW_TEXSUB_AB 0   /* 1 = measurement: fast path only in odd 600-frame windows */
#endif
extern uint8_t texture_slots[];
extern uint32_t vgl_framecount;
unsigned g_zw_texsub_n, g_zw_texsub_skip, g_zw_texsub_us, g_zw_texsub_max_us, g_zw_texsub_last[6];
static void my_glTexSubImage2D(GLenum t, GLint lv, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum ty, const void *px) {
    GLuint id = t == GL_TEXTURE_2D ? zw_unit_tex[zw_active_unit & 7] : 0;
    int fast = ZW_TEXSUB_FAST && (!ZW_TEXSUB_AB || ((vgl_framecount / 600) & 1));
    if (fast && id && id < 16384) {
        uint32_t *last = (uint32_t *)(texture_slots + (size_t)id * 56);
        if (*last != 0xFFFFFFFFu && vgl_framecount - *last <= 4) { *last = 0xFFFFFFFFu; g_zw_texsub_skip++; }
    }
    uint64_t t0 = sceKernelGetProcessTimeWide();
    glTexSubImage2D(t, lv, x, y, w, h, fmt, ty, px);
    unsigned us = (unsigned)(sceKernelGetProcessTimeWide() - t0);
    g_zw_texsub_n++; g_zw_texsub_us += us;
    if (us > g_zw_texsub_max_us) { g_zw_texsub_max_us = us;
        g_zw_texsub_last[0] = id; g_zw_texsub_last[1] = (unsigned)w; g_zw_texsub_last[2] = (unsigned)h; g_zw_texsub_last[3] = fmt; g_zw_texsub_last[4] = (unsigned)fast; g_zw_texsub_last[5] = us; }
}
static void tr_glActiveTexture(GLenum u) { GLT("gl ActiveTexture 0x%x", (unsigned)u); zw_active_unit = (int)(u - GL_TEXTURE0) & 7; glActiveTexture(u); }
static void zw_note_tex(GLint lvl, GLenum ifmt, GLsizei w, GLsizei h, int compressed) {
    GLuint t = zw_unit_tex[zw_active_unit & 7];
    if (t >= 1024) return;
    if (lvl == 0) { zw_tex_info[t].w = (uint16_t)w; zw_tex_info[t].h = (uint16_t)h; zw_tex_info[t].ifmt = (uint16_t)ifmt; zw_tex_info[t].compressed = (uint8_t)compressed; zw_tex_info[t].levels = 0; }
    zw_tex_info[t].levels++;
}
volatile int g_zw_force_nodepth = 0;    /* v4.56: R toggles */
volatile int g_zw_force_nostencil = 0;  /* v4.56: D-pad Up toggles (v4.64 default ON) */
static int zw_ds_log_budget = 60;       /* first N depth/stencil state calls always logged */
#define DSLOG(...) do { if (zw_ds_log_budget > 0 || g_zw_gl_trace) { if (zw_ds_log_budget > 0) zw_ds_log_budget--; dynlib_logf(__VA_ARGS__); } } while (0)
static void tr_glEnable(GLenum c) {
    GLT("gl Enable 0x%x", (unsigned)c);
    if ((c == GL_DEPTH_TEST && g_zw_force_nodepth) || (c == GL_STENCIL_TEST && g_zw_force_nostencil)) { glDisable(c); return; }
    glEnable(c);
}
volatile GLenum g_zw_depthfunc_override = 1;   /* v4.61: D-pad Down cycles (v4.64 default AUTO) */
void zw_apply_depthfunc_override(void);
static GLenum zw_game_depthfunc = GL_LESS;
static void tr_glDepthFunc(GLenum f) {
    DSLOG("gl DepthFunc 0x%x", (unsigned)f);
    zw_game_depthfunc = f;
    zw_apply_depthfunc_override();
}
/* v4.62: default (override 0 = AUTO) maps LESS/LEQUAL to NOTEQUAL: on hw the
 * game's LEQUAL failed against the cleared depth while NOTEQUAL layered fine.
 * Workaround until the clear/depth-value mismatch is understood.
 * 1 = GAME (no change); other values force that func. */
void zw_apply_depthfunc_override(void) {
    GLenum f = zw_game_depthfunc;
    if (g_zw_depthfunc_override == 0) {
        if (f == GL_LEQUAL || f == GL_LESS) f = GL_NOTEQUAL;
    } else if (g_zw_depthfunc_override != 1) {
        f = g_zw_depthfunc_override;
    }
    glDepthFunc(f);
}
static void tr_glDepthMask(GLboolean m) { DSLOG("gl DepthMask %d", m); zw_depth_mask = m; glDepthMask(m); }
static void tr_glStencilFunc(GLenum f, GLint r, GLuint m) { DSLOG("gl StencilFunc 0x%x ref=%d mask=0x%x", (unsigned)f, r, m); glStencilFunc(f, r, m); }
static void tr_glStencilOp(GLenum a, GLenum b, GLenum c) { DSLOG("gl StencilOp 0x%x 0x%x 0x%x", (unsigned)a, (unsigned)b, (unsigned)c); glStencilOp(a, b, c); }
static void tr_glStencilMask(GLuint m) { DSLOG("gl StencilMask 0x%x", m); zw_stencil_mask = m; glStencilMask(m); }
static void tr_glClearStencil(GLint v) { DSLOG("gl ClearStencil %d", v); glClearStencil(v); }
static void tr_glDisable(GLenum c) { GLT("gl Disable 0x%x", (unsigned)c); glDisable(c); }
static void tr_glBlendFunc(GLenum s, GLenum d) { GLT("gl BlendFunc 0x%x 0x%x", (unsigned)s, (unsigned)d); glBlendFunc(s, d); }
static void tr_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { GLT("gl ColorMask %d%d%d%d", r, g, b, a); glColorMask(r, g, b, a); }
static void tr_glBindBuffer(GLenum t, GLuint b) { GLT("gl BindBuffer 0x%x buf=%u", (unsigned)t, b); if (t == GL_ARRAY_BUFFER) zw_bound_array_buf = b; else if (t == GL_ELEMENT_ARRAY_BUFFER) zw_bound_elem_buf = b; glBindBuffer(t, b); }
static void tr_glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean tp, const GLfloat *v) {
    if (g_zw_gl_trace && v)
        dynlib_logf("gl UniformMatrix4fv loc=%d n=%d tp=%d [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.3f %.3f %.3f %.3f]",
                    loc, (int)n, (int)tp, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
    glUniformMatrix4fv(loc, n, tp, v);
}
static int zw_is_ident_ch(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
/* v4.69: SDK VitaGL reports mat4/mat3 uniforms as vec4[4]/vec3[3]. glitch then
 * treats material matrices (TextureMatrix = lightmap UV transform) as vector
 * arrays and uploads zeros. Fix the reported type from the GLSL source. */
extern char *g_zw_shader_src[512];
static int zw_uniform_declared_as(const char *type_kw, const char *name) {
    static char known[96][48]; static char known_kw[96]; static int nknown;
    for (int i = 0; i < nknown; i++) if (known_kw[i] == type_kw[3] && !strcmp(known[i], name)) return 1;
    size_t kl = strlen(type_kw), nl = strlen(name);
    for (int sh = 0; sh < 512; sh++) {
        const char *src = g_zw_shader_src[sh];
        if (!src) continue;
        for (const char *q = src; (q = strstr(q, type_kw)); q += kl) {
            const char *r = q + kl;
            if (*r != ' ' && *r != '\t') continue;
            while (*r == ' ' || *r == '\t') r++;
            if (!strncmp(r, name, nl) && !zw_is_ident_ch(r[nl])) {
                if (nknown < 96 && nl < 48) { strcpy(known[nknown], name); known_kw[nknown] = type_kw[3]; nknown++; }
                return 1;
            }
        }
    }
    return 0;
}
static void tr_glGetActiveUniform(GLuint prog, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name) {
    glGetActiveUniform(prog, index, bufSize, length, size, type, name);
    if (type && size && name) {
        if (*type == GL_FLOAT_VEC4 && *size == 4 && zw_uniform_declared_as("mat4", name)) { *type = GL_FLOAT_MAT4; *size = 1; }
        else if (*type == GL_FLOAT_VEC3 && *size == 3 && zw_uniform_declared_as("mat3", name)) { *type = GL_FLOAT_MAT3; *size = 1; }
    }
    GLT("gl GetActiveUniform prog=%u idx=%u %s type=0x%x size=%d", prog, index,
        name ? name : "?", type ? (unsigned)*type : 0u, size ? (int)*size : -1);
}
/* v4.67: uniform value + location-name tracing */
static GLint tr_glGetUniformLocation(GLuint prog, const char *name) {
    GLint loc = glGetUniformLocation(prog, name);
    GLT("gl GetUniformLocation prog=%u %s -> %d", prog, name ? name : "(null)", loc);
    return loc;
}
static GLint tr_glGetAttribLocation(GLuint prog, const char *name) {
    GLint loc = glGetAttribLocation(prog, name);
    GLT("gl GetAttribLocation prog=%u %s -> %d", prog, name ? name : "(null)", loc);
    return loc;
}
static void tr_glUniformNfv(const char *nm, int comps, GLint loc, GLsizei n, const GLfloat *v) {
    if (!g_zw_gl_trace || !v) return;
    char buf[160]; int o = 0, tot = comps * (n > 0 ? n : 0);
    buf[0] = 0;
    for (int i = 0; i < tot && i < 8 && o < 140; i++) o += snprintf(buf + o, sizeof(buf) - o, " %g", (double)v[i]);
    dynlib_logf("gl %s loc=%d n=%d [%s ]", nm, loc, (int)n, buf);
}
static void tr_glUniform1fv(GLint loc, GLsizei n, const GLfloat *v) { tr_glUniformNfv("Uniform1fv", 1, loc, n, v); glUniform1fv(loc, n, v); }
static void tr_glUniform2fv(GLint loc, GLsizei n, const GLfloat *v) { tr_glUniformNfv("Uniform2fv", 2, loc, n, v); glUniform2fv(loc, n, v); }
static void tr_glUniform3fv(GLint loc, GLsizei n, const GLfloat *v) { tr_glUniformNfv("Uniform3fv", 3, loc, n, v); glUniform3fv(loc, n, v); }
static void tr_glUniform4fv(GLint loc, GLsizei n, const GLfloat *v) { tr_glUniformNfv("Uniform4fv", 4, loc, n, v); glUniform4fv(loc, n, v); }
static void tr_glUniform1i(GLint loc, GLint v) { GLT("gl Uniform1i loc=%d v=%d", loc, v); glUniform1i(loc, v); }
static void tr_glUniform1iv(GLint loc, GLsizei n, const GLint *v) { GLT("gl Uniform1iv loc=%d n=%d v0=%d", loc, (int)n, v ? v[0] : 0); glUniform1iv(loc, n, v); }
static void tr_glTexImage2D(GLenum t, GLint lvl, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum fmt, GLenum ty, const void *px) {
    static int n;
    if (n < 60 || g_zw_gl_trace) { n++; dynlib_logf("gl TexImage2D t=0x%x lvl=%d ifmt=0x%x %dx%d fmt=0x%x type=0x%x px=%p", (unsigned)t, lvl, (unsigned)ifmt, w, h, (unsigned)fmt, (unsigned)ty, px); }
    zw_note_tex(lvl, (GLenum)ifmt, w, h, 0);
    { SceUInt64 _pt = sceKernelGetProcessTimeWide(); glTexImage2D(t, lvl, ifmt, w, h, b, fmt, ty, px);
      extern volatile uint64_t g_zw_prof_us[6]; extern volatile unsigned g_zw_prof_n[6];
      g_zw_prof_us[5] += sceKernelGetProcessTimeWide() - _pt; g_zw_prof_n[5]++; }
}
static void tr_glCompressedTexImage2D(GLenum t, GLint lvl, GLenum ifmt, GLsizei w, GLsizei h, GLint b, GLsizei sz, const void *d) {
    static int n;
    if (n < 60 || g_zw_gl_trace) { n++; dynlib_logf("gl CompressedTexImage2D t=0x%x lvl=%d ifmt=0x%x %dx%d size=%d", (unsigned)t, lvl, (unsigned)ifmt, w, h, (int)sz); }
    zw_note_tex(lvl, ifmt, w, h, 1);
    { SceUInt64 _pt = sceKernelGetProcessTimeWide(); glCompressedTexImage2D(t, lvl, ifmt, w, h, b, sz, d);
      extern volatile uint64_t g_zw_prof_us[6]; extern volatile unsigned g_zw_prof_n[6];
      g_zw_prof_us[5] += sceKernelGetProcessTimeWide() - _pt; g_zw_prof_n[5]++; }
}

static void my_gl_shader_source(GLuint shader, GLsizei count,
                                const GLchar *const *string,
                                const GLint *length) {
#ifdef VITA3K_BUILD
    static unsigned int log_count = 0;
    int saw_mask = 0;
    int saw_frag = 0;
    char preview[192];
    size_t preview_len = 0;

    preview[0] = '\0';
    for (GLsizei i = 0; string && i < count; i++) {
        const char *src = string[i];
        if (!src) continue;
        int len = length ? length[i] : (int)strlen(src);
        if (len < 0) len = (int)strlen(src);
        if (len <= 0) continue;

        if (strstr(src, "mask") || strstr(src, "Mask") ||
            strstr(src, "MASK") || strstr(src, "MASK_MESH")) {
            saw_mask = 1;
        }
        if (strstr(src, "gl_FragColor") || strstr(src, "FRAGCOLOR")) saw_frag = 1;

        if (preview_len + 1 < sizeof(preview)) {
            size_t room = sizeof(preview) - 1 - preview_len;
            size_t copy = (size_t)len < room ? (size_t)len : room;
            memcpy(preview + preview_len, src, copy);
            preview_len += copy;
            preview[preview_len] = '\0';
        }
    }

    if (log_count < 24 || saw_mask) {
        for (char *p = preview; *p; p++) {
            if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
        }
        dynlib_logf("glShaderSource shader=%u count=%d mask=%d frag=%d src='%s'",
                    (unsigned)shader, (int)count, saw_mask, saw_frag, preview);
    }
    log_count++;

    if (saw_mask && saw_frag) {
        static const GLchar *solid_frag =
            "precision mediump float;\n"
            "void main() { gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0); }\n";
        GLint solid_len = (GLint)strlen(solid_frag);
        dynlib_logf("Vita3K: replacing mask fragment shader %u with solid fallback",
                    (unsigned)shader);
        glShaderSource(shader, 1, &solid_frag, &solid_len);
        return;
    }
#endif
#ifndef VITA3K_BUILD
    /* v4.54: remember a one-line preview per shader for failure reports */
    {
        extern char g_zw_shader_preview[512][120];
        extern char *g_zw_shader_src[512];
        if (shader < 512) {
            size_t total = 0;
            for (GLsizei i = 0; string && i < count; i++)
                if (string[i]) total += (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
            free(g_zw_shader_src[shader]);
            g_zw_shader_src[shader] = malloc(total + 1);
            if (g_zw_shader_src[shader]) {
                size_t o = 0;
                for (GLsizei i = 0; string && i < count; i++) {
                    if (!string[i]) continue;
                    size_t l = (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
                    memcpy(g_zw_shader_src[shader] + o, string[i], l); o += l;
                }
                g_zw_shader_src[shader][o] = 0;
            }
        }
        if (shader < 512) {
            char *pv = g_zw_shader_preview[shader];
            size_t n = 0;
            pv[0] = '\0';
            for (GLsizei i = 0; string && i < count && n + 1 < 120; i++) {
                const char *src = string[i];
                if (!src) continue;
                int len = length && length[i] >= 0 ? length[i] : (int)strlen(src);
                for (int k = 0; k < len && n + 1 < 120; k++) {
                    char ch = src[k];
                    pv[n++] = (ch == '\n' || ch == '\r' || ch == '\t') ? ' ' : ch;
                }
            }
            pv[n] = '\0';
        }
    }
#endif
    glShaderSource(shader, count, string, length);
}

/* v4.54: report shader compile / program link failures with the driver log */
char g_zw_shader_preview[512][120];
char *g_zw_shader_src[512];
static GLuint g_zw_prog_shaders[256][2];
static void tr_glAttachShader(GLuint prog, GLuint sh) {
    if (prog < 256) {
        /* v5.02: slot by shader type ([0] vertex, [1] fragment). The old "first free slot"
         * logic kept stale entries when a program id was reused; the program cache keys on
         * these sources, so they must be right. */
        GLint ty = 0;
        glGetShaderiv(sh, GL_SHADER_TYPE, &ty);
        g_zw_prog_shaders[prog][ty == GL_FRAGMENT_SHADER ? 1 : 0] = sh;
    }
    zw_note_tree_shader(prog, sh);
    glAttachShader(prog, sh);
}
static void zw_dump_program_sources(GLuint prog) {
    /* v5.01: off by default - it created a directory and wrote a file on the memory card
     * for every program linked, which stalls a frame whenever a new shader appears. */
#ifndef ZW_SHADER_DUMP
#define ZW_SHADER_DUMP 0
#endif
    if (!ZW_SHADER_DUMP) return;
    if (prog >= 256) return;
    sceIoMkdir("ux0:/data/zombiewood/shaders", 0777);
    char path[96];
    snprintf(path, sizeof(path), "ux0:/data/zombiewood/shaders/prog_%03u.txt", prog);
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return;
    for (int k = 0; k < 2; k++) {
        GLuint sh = g_zw_prog_shaders[prog][k];
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "===== shader %u =====\n", sh);
        sceIoWrite(fd, hdr, strlen(hdr));
        if (sh < 512 && g_zw_shader_src[sh]) sceIoWrite(fd, g_zw_shader_src[sh], strlen(g_zw_shader_src[sh]));
        sceIoWrite(fd, "\n", 1);
    }
    sceIoClose(fd);
}
extern volatile uint64_t g_zw_prof_us[6];   /* v5.01 slow-frame profiler (main.c) */
extern volatile unsigned g_zw_prof_n[6];
#define ZW_PROF(slot, call) do { SceUInt64 _pt = sceKernelGetProcessTimeWide(); call; \
    g_zw_prof_us[slot] += sceKernelGetProcessTimeWide() - _pt; g_zw_prof_n[slot]++; } while (0)
static void tr_glCompileShader(GLuint shader) {
    static unsigned ok_count, fail_count;
    ZW_PROF(3, glCompileShader(shader));
    GLint st = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &st);
    if (st) { ok_count++; return; }
    fail_count++;
    if (fail_count <= 40) {
        char log[200]; GLsizei ln = 0; log[0] = '\0';
        glGetShaderInfoLog(shader, sizeof(log), &ln, log);
        for (char *p = log; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
        dynlib_logf("SHADER COMPILE FAIL #%u (ok so far %u) shader=%u log='%s'", fail_count, ok_count, shader, log);
        dynlib_logf("   src: %s", shader < 512 ? g_zw_shader_preview[shader] : "?");
    }
}
/* v5.02: program binary cache. VitaGL compiles GLSL at link time (~250 ms per program on
 * the Vita), so every effect seen for the first time stalled the game. Linked programs are
 * saved with glGetProgramBinary under ux0:data/zombiewood/progcache/<hash of both sources>
 * and restored with glProgramBinary (no compiler run) on later sessions. */
#ifndef ZW_PROG_CACHE
#define ZW_PROG_CACHE 1
#endif
/* v5.10: the binaries are VitaGL/compiler output, so the cache is versioned. BUMP
 * ZW_PROG_CACHE_VER whenever the SDK's VitaGL, its shader translation, or the GL wrappers
 * that change shader source or attribute binding (this file) change; the old folder is then
 * ignored and a fresh cache is built. */
#define ZW_PROG_CACHE_VER "1"
#define ZW_PROG_CACHE_ROOT "ux0:data/zombiewood/progcache"
#define ZW_PROG_CACHE_DIR ZW_PROG_CACHE_ROOT "/v" ZW_PROG_CACHE_VER
/* once per session: the v5.02-v5.09 cache sat unversioned in the root folder; delete those
 * files so they do not waste space (sub-folders, i.e. other versions, are left alone) */
static void zw_prog_cache_migrate(void) {
    static int done;
    if (done) return;
    done = 1;
    SceUID d = sceIoDopen(ZW_PROG_CACHE_ROOT);
    if (d < 0) return;
    SceIoDirent e; unsigned n = 0;
    while (sceIoDread(d, &e) > 0) {
        if (SCE_S_ISDIR(e.d_stat.st_mode)) continue;
        char p[160];
        snprintf(p, sizeof(p), ZW_PROG_CACHE_ROOT "/%s", e.d_name);
        if (sceIoRemove(p) >= 0) n++;
    }
    sceIoDclose(d);
    if (n) dynlib_logf("v5.10 progcache: removed %u unversioned files", n);
}
static uint64_t zw_fnv64(uint64_t h, const char *s) {
    for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 0x100000001b3ULL; }
    h ^= 0xff; h *= 0x100000001b3ULL;   /* separator so (a,b) != (ab,"") */
    return h;
}
static int zw_prog_cache_key(GLuint prog, char *path, size_t n) {
    if (prog >= 256) return 0;
    GLuint vs = g_zw_prog_shaders[prog][0], fs = g_zw_prog_shaders[prog][1];
    if (!vs || !fs || vs >= 512 || fs >= 512 || !g_zw_shader_src[vs] || !g_zw_shader_src[fs]) return 0;
    uint64_t h = zw_fnv64(0xcbf29ce484222325ULL, g_zw_shader_src[vs]);
    h = zw_fnv64(h, g_zw_shader_src[fs]);
    snprintf(path, n, ZW_PROG_CACHE_DIR "/%016llx.bin", (unsigned long long)h);
    return 1;
}
static int zw_prog_cache_load(GLuint prog, const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    int sz = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    void *buf = sz > 0 ? malloc(sz) : NULL;
    int ok = buf && sceIoRead(fd, buf, sz) == sz;
    sceIoClose(fd);
    if (ok) glProgramBinary(prog, 0, buf, sz);
    free(buf);
    if (!ok) return 0;
    GLint st = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &st);
    return st != 0;
}
static void zw_prog_cache_save(GLuint prog, const char *path) {
    const GLsizei cap = 256 * 1024;
    void *buf = malloc(cap);
    if (!buf) return;
    GLsizei len = 0; GLenum fmt = 0;
    glGetProgramBinary(prog, cap, &len, &fmt, buf);
    if (len > 0 && len <= cap) {
        zw_prog_cache_migrate();
        sceIoMkdir(ZW_PROG_CACHE_ROOT, 0777);
        sceIoMkdir(ZW_PROG_CACHE_DIR, 0777);
        char tmp[112]; snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        SceUID fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (fd >= 0) {
            int w = sceIoWrite(fd, buf, len);
            sceIoClose(fd);
            if (w == len) sceIoRename(tmp, path); else sceIoRemove(tmp);
        }
    }
    free(buf);
}
static void tr_glLinkProgram(GLuint prog) {
    static unsigned fail_count, hits, misses;
    char cpath[96];
    int keyed = ZW_PROG_CACHE && zw_prog_cache_key(prog, cpath, sizeof(cpath));
    if (keyed) {
        int hit;
        ZW_PROF(4, hit = zw_prog_cache_load(prog, cpath));
        if (hit) { hits++; return; }
    }
    ZW_PROF(4, glLinkProgram(prog));
    zw_dump_program_sources(prog);   /* v4.55 */
    GLint st = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &st);
    if (st && keyed) {
        misses++;
        zw_prog_cache_save(prog, cpath);
        if (misses <= 3 || (misses % 25) == 0)
            dynlib_logf("v5.02 progcache: saved #%u (hits so far %u) %s", misses, hits, cpath);
    }
    if (st) return;
    fail_count++;
    if (fail_count <= 40) {
        char log[200]; GLsizei ln = 0; log[0] = '\0';
        glGetProgramInfoLog(prog, sizeof(log), &ln, log);
        for (char *p = log; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
        dynlib_logf("PROGRAM LINK FAIL #%u prog=%u log='%s'", fail_count, prog, log);
    }
}

static int my_android_log_print(int prio,const char*tag,const char*fmt,...){
    (void)prio;
    (void)tag;
    (void)fmt;
    return 0;
}

static int my_android_log_vprint(int prio,const char*tag,const char*fmt,va_list ap){
    (void)prio;
    (void)tag;
    (void)fmt;
    (void)ap;
    return 0;
}

static int my_swprintf(wchar_t *dst, size_t maxlen, const wchar_t *fmt, ...){
    static unsigned int call_count = 0;
    if (dst && maxlen > 0) {
        dst[0] = L'\0';
    }

    if (call_count < 3) {
        dynlib_logf("swprintf suppressed: dst=%p maxlen=0x%08X fmt=%p",
                    dst, (unsigned)maxlen, fmt);
    } else if (call_count == 3) {
        dynlib_logf("swprintf repeated; suppressing further logs");
    }
    call_count++;
    return 0;
}

static void my_assert2(const char*file,int line,const char*func,const char*cond){
    static unsigned int assert_count = 0;
    if (assert_count < 3) {
        dynlib_logf("__assert2 failed: %s:%d %s",
                    file ? file : "(null)",
                    line,
                    func ? func : "(null)");
    } else if (assert_count == 3) {
        dynlib_logf("__assert2 repeated; suppressing further assert logs");
    }
    assert_count++;

    /* Android release builds often keep assertion calls in third-party libs.
       Treat them as diagnostics while porting so one noisy library check does
       not close the Vita app before we can see the next runtime state. */
    (void)cond;
}

static int my_clock_gettime(int clk,struct timespec*ts){
    SceKernelSysClock t; sceKernelGetProcessTime(&t);
    ts->tv_sec=t/1000000ULL; ts->tv_nsec=(t%1000000ULL)*1000;
    (void)clk; return 0;
}

static int my_strcoll(const char *a, const char *b){
    if(!a) a="";
    if(!b) b="";
    return strcmp(a,b);
}

static size_t my_strxfrm(char *dst,const char *src,size_t n){
    if(!src) src="";
    size_t len=strlen(src);
    if(dst && n>0){
        size_t copy=len < (n-1) ? len : (n-1);
        memcpy(dst,src,copy);
        dst[copy]='\0';
    }
    return len;
}

#define TLS_KEY_MAX 64
static void *g_tls_values[TLS_KEY_MAX];
static void (*g_tls_dtors[TLS_KEY_MAX])(void *);
static unsigned int g_tls_key_used[TLS_KEY_MAX];
static unsigned int g_tls_next_key = 1;
static int g_tls_default_word = 0;

typedef struct {
    pthread_attr_t *real_ptr;
    int32_t magic;
    size_t stack_size;
    size_t guard_size;
    int32_t sched_policy;
    int32_t sched_priority;
} android_pthread_attr_t;

typedef struct {
    pthread_mutex_t *real_ptr;
} android_pthread_mutex_t;

typedef struct {
    pthread_cond_t *real_ptr;
} android_pthread_cond_t;

#define ANDROID_PTHREAD_MAGIC 0x42424242
#define PTHREAD_OBJECT_MAX 512
#define BIONIC_PTHREAD_MUTEX_RECURSIVE_INITIALIZER  0x4000
#define BIONIC_PTHREAD_MUTEX_ERRORCHECK_INITIALIZER 0x8000
#define BIONIC_PTHREAD_MUTEX_RECURSIVE 1
#define BIONIC_PTHREAD_MUTEX_ERRORCHECK 2

static void *g_pthread_objects[PTHREAD_OBJECT_MAX];
static SceKernelLwMutexWork g_pthread_registry_lock;
static int g_pthread_registry_lock_inited = 0;

static void pthread_registry_lock(void) {
    if (!g_pthread_registry_lock_inited) {
        if (sceKernelCreateLwMutex(&g_pthread_registry_lock, "zw_pthr_registry", 0, 0, NULL) >= 0) {
            g_pthread_registry_lock_inited = 1;
        }
    }
    if (g_pthread_registry_lock_inited) {
        sceKernelLockLwMutex(&g_pthread_registry_lock, 1, NULL);
    }
}

static void pthread_registry_unlock(void) {
    if (g_pthread_registry_lock_inited) {
        sceKernelUnlockLwMutex(&g_pthread_registry_lock, 1);
    }
}

static int pthread_object_is_known(const void *obj) {
    if (!obj) return 0;
    int found = 0;
    pthread_registry_lock();
    for (int i = 0; i < PTHREAD_OBJECT_MAX; i++) {
        if (g_pthread_objects[i] == obj) {
            found = 1;
            break;
        }
    }
    pthread_registry_unlock();
    return found;
}

static void pthread_object_remember(void *obj) {
    if (!obj || pthread_object_is_known(obj)) return;
    pthread_registry_lock();
    for (int i = 0; i < PTHREAD_OBJECT_MAX; i++) {
        if (!g_pthread_objects[i]) {
            g_pthread_objects[i] = obj;
            break;
        }
    }
    pthread_registry_unlock();
}

static void pthread_object_forget(const void *obj) {
    if (!obj) return;
    pthread_registry_lock();
    for (int i = 0; i < PTHREAD_OBJECT_MAX; i++) {
        if (g_pthread_objects[i] == obj) {
            g_pthread_objects[i] = NULL;
            break;
        }
    }
    pthread_registry_unlock();
}

static int my_pthread_key_create(pthread_key_t *key, void (*destructor)(void *)){
    if (!key) return EINVAL;
    if (g_tls_next_key >= TLS_KEY_MAX) return EAGAIN;

    unsigned int k = g_tls_next_key++;
    g_tls_key_used[k] = 1;
    g_tls_values[k] = NULL;
    g_tls_dtors[k] = destructor;
    *key = (pthread_key_t)k;
    dynlib_logf("pthread_key_create -> %u", k);
    return 0;
}

static int my_pthread_setspecific(pthread_key_t key, const void *value){
    unsigned int k = (unsigned int)(uintptr_t)key;
    if (k >= TLS_KEY_MAX || !g_tls_key_used[k]) return EINVAL;

    g_tls_values[k] = (void *)value;
    return 0;
}

static void *my_pthread_getspecific(pthread_key_t key){
    unsigned int k = (unsigned int)(uintptr_t)key;
    if (k < TLS_KEY_MAX && g_tls_key_used[k]) {
        return g_tls_values[k];
    }

    /* Some GLF paths query mNativeTls before the Android thread bootstrap
       has created the pthread key. Return a readable zero word instead of
       letting the game dereference NULL in GetSequentialThreadId(). */
    return &g_tls_default_word;
}

static int my_pthread_key_delete(pthread_key_t key){
    unsigned int k = (unsigned int)(uintptr_t)key;
    if (k >= TLS_KEY_MAX || !g_tls_key_used[k]) return EINVAL;

    g_tls_key_used[k] = 0;
    g_tls_values[k] = NULL;
    g_tls_dtors[k] = NULL;
    return 0;
}

static int my_pthread_attr_init(android_pthread_attr_t *attr) {
    if (!attr) return EINVAL;
    if (attr->magic != ANDROID_PTHREAD_MAGIC || !attr->real_ptr) {
        memset(attr, 0, sizeof(*attr));
        attr->real_ptr = malloc(sizeof(pthread_attr_t));
        if (!attr->real_ptr) return ENOMEM;
        int ret = pthread_attr_init(attr->real_ptr);
        if (ret != 0) {
            free(attr->real_ptr);
            attr->real_ptr = NULL;
            return ret;
        }
        attr->magic = ANDROID_PTHREAD_MAGIC;
    }
    return 0;
}

static int my_pthread_attr_destroy(android_pthread_attr_t *attr) {
    if (!attr) return 0;
    if (attr->magic != ANDROID_PTHREAD_MAGIC || !attr->real_ptr) return 0;
    int ret = pthread_attr_destroy(attr->real_ptr);
    free(attr->real_ptr);
    attr->real_ptr = NULL;
    attr->magic = 0;
    return ret;
}

static int my_pthread_attr_setdetachstate(android_pthread_attr_t *attr, int state) {
    int ret = my_pthread_attr_init(attr);
    if (ret != 0) return ret;
    return pthread_attr_setdetachstate(attr->real_ptr, state);
}

static int my_pthread_attr_setstacksize(android_pthread_attr_t *attr, size_t stacksize) {
    int ret = my_pthread_attr_init(attr);
    if (ret != 0) return ret;
    if (stacksize < 512 * 1024) stacksize = 512 * 1024;
    return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}

static int my_pthread_create(pthread_t *thread, const android_pthread_attr_t *attr,
                             void *(*start)(void *), void *param) {
    if (!thread || !start) return EINVAL;
    uintptr_t start_addr = (uintptr_t)start & ~(uintptr_t)1;
    dynlib_logf("pthread_create(start=%p,param=%p)", start, param);

    /* MC3-style service cutout: Gameloft's tracker/error/online helpers spawn
       background threads that are irrelevant offline and unstable under this
       loader. Keep game/GLF worker threads, but do not launch telemetry ones. */
    if ((start_addr >= 0x98750000 && start_addr < 0x98771000) ||
        start_addr == 0x986928e0 ||
        (start_addr >= 0x989FE000 && start_addr < 0x98A09000)) {
        dynlib_logf("pthread_create suppressed Gameloft service thread start=%p", start);
        memset(thread, 0, sizeof(*thread));
        return EAGAIN;
    }

    int ret;
    if (attr) {
        android_pthread_attr_t *mutable_attr = (android_pthread_attr_t *)attr;
        ret = my_pthread_attr_init(mutable_attr);
        if (ret != 0) return ret;
        pthread_attr_setstacksize(mutable_attr->real_ptr, 512 * 1024);
        ret = pthread_create(thread, mutable_attr->real_ptr, start, param);
    } else {
        pthread_attr_t vita_attr;
        pthread_attr_init(&vita_attr);
        pthread_attr_setstacksize(&vita_attr, 512 * 1024);
        ret = pthread_create(thread, &vita_attr, start, param);
        pthread_attr_destroy(&vita_attr);
    }
    dynlib_logf("pthread_create(start=%p) -> %d thread=%p", start, ret,
                ret == 0 ? (void *)(uintptr_t)(*thread) : NULL);
    return ret;
}

static int my_pthread_mutexattr_init(void *attr){
    if (attr) {
        *(uint32_t *)attr = 0;
    }
    return 0;
}

static int my_pthread_mutexattr_destroy(void *attr){
    (void)attr;
    return 0;
}

static int my_pthread_mutexattr_settype(void *attr, int type){
    if (!attr) return EINVAL;
    int vita_type = PTHREAD_MUTEX_NORMAL;
    if (type == BIONIC_PTHREAD_MUTEX_RECURSIVE) {
        vita_type = PTHREAD_MUTEX_RECURSIVE;
    } else if (type == BIONIC_PTHREAD_MUTEX_ERRORCHECK) {
        vita_type = PTHREAD_MUTEX_ERRORCHECK;
    }
    return pthread_mutexattr_settype((pthread_mutexattr_t *)attr, vita_type);
}

static int my_pthread_mutex_init(android_pthread_mutex_t *mutex, const pthread_mutexattr_t *attr){
    if (!mutex) return EINVAL;
    if (pthread_object_is_known(mutex) && mutex->real_ptr) return 0;

    int kind = PTHREAD_MUTEX_NORMAL;
    if (attr) {
        pthread_mutexattr_gettype((pthread_mutexattr_t *)attr, &kind);
    } else {
        uint32_t raw = *(uint32_t *)mutex;
        if (raw == BIONIC_PTHREAD_MUTEX_RECURSIVE_INITIALIZER) kind = PTHREAD_MUTEX_RECURSIVE;
        else if (raw == BIONIC_PTHREAD_MUTEX_ERRORCHECK_INITIALIZER) kind = PTHREAD_MUTEX_ERRORCHECK;
    }

    pthread_mutex_t *real = malloc(sizeof(pthread_mutex_t));
    if (!real) return ENOMEM;

    pthread_mutexattr_t vita_attr;
    pthread_mutexattr_init(&vita_attr);
    pthread_mutexattr_settype(&vita_attr, kind);
    int ret = pthread_mutex_init(real, &vita_attr);
    pthread_mutexattr_destroy(&vita_attr);
    if (ret != 0) {
        free(real);
        return ret;
    }

    mutex->real_ptr = real;
    pthread_object_remember(mutex);
    return 0;
}

static int my_pthread_mutex_destroy(android_pthread_mutex_t *mutex){
    if (!mutex) return 0;
    if (!mutex->real_ptr || !pthread_object_is_known(mutex)) return 0;
    int ret = pthread_mutex_destroy(mutex->real_ptr);
    free(mutex->real_ptr);
    mutex->real_ptr = NULL;
    pthread_object_forget(mutex);
    return ret;
}

static int my_pthread_mutex_lock(android_pthread_mutex_t *mutex){
    if (!mutex) return EINVAL;
    int ret = my_pthread_mutex_init(mutex, NULL);
    if (ret != 0) return ret;
    return pthread_mutex_lock(mutex->real_ptr);
}

static int my_pthread_mutex_trylock(android_pthread_mutex_t *mutex){
    if (!mutex) return EINVAL;
    int ret = my_pthread_mutex_init(mutex, NULL);
    if (ret != 0) return ret;
    return pthread_mutex_trylock(mutex->real_ptr);
}

static int my_pthread_mutex_unlock(android_pthread_mutex_t *mutex){
    if (!mutex || !mutex->real_ptr) return EINVAL;
    return pthread_mutex_unlock(mutex->real_ptr);
}

static int my_pthread_cond_init(android_pthread_cond_t *cond, const pthread_condattr_t *attr){
    if (!cond) return EINVAL;
    if (pthread_object_is_known(cond) && cond->real_ptr) return 0;

    pthread_cond_t *real = malloc(sizeof(pthread_cond_t));
    if (!real) return ENOMEM;
    int ret = pthread_cond_init(real, attr);
    if (ret != 0) {
        free(real);
        return ret;
    }

    cond->real_ptr = real;
    pthread_object_remember(cond);
    return 0;
}

static int my_pthread_cond_destroy(android_pthread_cond_t *cond){
    if (!cond) return 0;
    if (!cond->real_ptr || !pthread_object_is_known(cond)) return 0;
    int ret = pthread_cond_destroy(cond->real_ptr);
    free(cond->real_ptr);
    cond->real_ptr = NULL;
    pthread_object_forget(cond);
    return ret;
}

static int my_pthread_cond_signal(android_pthread_cond_t *cond){
    if (!cond) return EINVAL;
    int ret = my_pthread_cond_init(cond, NULL);
    if (ret != 0) return ret;
    return pthread_cond_signal(cond->real_ptr);
}

static int my_pthread_cond_broadcast(android_pthread_cond_t *cond){
    if (!cond) return EINVAL;
    int ret = my_pthread_cond_init(cond, NULL);
    if (ret != 0) return ret;
    return pthread_cond_broadcast(cond->real_ptr);
}

static int my_pthread_cond_wait(android_pthread_cond_t *cond, android_pthread_mutex_t *mutex){
    if (!cond || !mutex) return EINVAL;
    int ret = my_pthread_cond_init(cond, NULL);
    if (ret != 0) return ret;
    ret = my_pthread_mutex_init(mutex, NULL);
    if (ret != 0) return ret;
    return pthread_cond_wait(cond->real_ptr, mutex->real_ptr);
}

static int my_pthread_cond_timedwait(android_pthread_cond_t *cond, android_pthread_mutex_t *mutex, const struct timespec *abstime){
    if (!cond || !mutex) return EINVAL;
    int ret = my_pthread_cond_init(cond, NULL);
    if (ret != 0) return ret;
    ret = my_pthread_mutex_init(mutex, NULL);
    if (ret != 0) return ret;
    return pthread_cond_timedwait(cond->real_ptr, mutex->real_ptr, abstime);
}

static int my_pthread_once(volatile int *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine) return EINVAL;
    if (__sync_lock_test_and_set(once_control, 1) == 0) {
        init_routine();
    }
    return 0;
}

/*
 * v4.44: softfp <-> hardfp bridges.
 * libZombieWood.so is softfp (armeabi-v7a: float/double args and returns in
 * r0-r3), this loader is hard-float (VFP registers). Mapping float-taking
 * imports straight to newlib/VitaGL made them read and return garbage.
 * pcs("aapcs") makes each wrapper use the base (softfp) convention on its
 * boundary while calling the real hard-float function inside.
 */
#define SFP __attribute__((pcs("aapcs")))
#define SFP_D1(n)  static SFP double sfp_##n(double x) { return n(x); }
#define SFP_F1(n)  static SFP float  sfp_##n(float x)  { return n(x); }
#define SFP_D2(n)  static SFP double sfp_##n(double x, double y) { return n(x, y); }
#define SFP_F2(n)  static SFP float  sfp_##n(float x, float y)   { return n(x, y); }
SFP_D1(acos) SFP_D1(asin) SFP_D1(atan) SFP_D1(ceil) SFP_D1(cos) SFP_D1(cosh)
SFP_D1(exp) SFP_D1(floor) SFP_D1(log) SFP_D1(log10) SFP_D1(sin) SFP_D1(sinh)
SFP_D1(tan) SFP_D1(tanh)
SFP_F1(acosf) SFP_F1(asinf) SFP_F1(atanf) SFP_F1(ceilf) SFP_F1(cosf)
SFP_F1(expf) SFP_F1(floorf) SFP_F1(logf) SFP_F1(sinf) SFP_F1(tanf)
SFP_D2(atan2) SFP_D2(fmod) SFP_D2(pow)
SFP_F2(atan2f) SFP_F2(fmodf) SFP_F2(powf)
static SFP double sfp_ldexp(double x, int e)       { return ldexp(x, e); }
static SFP float  sfp_ldexpf(float x, int e)       { return ldexpf(x, e); }
static SFP float  sfp_frexpf(float x, int *e)      { return frexpf(x, e); }
static SFP float  sfp_modff(float x, float *ip)    { return modff(x, ip); }
static SFP double sfp_strtod(const char *s, char **end) { return strtod(s, end); }
static SFP double sfp_difftime(time_t a, time_t b) { return difftime(a, b); }

static SFP void sfp_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { GLT("gl ClearColor %.3f %.3f %.3f %.3f", r, g, b, a); glClearColor(r, g, b, a); }
static SFP void sfp_glClearDepthf(GLclampf d)                 { DSLOG("gl ClearDepthf %f", d); glClearDepthf(d); }
static SFP void sfp_glDepthRangef(GLclampf n, GLclampf f)     { DSLOG("gl DepthRangef %f %f", n, f); glDepthRangef(n, f); }
static SFP void sfp_glLineWidth(GLfloat w)                    { glLineWidth(w); }
static SFP void sfp_glPolygonOffset(GLfloat f, GLfloat u)     { glPolygonOffset(f, u); }
static SFP void sfp_glTexParameterf(GLenum t, GLenum p, GLfloat v) { glTexParameterf(t, p, v); }
static SFP void sfp_glUniform1f(GLint loc, GLfloat v)         { GLT("gl Uniform1f loc=%d v=%g", loc, (double)v); glUniform1f(loc, v); }
static SFP void sfp_glVertexAttrib4f(GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { glVertexAttrib4f(i, x, y, z, w); }

so_default_dynlib g_default_dynlib[] = {
    { "__android_log_print", (uintptr_t)my_android_log_print },
    { "__android_log_vprint", (uintptr_t)my_android_log_vprint },
    { "__assert2", (uintptr_t)my_assert2 },
    { "__cxa_atexit", (uintptr_t)my_cxa_atexit },
    { "__cxa_finalize", (uintptr_t)my_cxa_finalize },
    { "__cxa_guard_abort", (uintptr_t)my_cxa_guard_abort },
    { "__cxa_guard_acquire", (uintptr_t)my_cxa_guard_acquire },
    { "__cxa_guard_release", (uintptr_t)my_cxa_guard_release },
    { "__cxa_pure_virtual", (uintptr_t)my_abort },
    { "__errno", (uintptr_t)__errno },
    { "__gnu_Unwind_Find_exidx", (uintptr_t)stub_ptr_null },
    { "__sF", (uintptr_t)g_android_sF },
    { "__stack_chk_guard", (uintptr_t)&my_stack_chk_guard },
    { "__stack_chk_fail", (uintptr_t)my_stack_chk_fail },
    { "_ctype_", (uintptr_t)&g_android_ctype_ptr },
    { "_tolower_tab_", (uintptr_t)&g_android_tolower_tab_ptr },   /* v4.96 */
    { "SL_IID_ENGINE", (uintptr_t)&zw_SL_IID_ENGINE },             /* v4.99 audio_sl.c */
    { "SL_IID_PLAY", (uintptr_t)&zw_SL_IID_PLAY },
    { "SL_IID_BUFFERQUEUE", (uintptr_t)&zw_SL_IID_BUFFERQUEUE },
    { "_toupper_tab_", (uintptr_t)&g_android_toupper_tab_ptr },   /* v4.96 */
    { "abort", (uintptr_t)my_abort },
    { "accept", (uintptr_t)stub_int_neg1 },
    { "acos", (uintptr_t)sfp_acos },
    { "acosf", (uintptr_t)sfp_acosf },
    { "asin", (uintptr_t)sfp_asin },
    { "asinf", (uintptr_t)sfp_asinf },
    { "atan", (uintptr_t)sfp_atan },
    { "atan2", (uintptr_t)sfp_atan2 },
    { "atan2f", (uintptr_t)sfp_atan2f },
    { "atanf", (uintptr_t)sfp_atanf },
    { "atoi", (uintptr_t)atoi },
    { "atol", (uintptr_t)atol },
    { "bind", (uintptr_t)stub_int_neg1 },
    { "bsd_signal", (uintptr_t)stub_ptr_null },
    { "btowc", (uintptr_t)btowc },
    { "calloc", (uintptr_t)calloc },
    { "ceil", (uintptr_t)sfp_ceil },
    { "ceilf", (uintptr_t)sfp_ceilf },
    { "chdir", (uintptr_t)chdir },
    { "chmod", (uintptr_t)chmod },
    { "clearerr", (uintptr_t)vita_clearerr },
    { "clock", (uintptr_t)clock },
    { "clock_gettime", (uintptr_t)my_clock_gettime },
    { "close", (uintptr_t)vita_close },
    { "closedir", (uintptr_t)closedir },
    { "connect", (uintptr_t)stub_int_neg1 },
    { "cos", (uintptr_t)sfp_cos },
    { "cosf", (uintptr_t)sfp_cosf },
    { "cosh", (uintptr_t)sfp_cosh },
    { "difftime", (uintptr_t)sfp_difftime },
    { "exit", (uintptr_t)my_exit },
    { "exp", (uintptr_t)sfp_exp },
    { "expf", (uintptr_t)sfp_expf },
    { "fclose", (uintptr_t)vita_fclose },
    { "fcntl", (uintptr_t)stub_int_neg1 },
    { "fdopen", (uintptr_t)fdopen },
    { "feof", (uintptr_t)vita_feof },
    { "ferror", (uintptr_t)vita_ferror },
    { "fgetc", (uintptr_t)vita_fgetc },
    { "fflush", (uintptr_t)vita_fflush },
    { "fgets", (uintptr_t)vita_fgets },
    { "fileno", (uintptr_t)vita_fileno },
    { "floor", (uintptr_t)sfp_floor },
    { "floorf", (uintptr_t)sfp_floorf },
    { "fmod", (uintptr_t)sfp_fmod },
    { "fmodf", (uintptr_t)sfp_fmodf },
    { "fopen", (uintptr_t)vita_fopen },
    { "fprintf", (uintptr_t)vita_fprintf },
    { "fputc", (uintptr_t)vita_fputc },
    { "fputs", (uintptr_t)vita_fputs },
    { "fread", (uintptr_t)vita_fread },
    { "free", (uintptr_t)free },
    { "freeaddrinfo", (uintptr_t)stub_int_neg1 },
    { "freopen", (uintptr_t)freopen },
    { "frexpf", (uintptr_t)sfp_frexpf },
    { "fscanf", (uintptr_t)fscanf },
    { "fseek", (uintptr_t)vita_fseek },
    { "fstat", (uintptr_t)vita_fstat },
    { "fsync", (uintptr_t)stub_int_neg1 },
    { "ftell", (uintptr_t)vita_ftell },
    { "fwrite", (uintptr_t)vita_fwrite },
    { "getaddrinfo", (uintptr_t)stub_int_neg1 },
    { "getc", (uintptr_t)vita_getc },
    { "getcwd", (uintptr_t)getcwd },
    { "getegid", (uintptr_t)stub_int_zero },
    { "getenv", (uintptr_t)getenv },
    { "geteuid", (uintptr_t)stub_int_zero },
    { "getgid", (uintptr_t)stub_int_zero },
    { "gethostbyaddr", (uintptr_t)stub_ptr_null },
    { "gethostbyname", (uintptr_t)stub_ptr_null },
    { "gethostname", (uintptr_t)stub_int_neg1 },
    { "getpeername", (uintptr_t)stub_int_neg1 },
    { "getpid", (uintptr_t)stub_int_zero },
    { "getpwuid", (uintptr_t)stub_int_neg1 },
    { "getsockname", (uintptr_t)stub_int_neg1 },
    { "getsockopt", (uintptr_t)stub_int_neg1 },
    { "gettimeofday", (uintptr_t)gettimeofday },
    { "getuid", (uintptr_t)stub_int_zero },
    { "getwc", (uintptr_t)vita_getwc },
    { "glActiveTexture", (uintptr_t)tr_glActiveTexture },
    { "glAttachShader", (uintptr_t)tr_glAttachShader },
    { "glBindBuffer", (uintptr_t)tr_glBindBuffer },
    { "glBindFramebuffer", (uintptr_t)tr_glBindFramebuffer },
    { "glBindRenderbuffer", (uintptr_t)glBindRenderbuffer },
    { "glBindTexture", (uintptr_t)tr_glBindTexture },
    { "glBlendColor", (uintptr_t)my_gl_noop },
    { "glBlendEquation", (uintptr_t)glBlendEquation },
    { "glBlendFunc", (uintptr_t)tr_glBlendFunc },
    { "glBufferData", (uintptr_t)glBufferData },
    { "glBufferSubData", (uintptr_t)glBufferSubData },
    { "glCheckFramebufferStatus", (uintptr_t)glCheckFramebufferStatus },
    { "glClear", (uintptr_t)tr_glClear },
    { "glClearColor", (uintptr_t)sfp_glClearColor },
    { "glClearDepthf", (uintptr_t)sfp_glClearDepthf },
    { "glClearStencil", (uintptr_t)tr_glClearStencil },
    { "glColorMask", (uintptr_t)tr_glColorMask },
    { "glCompileShader", (uintptr_t)tr_glCompileShader },
    { "glCompressedTexImage2D", (uintptr_t)tr_glCompressedTexImage2D },
    { "glCompressedTexImage3DOES", (uintptr_t)my_gl_noop },
    { "glCompressedTexSubImage2D", (uintptr_t)my_gl_noop },
    { "glCompressedTexSubImage3DOES", (uintptr_t)my_gl_noop },
    { "glCopyTexSubImage2D", (uintptr_t)glCopyTexSubImage2D },
    { "glCreateProgram", (uintptr_t)glCreateProgram },
    { "glCreateShader", (uintptr_t)glCreateShader },
    { "glCullFace", (uintptr_t)glCullFace },
    { "glDeleteBuffers", (uintptr_t)glDeleteBuffers },
    { "glDeleteFramebuffers", (uintptr_t)glDeleteFramebuffers },
    { "glDeleteProgram", (uintptr_t)glDeleteProgram },
    { "glDeleteRenderbuffers", (uintptr_t)glDeleteRenderbuffers },
    { "glDeleteShader", (uintptr_t)glDeleteShader },
    { "glDeleteTextures", (uintptr_t)glDeleteTextures },
    { "glDepthFunc", (uintptr_t)tr_glDepthFunc },
    { "glDepthMask", (uintptr_t)tr_glDepthMask },
    { "glDepthRangef", (uintptr_t)sfp_glDepthRangef },
    { "glDisable", (uintptr_t)tr_glDisable },
    { "glDisableVertexAttribArray", (uintptr_t)tr_glDisableVertexAttribArray },
    { "glDrawArrays", (uintptr_t)my_gl_draw_arrays },
    { "glDrawElements", (uintptr_t)my_gl_draw_elements },
    { "glEnable", (uintptr_t)tr_glEnable },
    { "glEnableVertexAttribArray", (uintptr_t)tr_glEnableVertexAttribArray },
    { "glFinish", (uintptr_t)glFinish },
    { "glFlush", (uintptr_t)glFlush },
    { "glFramebufferRenderbuffer", (uintptr_t)glFramebufferRenderbuffer },
    { "glFramebufferTexture2D", (uintptr_t)glFramebufferTexture2D },
    { "glFrontFace", (uintptr_t)glFrontFace },
    { "glGenBuffers", (uintptr_t)glGenBuffers },
    { "glGenFramebuffers", (uintptr_t)glGenFramebuffers },
    { "glGenRenderbuffers", (uintptr_t)glGenRenderbuffers },
    { "glGenTextures", (uintptr_t)glGenTextures },
    { "glGenerateMipmap", (uintptr_t)glGenerateMipmap },
    { "glGetActiveAttrib", (uintptr_t)glGetActiveAttrib },
    { "glGetActiveUniform", (uintptr_t)tr_glGetActiveUniform },
    { "glGetAttribLocation", (uintptr_t)tr_glGetAttribLocation },
    { "glGetError", (uintptr_t)glGetError },
    { "glGetFloatv", (uintptr_t)glGetFloatv },
    { "glGetIntegerv", (uintptr_t)glGetIntegerv },
    { "glGetProgramInfoLog", (uintptr_t)glGetProgramInfoLog },
    { "glGetProgramiv", (uintptr_t)glGetProgramiv },
    { "glGetShaderInfoLog", (uintptr_t)glGetShaderInfoLog },
    { "glGetShaderSource", (uintptr_t)glGetShaderSource },
    { "glGetShaderiv", (uintptr_t)glGetShaderiv },
    { "glGetString", (uintptr_t)glGetString },
    { "glGetUniformLocation", (uintptr_t)tr_glGetUniformLocation },
    { "glLineWidth", (uintptr_t)sfp_glLineWidth },
    { "glLinkProgram", (uintptr_t)tr_glLinkProgram },
    { "glMapBufferOES", (uintptr_t)my_gl_map_buffer_oes },
    { "glPixelStorei", (uintptr_t)glPixelStorei },
    { "glPolygonOffset", (uintptr_t)sfp_glPolygonOffset },
    { "glReadPixels", (uintptr_t)glReadPixels },
    { "glRenderbufferStorage", (uintptr_t)glRenderbufferStorage },
    { "glSampleCoverage", (uintptr_t)my_gl_noop },
    { "glScissor", (uintptr_t)tr_glScissor },
    { "glShaderSource", (uintptr_t)my_gl_shader_source },
    { "glStencilFunc", (uintptr_t)tr_glStencilFunc },
    { "glStencilMask", (uintptr_t)tr_glStencilMask },
    { "glStencilOp", (uintptr_t)tr_glStencilOp },
    { "glTexImage2D", (uintptr_t)tr_glTexImage2D },
    { "glTexImage3DOES", (uintptr_t)my_gl_noop },
    { "glTexParameterf", (uintptr_t)sfp_glTexParameterf },
    { "glTexParameteri", (uintptr_t)glTexParameteri },
    { "glTexSubImage2D", (uintptr_t)my_glTexSubImage2D },
    { "glTexSubImage3DOES", (uintptr_t)my_gl_noop },
    { "glUniform1f", (uintptr_t)sfp_glUniform1f },
    { "glUniform1fv", (uintptr_t)tr_glUniform1fv },
    { "glUniform1i", (uintptr_t)tr_glUniform1i },
    { "glUniform1iv", (uintptr_t)tr_glUniform1iv },
    { "glUniform2fv", (uintptr_t)tr_glUniform2fv },
    { "glUniform2iv", (uintptr_t)glUniform2iv },
    { "glUniform3fv", (uintptr_t)tr_glUniform3fv },
    { "glUniform3iv", (uintptr_t)glUniform3iv },
    { "glUniform4fv", (uintptr_t)tr_glUniform4fv },
    { "glUniform4iv", (uintptr_t)glUniform4iv },
    { "glUniformMatrix4fv", (uintptr_t)tr_glUniformMatrix4fv },
    { "glUnmapBufferOES", (uintptr_t)my_gl_unmap_buffer_oes },
    { "glUseProgram", (uintptr_t)tr_glUseProgram },
    { "glVertexAttrib4f", (uintptr_t)sfp_glVertexAttrib4f },
    { "glVertexAttribPointer", (uintptr_t)tr_glVertexAttribPointer },
    { "glViewport", (uintptr_t)tr_glViewport },
    { "gmtime", (uintptr_t)gmtime },
    { "gmtime_r", (uintptr_t)gmtime_r },
    { "inet_addr", (uintptr_t)stub_int_neg1 },
    { "inet_ntoa", (uintptr_t)stub_int_neg1 },
    { "ioctl", (uintptr_t)stub_int_neg1 },
    { "isalnum", (uintptr_t)isalnum },
    { "isalpha", (uintptr_t)isalpha },
    { "iscntrl", (uintptr_t)iscntrl },
    { "islower", (uintptr_t)islower },
    { "ispunct", (uintptr_t)ispunct },
    { "isspace", (uintptr_t)isspace },
    { "isupper", (uintptr_t)isupper },
    { "iswctype", (uintptr_t)iswctype },
    { "isxdigit", (uintptr_t)isxdigit },
    { "ldexp", (uintptr_t)sfp_ldexp },
    { "ldexpf", (uintptr_t)sfp_ldexpf },
    { "listen", (uintptr_t)stub_int_neg1 },
    { "localtime", (uintptr_t)localtime },
    { "localtime_r", (uintptr_t)localtime_r },
    { "log", (uintptr_t)sfp_log },
    { "log10", (uintptr_t)sfp_log10 },
    { "logf", (uintptr_t)sfp_logf },
    { "longjmp", (uintptr_t)longjmp },
    { "lrand48", (uintptr_t)lrand48 },
    { "lseek", (uintptr_t)lseek },
    { "malloc", (uintptr_t)malloc },
    { "mbrtowc", (uintptr_t)mbrtowc },
    { "memchr", (uintptr_t)memchr },
    { "memcmp", (uintptr_t)memcmp },
    { "memcpy", (uintptr_t)memcpy },
    { "memmove", (uintptr_t)memmove },
    { "memset", (uintptr_t)memset },
    { "mkdir", (uintptr_t)vita_mkdir },
    { "mktime", (uintptr_t)mktime },
    { "modff", (uintptr_t)sfp_modff },
    { "open", (uintptr_t)vita_open },
    { "opendir", (uintptr_t)vita_opendir },
    { "perror", (uintptr_t)stub_void },
    { "poll", (uintptr_t)stub_int_neg1 },
    { "pow", (uintptr_t)sfp_pow },
    { "powf", (uintptr_t)sfp_powf },
    { "pread", (uintptr_t)pread },
    { "printf", (uintptr_t)printf },
    { "pthread_attr_destroy", (uintptr_t)my_pthread_attr_destroy },
    { "pthread_attr_init", (uintptr_t)my_pthread_attr_init },
    { "pthread_attr_setdetachstate", (uintptr_t)my_pthread_attr_setdetachstate },
    { "pthread_attr_setstacksize", (uintptr_t)my_pthread_attr_setstacksize },
    { "pthread_cond_broadcast", (uintptr_t)my_pthread_cond_broadcast },
    { "pthread_cond_destroy", (uintptr_t)my_pthread_cond_destroy },
    { "pthread_cond_init", (uintptr_t)my_pthread_cond_init },
    { "pthread_cond_signal", (uintptr_t)my_pthread_cond_signal },
    { "pthread_cond_timedwait", (uintptr_t)my_pthread_cond_timedwait },
    { "pthread_cond_wait", (uintptr_t)my_pthread_cond_wait },
    { "pthread_create", (uintptr_t)my_pthread_create },
    { "pthread_equal", (uintptr_t)pthread_equal },
    { "pthread_getspecific", (uintptr_t)my_pthread_getspecific },
    { "pthread_join", (uintptr_t)pthread_join },
    { "pthread_key_create", (uintptr_t)my_pthread_key_create },
    { "pthread_key_delete", (uintptr_t)my_pthread_key_delete },
    { "pthread_mutex_destroy", (uintptr_t)my_pthread_mutex_destroy },
    { "pthread_mutex_init", (uintptr_t)my_pthread_mutex_init },
    { "pthread_mutex_lock", (uintptr_t)my_pthread_mutex_lock },
    { "pthread_mutex_trylock", (uintptr_t)my_pthread_mutex_trylock },
    { "pthread_mutex_unlock", (uintptr_t)my_pthread_mutex_unlock },
    { "pthread_mutexattr_destroy", (uintptr_t)my_pthread_mutexattr_destroy },
    { "pthread_mutexattr_init", (uintptr_t)my_pthread_mutexattr_init },
    { "pthread_mutexattr_settype", (uintptr_t)my_pthread_mutexattr_settype },
    { "pthread_once", (uintptr_t)my_pthread_once },
    { "pthread_self", (uintptr_t)pthread_self },
    { "pthread_setspecific", (uintptr_t)my_pthread_setspecific },
    { "putc", (uintptr_t)vita_putc },
    { "putchar", (uintptr_t)putchar },
    { "puts", (uintptr_t)puts },
    { "putwc", (uintptr_t)putwc },
    { "qsort", (uintptr_t)vita_qsort },
    { "raise", (uintptr_t)stub_int_neg1 },
    { "read", (uintptr_t)read },
    { "readdir", (uintptr_t)vita_readdir },
    { "realloc", (uintptr_t)realloc },
    { "recv", (uintptr_t)stub_int_neg1 },
    { "recvfrom", (uintptr_t)stub_int_neg1 },
    { "remove", (uintptr_t)vita_remove },
    { "rename", (uintptr_t)vita_rename },
    { "rewind", (uintptr_t)vita_rewind },
    { "sched_yield", (uintptr_t)stub_int_zero },
    { "select", (uintptr_t)stub_int_neg1 },
    { "send", (uintptr_t)stub_int_neg1 },
    { "sendto", (uintptr_t)stub_int_neg1 },
    { "setjmp", (uintptr_t)setjmp },
    { "setlocale", (uintptr_t)vita_setlocale },
    { "setsockopt", (uintptr_t)stub_int_neg1 },
    { "setvbuf", (uintptr_t)setvbuf },
    { "shutdown", (uintptr_t)stub_int_neg1 },
    { "sigaction", (uintptr_t)stub_int_neg1 },
    { "sin", (uintptr_t)sfp_sin },
    { "sinf", (uintptr_t)sfp_sinf },
    { "sinh", (uintptr_t)sfp_sinh },
    { "slCreateEngine", (uintptr_t)slCreateEngine },
    { "sleep", (uintptr_t)sleep },
    { "snprintf", (uintptr_t)snprintf },
    { "socket", (uintptr_t)stub_int_neg1 },
    { "sprintf", (uintptr_t)sprintf },
    { "srand48", (uintptr_t)srand48 },
    { "sscanf", (uintptr_t)sscanf },
    { "stat", (uintptr_t)vita_stat },
    { "strcasecmp", (uintptr_t)strcasecmp },
    { "strcat", (uintptr_t)strcat },
    { "strchr", (uintptr_t)strchr },
    { "strcmp", (uintptr_t)strcmp },
    { "strcoll", (uintptr_t)my_strcoll },
    { "strcpy", (uintptr_t)strcpy },
    { "strcspn", (uintptr_t)strcspn },
    { "strdup", (uintptr_t)strdup },
    { "strerror", (uintptr_t)strerror },
    { "strerror_r", (uintptr_t)strerror_r },
    { "strftime", (uintptr_t)strftime },
    { "strlcat", (uintptr_t)strlcat },
    { "strlen", (uintptr_t)strlen },
    { "strncasecmp", (uintptr_t)strncasecmp },
    { "strncat", (uintptr_t)strncat },
    { "strncmp", (uintptr_t)strncmp },
    { "strncpy", (uintptr_t)strncpy },
    { "strpbrk", (uintptr_t)strpbrk },
    { "strptime", (uintptr_t)strptime },
    { "strrchr", (uintptr_t)strrchr },
    { "strstr", (uintptr_t)vita_strstr },
    { "strtod", (uintptr_t)sfp_strtod },
    { "strtok", (uintptr_t)strtok },
    { "strtok_r", (uintptr_t)strtok_r },
    { "strtol", (uintptr_t)strtol },
    { "strtoll", (uintptr_t)strtoll },
    { "strtoul", (uintptr_t)strtoul },
    { "strxfrm", (uintptr_t)my_strxfrm },
    { "swprintf", (uintptr_t)my_swprintf },
    { "syscall", (uintptr_t)stub_int_neg1 },  /* libgenerator-only; no Linux syscalls on Vita */
    { "sysconf", (uintptr_t)stub_int_zero },
    { "system", (uintptr_t)stub_int_neg1 },
    { "tan", (uintptr_t)sfp_tan },
    { "tanf", (uintptr_t)sfp_tanf },
    { "tanh", (uintptr_t)sfp_tanh },
    { "time", (uintptr_t)time },
    { "tmpfile", (uintptr_t)stub_ptr_null },
    { "tmpnam", (uintptr_t)stub_ptr_null },
    { "tolower", (uintptr_t)tolower },
    { "toupper", (uintptr_t)toupper },
    { "towlower", (uintptr_t)towlower },
    { "towupper", (uintptr_t)towupper },
    { "ungetc", (uintptr_t)vita_ungetc },
    { "ungetwc", (uintptr_t)vita_ungetwc },
    { "unlink", (uintptr_t)vita_unlink },
    { "usleep", (uintptr_t)usleep },
    { "vfprintf", (uintptr_t)vita_vfprintf },
    { "vsnprintf", (uintptr_t)vsnprintf },
    { "vsprintf", (uintptr_t)vsprintf },
    { "wcrtomb", (uintptr_t)wcrtomb },
    { "wcscmp", (uintptr_t)wcscmp },
    { "wcscoll", (uintptr_t)wcscoll },
    { "wcscpy", (uintptr_t)wcscpy },
    { "wcsftime", (uintptr_t)wcsftime },
    { "wcslen", (uintptr_t)wcslen },
    { "wcsxfrm", (uintptr_t)wcsxfrm },
    { "wctob", (uintptr_t)wctob },
    { "wctype", (uintptr_t)wctype },
    { "wmemchr", (uintptr_t)wmemchr },
    { "wmemcmp", (uintptr_t)wmemcmp },
    { "wmemcpy", (uintptr_t)wmemcpy },
    { "wmemmove", (uintptr_t)wmemmove },
    { "wmemset", (uintptr_t)wmemset },
    { "write", (uintptr_t)write },
    { "writev", (uintptr_t)my_writev },
};
/* so_resolve treats size_default_dynlib as BYTES (it divides by
   sizeof(so_default_dynlib) internally), so export the byte size,
   not the entry count. Matching the type/name main.c expects. */
const int g_default_dynlib_count = sizeof(g_default_dynlib);
