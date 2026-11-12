/* zw_gxmstat.c - measurement only (cmake -DZW_GXMSTAT=1). Never ship a build with it on.
 * Times the SceGxm / SceClib calls that the prebuilt libvitaGL makes while a draw call is
 * running (the linker redirects them here with --wrap). Shows where glDrawElements time goes. */
#if ZW_GXMSTAT
#include <psp2/gxm.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void zw_log_raw(const char *s);
volatile int g_zw_in_draw;
enum { S_DRAW, S_UNIF, S_RESV, S_RESF, S_STREAM, S_FTEX, S_VPROG, S_FPROG, S_TEXVAL, S_MEMCPY, S_PATCHV, S_PATCHF, S_N };
static const char *const g_nm[S_N] = { "Draw", "SetUniformDataF", "ReserveVertUB", "ReserveFragUB", "SetVertexStream", "SetFragTexture",
    "SetVertexProgram", "SetFragProgram", "TextureValidate", "ClibMemcpy", "PatchVertex", "PatchFragment" };
static uint64_t g_us[S_N]; static unsigned g_n[S_N]; static uint64_t g_bytes;
#define T0 uint64_t _t = g_zw_in_draw ? sceKernelGetProcessTimeWide() : 0
#define T1(s) do { if (_t) { g_us[s] += sceKernelGetProcessTimeWide() - _t; g_n[s]++; } } while (0)

int __real_sceGxmDraw(SceGxmContext *, SceGxmPrimitiveType, SceGxmIndexFormat, const void *, unsigned int);
int __wrap_sceGxmDraw(SceGxmContext *c, SceGxmPrimitiveType p, SceGxmIndexFormat f, const void *i, unsigned int n) { T0; int r = __real_sceGxmDraw(c, p, f, i, n); T1(S_DRAW); return r; }
int __real_sceGxmSetUniformDataF(void *, const SceGxmProgramParameter *, unsigned int, unsigned int, const float *);
int __wrap_sceGxmSetUniformDataF(void *b, const SceGxmProgramParameter *p, unsigned int o, unsigned int n, const float *d) { T0; int r = __real_sceGxmSetUniformDataF(b, p, o, n, d); T1(S_UNIF); return r; }
int __real_sceGxmReserveVertexDefaultUniformBuffer(SceGxmContext *, void **);
int __wrap_sceGxmReserveVertexDefaultUniformBuffer(SceGxmContext *c, void **b) { T0; int r = __real_sceGxmReserveVertexDefaultUniformBuffer(c, b); T1(S_RESV); return r; }
int __real_sceGxmReserveFragmentDefaultUniformBuffer(SceGxmContext *, void **);
int __wrap_sceGxmReserveFragmentDefaultUniformBuffer(SceGxmContext *c, void **b) { T0; int r = __real_sceGxmReserveFragmentDefaultUniformBuffer(c, b); T1(S_RESF); return r; }
int __real_sceGxmSetVertexStream(SceGxmContext *, unsigned int, const void *);
int __wrap_sceGxmSetVertexStream(SceGxmContext *c, unsigned int i, const void *d) { T0; int r = __real_sceGxmSetVertexStream(c, i, d); T1(S_STREAM); return r; }
int __real_sceGxmSetFragmentTexture(SceGxmContext *, unsigned int, const SceGxmTexture *);
int __wrap_sceGxmSetFragmentTexture(SceGxmContext *c, unsigned int i, const SceGxmTexture *t) { T0; int r = __real_sceGxmSetFragmentTexture(c, i, t); T1(S_FTEX); return r; }
void __real_sceGxmSetVertexProgram(SceGxmContext *, const SceGxmVertexProgram *);
void __wrap_sceGxmSetVertexProgram(SceGxmContext *c, const SceGxmVertexProgram *p) { T0; __real_sceGxmSetVertexProgram(c, p); T1(S_VPROG); }
void __real_sceGxmSetFragmentProgram(SceGxmContext *, const SceGxmFragmentProgram *);
void __wrap_sceGxmSetFragmentProgram(SceGxmContext *c, const SceGxmFragmentProgram *p) { T0; __real_sceGxmSetFragmentProgram(c, p); T1(S_FPROG); }
int __real_sceGxmTextureValidate(const SceGxmTexture *);
int __wrap_sceGxmTextureValidate(const SceGxmTexture *t) { T0; int r = __real_sceGxmTextureValidate(t); T1(S_TEXVAL); return r; }
void *__real_sceClibMemcpy(void *, const void *, SceSize);
void *__wrap_sceClibMemcpy(void *d, const void *s, SceSize n) { T0; void *r = __real_sceClibMemcpy(d, s, n); if (_t) g_bytes += n; T1(S_MEMCPY); return r; }
int __real_sceGxmShaderPatcherCreateVertexProgram(SceGxmShaderPatcher *, SceGxmShaderPatcherId, const SceGxmVertexAttribute *, unsigned int, const SceGxmVertexStream *, unsigned int, SceGxmVertexProgram **);
int __wrap_sceGxmShaderPatcherCreateVertexProgram(SceGxmShaderPatcher *a, SceGxmShaderPatcherId b, const SceGxmVertexAttribute *c, unsigned int d, const SceGxmVertexStream *e, unsigned int f, SceGxmVertexProgram **g) { T0; int r = __real_sceGxmShaderPatcherCreateVertexProgram(a, b, c, d, e, f, g); T1(S_PATCHV); return r; }
int __real_sceGxmShaderPatcherCreateFragmentProgram(SceGxmShaderPatcher *, SceGxmShaderPatcherId, SceGxmOutputRegisterFormat, SceGxmMultisampleMode, const SceGxmBlendInfo *, const SceGxmProgram *, SceGxmFragmentProgram **);
int __wrap_sceGxmShaderPatcherCreateFragmentProgram(SceGxmShaderPatcher *a, SceGxmShaderPatcherId b, SceGxmOutputRegisterFormat c, SceGxmMultisampleMode d, const SceGxmBlendInfo *e, const SceGxmProgram *f, SceGxmFragmentProgram **g) { T0; int r = __real_sceGxmShaderPatcherCreateFragmentProgram(a, b, c, d, e, f, g); T1(S_PATCHF); return r; }

void zw_gxmstat_log(int frame) {
    char buf[700]; int n = 0;
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "  stats gxm f%d per frame (ms/calls):", frame);
    for (int i = 0; i < S_N && n < (int)sizeof(buf) - 80; i++) {
        if (g_n[i]) n += snprintf(buf + n, sizeof(buf) - (size_t)n, " %s %.2f/%.0f", g_nm[i], g_us[i] / 60000.0, g_n[i] / 60.0);
        g_us[i] = 0; g_n[i] = 0;
    }
    snprintf(buf + n, sizeof(buf) - (size_t)n, " | memcpy %.1f KB\n", g_bytes / 60.0 / 1024.0);
    g_bytes = 0;
    zw_log_raw(buf);
}
#endif
