/*
 * Zombiewood PS Vita Loader
 * Gameloft  |  armeabi-v7a ARM  |  OpenGL ES 2.0  |  2014
 *
 * Loads the Android ARMv7 .so into Vita memory, patches all imports,
 * and redirects Android API calls to Vita equivalents.
 *
 * Build requires: VitaSDK, vitaGL, vitashark, SceShaccCgExt, mathneon
 * References so_util from TheOfficialFloW/gtasa_vita
 */

#include <vitasdk.h>
#include <vitaGL.h>
#ifndef VITA3K_BUILD
#include <kubridge.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <setjmp.h>
#include <png.h>
#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>
#include <ctype.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <wchar.h>
#include <time.h>
#include <jni.h>

#include "so_util.h"

#ifndef JNI_OK
#define JNI_OK 0
#endif

#ifndef JNI_FALSE
#define JNI_FALSE 0
#endif

#ifndef JNI_TRUE
#define JNI_TRUE 1
#endif

/* Fixed load addresses for the two .so files.
   so_util.c sets opt.attr=0x1 and opt.field_C=load_addr, which means
   "map at this fixed virtual address." load_addr=0 means VA 0 → invalid →
   kuKernelAllocMemBlock returns 0x80020001. Match TheOfficialFloW's
   convention (gtasa_vita uses 0x98000000) and stack the two libs. */
#define LOAD_ADDR_GENERATOR  0x97000000  /* ~16 MB region — generator is 448 KB */
#define LOAD_ADDR_ZOMBIEWOOD 0x98000000  /* ~16 MB region — main is 13.5 MB */

/* OpenSL ES stub types (we don't link the real lib) */
typedef void *SLObjectItf;
typedef const void *SLInterfaceID;
typedef unsigned int SLuint32;
typedef unsigned char SLboolean;
typedef struct SLEngineOption_ { SLuint32 feature; SLuint32 data; } SLEngineOption;
typedef SLuint32 SLresult;

/* ─────────────────────────────────────────────
   Logging helpers - Direct kernel file I/O (no FILE*, no vfprintf)
   ───────────────────────────────────────────── */
#define LOG_PATH "ux0:/data/zombiewood/debug_v2.log"
static SceUID g_log_fd = -1;
static void zw_file_locks_init(void);

static void log_init(void) {
    zw_file_locks_init();
    sceIoMkdir("ux0:/data/zombiewood", 0777);
    sceIoMkdir("ux0:/data/zombiewood/writecache", 0777);
    sceIoRemove(LOG_PATH);
    g_log_fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (g_log_fd >= 0) {
    SceDateTime now;
    char stamp[96];
    int rtc_res = sceRtcGetCurrentClockLocalTime(&now);
    if (rtc_res >= 0) {
        snprintf(stamp, sizeof(stamp),
                 "[ZW] Log generated: %04u-%02u-%02u %02u:%02u:%02u local Vita time\n",
                 now.year, now.month, now.day,
                 now.hour, now.minute, now.second);
    } else {
        snprintf(stamp, sizeof(stamp),
                 "[ZW] Log generated: Vita RTC unavailable (0x%08X)\n",
                 rtc_res);
    }
    sceIoWrite(g_log_fd, stamp, strlen(stamp));
#ifndef VITA3K_BUILD
    sceClibPrintf("%s", stamp);
#endif
    }
}

static volatile uint32_t g_log_bytes = 0; /* v4.43: heartbeat for stall watchdog */

/* v5.04: the log is buffered in RAM. Every log call used to be three unbuffered sceIoWrite
 * calls (~15 ms per line on the memory card), and boot emits ~2200 lines. The buffer is
 * written out when it fills, every 5 s from the power-tick thread, and on exit paths.
 * Build with -DZW_LOG_SYNC=1 to write every line at once when chasing a crash. */
#ifndef ZW_LOG_SYNC
#define ZW_LOG_SYNC 0
#endif
#ifndef ZW_BOOT_TIMING
#define ZW_BOOT_TIMING 0
#endif
#ifndef ZW_CALLPROF
#define ZW_CALLPROF 0      /* 1 = call profiler (zw_prof.c), measurement only; never ship 1 */
#endif
#ifndef ZW_DEBUG_BUILD
#define ZW_DEBUG_BUILD 0   /* v5.10: 1 = SELECT debug overlay + its keys, autofire/autotouch test aids */
#endif
#define ZW_LOG_BUF_SZ (32 * 1024)
static char g_log_buf[ZW_LOG_BUF_SZ];
static unsigned g_log_buf_n;
static SceKernelLwMutexWork g_log_mtx;
static int g_log_mtx_ok;

static void log_flush_locked(void) {
    if (g_log_fd >= 0 && g_log_buf_n) sceIoWrite(g_log_fd, g_log_buf, g_log_buf_n);
    g_log_buf_n = 0;
}

void zw_log_flush(void) {
    if (g_log_mtx_ok) sceKernelLockLwMutex(&g_log_mtx, 1, NULL);
    log_flush_locked();
    if (g_log_mtx_ok) sceKernelUnlockLwMutex(&g_log_mtx, 1);
}

static void log_write(const char *str) {
    if (!str) return;
    size_t n = strlen(str);
    g_log_bytes += (uint32_t)n;
    if (g_log_mtx_ok) sceKernelLockLwMutex(&g_log_mtx, 1, NULL);
#if ZW_BOOT_TIMING
    {   /* v5.06: mark every gap of 250 ms or more between two log lines with the process time */
        static SceUInt64 last_t; static int mid_line;
        SceUInt64 now_t = sceKernelGetProcessTimeWide();
        if (!mid_line && last_t && now_t - last_t >= 250000 && g_log_buf_n + 80 < ZW_LOG_BUF_SZ)
            g_log_buf_n += snprintf(g_log_buf + g_log_buf_n, 80, "[ZW] v5.06 gap %u ms, now T+%u ms\n",
                                    (unsigned)((now_t - last_t) / 1000), (unsigned)(now_t / 1000));
        last_t = now_t;
        if (n) mid_line = str[n - 1] != '\n';
    }
#endif
    if (n >= ZW_LOG_BUF_SZ) {
        log_flush_locked();
        if (g_log_fd >= 0) sceIoWrite(g_log_fd, (void *)str, n);
    } else {
        if (g_log_buf_n + n > ZW_LOG_BUF_SZ) log_flush_locked();
        memcpy(g_log_buf + g_log_buf_n, str, n);
        g_log_buf_n += n;
        if (ZW_LOG_SYNC) log_flush_locked();
    }
    if (g_log_mtx_ok) sceKernelUnlockLwMutex(&g_log_mtx, 1);
    sceClibPrintf("%s", str);
}

static void log_print(const char *msg) {
    log_write("[ZW] ");
    log_write(msg);
    log_write("\n");
}

/* Safe formatted logging with buffer limits */
static void log_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_write("[ZW] ");
    log_write(buf);
    log_write("\n");
}

#define LOG(msg) log_print(msg)
#define LOGF(fmt, ...) log_printf(fmt, __VA_ARGS__)

/* v4.99: logging entry point for other files (audio_sl.c) */
void zw_log(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_write("[ZW] ");
    log_write(buf);
}

#ifdef VITA3K_BUILD
#define JNI_TRACEF(...) do { } while (0)
#else
#define JNI_TRACEF(...) LOGF(__VA_ARGS__)
#endif

extern jmp_buf g_android_exit_jmp;
extern int g_android_exit_active;
extern int g_android_exit_status;
extern unsigned int g_gl_draw_arrays_count;
extern unsigned int g_gl_draw_elements_count;

/* Fatal error handler (used by so_util.c) */
void fatal_error(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_write("[FATAL] ");
    log_write(buf);
    log_write("\n");
    zw_log_flush();
    sceKernelExitProcess(1);
}

/* Stubs for vitashark shader extensions (optional for basic gameplay) */
void sceShaccCgExtEnableExtensions(void) {
    LOG("sceShaccCgExtEnableExtensions() called (stub)\n");
}

void sceShaccCgExtDisableExtensions(void) {
    LOG("sceShaccCgExtDisableExtensions() called (stub)\n");
}

/* Stubs for SceShaccCg shader compiler - vitashark calls these */
typedef void *SceShaccCgProgram;

SceShaccCgParameter sceShaccCgGetFirstParameter(SceShaccCgProgram program) {
    return NULL;
}

SceShaccCgParameter sceShaccCgGetNextParameter(SceShaccCgParameter parameter) {
    return NULL;
}

const char *sceShaccCgGetParameterName(SceShaccCgParameter parameter) {
    return "param";
}

int sceShaccCgGetParameterClass(SceShaccCgParameter parameter) {
    return 0;
}

int sceShaccCgGetParameterBufferIndex(SceShaccCgParameter parameter) {
    return 0;
}





/* ─────────────────────────────────────────────
   Filesystem path redirect
   Android OBB paths  →  ux0:/data/zombiewood/
   ───────────────────────────────────────────── */
/* v5.04: no shared path buffers. Vox opens files on the audio thread while the game
 * thread does too; redirect_path() now writes into a buffer owned by each call site. */
#define ZW_PATH_MAX 512

typedef struct TrackedFile_ {
    FILE *fp;
    char path[160];
    char mode[16];
} TrackedFile;

static TrackedFile g_tracked_files[64];

#define VIRTUAL_ASSET_CACHE_SLOTS 16
#define VIRTUAL_FILE_SLOTS 128   /* v5.02: OBB streams are virtual now (~30 open at once) */
#define VIRTUAL_FILE_MAGIC 0x5646494Cu

typedef struct VirtualAssetCache_ {
    char path[384];
    unsigned char *data;
    long len;
} VirtualAssetCache;

typedef struct VirtualFile_ {
    uint32_t magic;
    int in_use;
    int sink;
    VirtualAssetCache *asset;
    long pos;
    int ungot;
    unsigned trace_ops;
    int obb;          /* v5.02: 1 = backed by the shared OBB block cache, not asset->data */
    int obb_idx;
} VirtualFile;

static VirtualAssetCache g_virtual_asset_cache[VIRTUAL_ASSET_CACHE_SLOTS];
static unsigned g_virtual_asset_cache_next = 0;
static VirtualFile g_virtual_files[VIRTUAL_FILE_SLOTS];

static int ptr_in_so_module(const so_module *mod, const void *ptr, size_t size) {
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t end = p + size;
    if (!ptr || end < p) {
        return 0;
    }
    if (mod->text_base && p >= mod->text_base && end <= mod->text_base + mod->text_size) {
        return 1;
    }
    if (mod->data_base && p >= mod->data_base && end <= mod->data_base + mod->data_size) {
        return 1;
    }
    return 0;
}

static const char *analytics_suffix_from_path(const char *path) {
    static const char *suffixes[] = {
        "w_ev.dat.new",
        "r_ev.dat.new",
        "error_events_full.dat.new",
        "error_events.dat.new",
        "ses_info.dat.new",
        "aux_file_data.dat.new",
        "tracking_markers.dat.new",
        "tracking_batchable_events.dat.new",
        "user_data.dat.new",
        "w_ev.dat",
        "r_ev.dat",
        "error_events_full.dat",
        "error_events.dat",
        "ses_info.dat",
        "aux_file_data.dat",
        "tracking_markers.dat",
        "tracking_batchable_events.dat",
        "tracking_batchable_events",
        "user_data.dat",
        "QADEBUG.txt",
        NULL
    };

    if (!path) return NULL;
    for (int i = 0; suffixes[i]; i++) {
        const char *hit = strstr(path, suffixes[i]);
        if (hit) return suffixes[i];
    }
    return NULL;
}

static const char *fuzzy_analytics_name(const char *path) {
    if (!path) return NULL;

    int wants_new = strstr(path, ".new") != NULL;

    if (strstr(path, "error_events_full"))
        return wants_new ? "error_events_full.dat.new" : "error_events_full.dat";
    if (strstr(path, "error_events"))
        return wants_new ? "error_events.dat.new" : "error_events.dat";
    if (strstr(path, "aux_file_data"))
        return wants_new ? "aux_file_data.dat.new" : "aux_file_data.dat";
    if (strstr(path, "ses_info"))
        return wants_new ? "ses_info.dat.new" : "ses_info.dat";
    if (strstr(path, "tracking_markers"))
        return wants_new ? "tracking_markers.dat.new" : "tracking_markers.dat";
    if (strstr(path, "tracking_batchable_events"))
        return wants_new ? "tracking_batchable_events.dat.new" : "tracking_batchable_events.dat";
    if (strstr(path, "user_data"))
        return wants_new ? "user_data.dat.new" : "user_data.dat";
    if (strstr(path, "QADEBUG"))
        return "QADEBUG.txt";
    if (strstr(path, "w_ev"))
        return wants_new ? "w_ev.dat.new" : "w_ev.dat";
    if (strstr(path, "r_ev"))
        return wants_new ? "r_ev.dat.new" : "r_ev.dat";

    return NULL;
}

static int is_save_state_name(const char *fname);

static const char *dollar_tail_filename(const char *path) {
    if (!path) return NULL;

    const char *dollar = strrchr(path, '$');
    if (!dollar || !dollar[1]) return NULL;

    const char *tail = dollar + 1;
    while (*tail && !isalnum((unsigned char)*tail) && *tail != '_' && *tail != '.') {
        tail++;
    }
    if (!*tail) return NULL;

    if (strstr(tail, ".dat") ||
        strstr(tail, ".txt") ||
        strstr(tail, ".xml") ||
        strstr(tail, ".json")) {
        return tail;
    }
    return NULL;
}

static volatile int g_main_loop_frame;
#ifndef ZW_PERIODIC_LOG
#define ZW_PERIODIC_LOG 0
#endif
/* v5.01: per-call file logging only during boot; in play each line is a ~15 ms stall. */
#define ZW_RUNTIME_LOG_OK() (ZW_PERIODIC_LOG || g_main_loop_frame < 60)
static int should_trace_file_path(const char *path) {
    if (!path || !ZW_RUNTIME_LOG_OK()) return 0;
#ifdef VITA3K_BUILD
    return 0;
#endif
    const char *slash = strrchr(path, '/');
    const char *fname = slash ? slash + 1 : path;
    if (is_save_state_name(fname)) return 1;
    if (strstr(path, ".obb")) return 0;
    if (strstr(path, ".bar")) return 0;
    if (strstr(path, ".bdae")) return 0;
    if (strstr(path, ".bin")) return 0;
    if (strstr(path, ".mpc")) return 0;
    if (strstr(path, ".swf")) return 0;
    if (strstr(path, ".ttf")) return 0;
    if (strstr(path, ".wav")) return 0;
    return strstr(path, "oconf.bar") ||
           strstr(path, "tracker") ||
           strstr(path, "events") ||
           strstr(path, "ses_info") ||
           fuzzy_analytics_name(path) ||
           analytics_suffix_from_path(path);
}

/* v4.84: which real files are open right now (leak hunt: fopen returned NULL for
 * existing files after ~70 shots = file handles exhausted). */
static volatile int g_main_loop_frame;
static unsigned g_zw_fopen_n, g_zw_fclose_n;
static struct { FILE *f; char name[40]; } g_zw_open[128];
static void zw_open_note(FILE *f, const char *path) {
    const char *b = path ? strrchr(path, '/') : NULL; b = b ? b + 1 : (path ? path : "?");
    for (int i = 0; i < 128; i++) if (!g_zw_open[i].f) { g_zw_open[i].f = f; snprintf(g_zw_open[i].name, sizeof(g_zw_open[i].name), "%s", b); return; }
}
static void zw_open_forget(FILE *f) {
    for (int i = 0; i < 128; i++) if (g_zw_open[i].f == f) { g_zw_open[i].f = NULL; return; }
}
static void zw_mem_report(const char *why, int err, const char *path);
static uintptr_t g_zw_so_base;
static uintptr_t g_zw_so_base_fwd(void) { return g_zw_so_base; }
static void zw_open_report(int frame) {
    int n = 0; char line[200]; int o = 0; line[0] = 0;
    for (int i = 0; i < 128; i++) if (g_zw_open[i].f) { n++; if (o < 150) o += snprintf(line + o, sizeof(line) - o, " %s", g_zw_open[i].name); }
    log_printf("  v4.84 files f%d: fopen=%u fclose=%u open_now=%d:%s\n", frame, g_zw_fopen_n, g_zw_fclose_n, n, line);
    zw_mem_report("periodic", 0, NULL);
}
/* v4.86: memory / descriptor state, for the fopen-NULL-during-play crash. */
static unsigned g_zw_open_n, g_zw_close_n;
static int g_zw_fd_now, g_zw_fd_max;
static void zw_mem_report(const char *why, int err, const char *path) {
    struct mallinfo mi = mallinfo();
    SceKernelFreeMemorySizeInfo fm; memset(&fm, 0, sizeof(fm)); fm.size = sizeof(fm);
    sceKernelGetFreeMemorySize(&fm);
    int n = 0; for (int i = 0; i < 128; i++) if (g_zw_open[i].f) n++;
    log_printf("  v4.86 %s f%d errno=%d '%s' heap_arena=%u used=%u free=%u | kern user=%uK cdram=%uK phycont=%uK | stdio_open=%d fd_open=%d fd_max=%d open()=%u close()=%u\n",
               why, g_main_loop_frame, err, path ? path : "-",
               (unsigned)mi.arena, (unsigned)mi.uordblks, (unsigned)mi.fordblks,
               (unsigned)(fm.size_user >> 10), (unsigned)(fm.size_cdram >> 10), (unsigned)(fm.size_phycont >> 10),
               n, g_zw_fd_now, g_zw_fd_max, g_zw_open_n, g_zw_close_n);
    /* v5.06: VitaGL's own pools (textures live here; the kernel cdram figure above is 0
     * because VitaGL takes all of it at start) */
    if (g_main_loop_frame > 0)   /* not before vglInit */
    log_printf("  v5.06 vgl free f%d: vram=%uK ram=%uK phycont=%uK newlib=%uK\n", g_main_loop_frame,
               (unsigned)(vglMemFree(VGL_MEM_VRAM) >> 10), (unsigned)(vglMemFree(VGL_MEM_RAM) >> 10),
               (unsigned)(vglMemFree(VGL_MEM_SLOW) >> 10), (unsigned)(vglMemFree(VGL_MEM_EXTERNAL) >> 10));
}
static int should_log_fopen_result(const char *path, const char *mode, FILE *fp) {
    if (!path) return 0;
    if (!fp) return 1;
#ifdef VITA3K_BUILD
    return 0;
#endif
    if (should_trace_file_path(path)) return 1;
    if (strstr(path, "/proc/meminfo")) return 0;
    if (strstr(path, ".obb")) return 0;
    if (strstr(path, ".bar")) return 0;
    if (strstr(path, ".bdae")) return 0;
    if (strstr(path, ".bin")) return 0;
    if (strstr(path, ".mpc")) return 0;
    if (strstr(path, ".swf")) return 0;
    if (strstr(path, ".ttf")) return 0;
    if (strstr(path, ".wav")) return 0;
    return mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'));
}

static void track_file(FILE *fp, const char *path, const char *mode) {
    if (!fp || !should_trace_file_path(path)) return;

    for (int i = 0; i < 64; i++) {
        if (g_tracked_files[i].fp == fp || !g_tracked_files[i].fp) {
            g_tracked_files[i].fp = fp;
            snprintf(g_tracked_files[i].path, sizeof(g_tracked_files[i].path),
                     "%s", path ? path : "(null)");
            snprintf(g_tracked_files[i].mode, sizeof(g_tracked_files[i].mode),
                     "%s", mode ? mode : "");
            return;
        }
    }
}

static const char *tracked_file_path(FILE *fp) {
    if (!fp) return NULL;
    for (int i = 0; i < 64; i++) {
        if (g_tracked_files[i].fp == fp) return g_tracked_files[i].path;
    }
    return NULL;
}

static void untrack_file(FILE *fp) {
    if (!fp) return;
    for (int i = 0; i < 64; i++) {
        if (g_tracked_files[i].fp == fp) {
            g_tracked_files[i].fp = NULL;
            g_tracked_files[i].path[0] = '\0';
            g_tracked_files[i].mode[0] = '\0';
            return;
        }
    }
}

static int is_zombiewood_asset_path(const char *path) {
    if (!path || !path[0]) return 0;

    const char *slash = strrchr(path, '/');
    const char *fname = slash ? slash + 1 : path;
    if (fname[0] == '\0') return 0;

    size_t len = strlen(fname);
    if (len > 4 && strcmp(fname + len - 4, ".bar") == 0) return 1;
    if (len > 4 && strcmp(fname + len - 4, ".bin") == 0) return 1;
    if (len > 4 && strcmp(fname + len - 4, ".xml") == 0) return 1;
    if (len > 4 && strcmp(fname + len - 4, ".dat") == 0) return 1;
    if (len > 5 && strcmp(fname + len - 5, ".bdae") == 0) return 1;
    if (len > 4 && strcmp(fname + len - 4, ".mpc") == 0) return 1;
    if (len > 4 && strcmp(fname + len - 4, ".vxn") == 0) return 1;
    if (len > 4 && strcmp(fname + len - 4, ".swf") == 0) return 1;
    if (len > 4 && strcmp(fname + len - 4, ".wav") == 0) return 1;
    if (len > 4 && strcmp(fname + len - 4, ".ttf") == 0) return 1;
    if (len > 5 && strcmp(fname + len - 5, ".conf") == 0) return 1;
    if (len > 5 && strcmp(fname + len - 5, ".json") == 0) return 1;
    if (len > 5 && strcmp(fname + len - 5, ".list") == 0) return 1;
    if (len > 4 && strcmp(fname + len - 4, ".txt") == 0) return 1;

    if (strcmp(fname, "ZOMBIESDLCTOC") == 0) return 1;
    if (strcmp(fname, "svn.revision") == 0) return 1;
    if (strcmp(fname, ".svn.revision") == 0) return 1;

    return 0;
}

static int has_ext(const char *fname, const char *ext) {
    if (!fname || !ext) return 0;
    size_t fname_len = strlen(fname);
    size_t ext_len = strlen(ext);
    return fname_len > ext_len && strcmp(fname + fname_len - ext_len, ext) == 0;
}

static int is_packaged_asset_name(const char *fname) {
    if (!fname) return 0;
    return has_ext(fname, ".bar")  ||
           has_ext(fname, ".bdae") ||
           has_ext(fname, ".bin")  ||
           has_ext(fname, ".conf") ||
           has_ext(fname, ".json") ||
           has_ext(fname, ".list") ||
           has_ext(fname, ".mpc")  ||
           has_ext(fname, ".swf")  ||
           has_ext(fname, ".txt")  ||
           has_ext(fname, ".vxn")  ||
           has_ext(fname, ".wav")  ||
           has_ext(fname, ".xml")  ||
           strcmp(fname, "ZOMBIESDLCTOC") == 0 ||
           strcmp(fname, "svn.revision") == 0 ||
           strcmp(fname, ".svn.revision") == 0;
}

static int is_save_state_name(const char *fname) {
    if (!fname) return 0;
    return strcmp(fname, "player.dat") == 0 ||
           strcmp(fname, "playerbckp.dat") == 0 ||
           strcmp(fname, "level.dat") == 0 ||
           strcmp(fname, "zomb.dat") == 0;
}

static int is_asset_write_mode(const char *mode) {
    return mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'));
}

static int is_asset_read_mode(const char *mode) {
    return mode && strchr(mode, 'r') &&
           !strchr(mode, 'w') && !strchr(mode, 'a') && !strchr(mode, '+');
}

static int is_service_scratch_name(const char *fname) {
    if (!fname) return 0;
    return strcmp(fname, "tracker.xml") == 0 ||
           strcmp(fname, "snsconfig.json") == 0 ||
           strstr(fname, "tracking") ||
           strstr(fname, "tracker");
}

static int file_is_empty(FILE *f) {
    if (!f) return 0;
    long pos = ftell(f);
    if (pos < 0) pos = 0;
    if (fseek(f, 0, SEEK_END) != 0) return 0;
    long end = ftell(f);
    fseek(f, pos, SEEK_SET);
    return end == 0;
}

static int save_file_looks_blank(FILE *f) {
    if (!f) return 0;
    long pos = ftell(f);
    if (pos < 0) pos = 0;

    if (fseek(f, 0, SEEK_END) != 0) return 0;
    long end = ftell(f);
    if (end <= 0) {
        fseek(f, pos, SEEK_SET);
        return 1;
    }

    if (fseek(f, 0, SEEK_SET) != 0) return 0;
    unsigned char buf[32];
    size_t got = fread(buf, 1, sizeof(buf), f);
    fseek(f, pos, SEEK_SET);

    if (got == 0) return 1;
    for (size_t i = 0; i < got; i++) {
        if (buf[i] != 0) return 0;
    }
    return 1;
}

/* v5.04: one lock for the VirtualFile slot table. Slots used to be claimed under the OBB
 * mutex, the wav mutex or no lock at all, and released with no lock, so the game thread and
 * the audio thread could claim the same slot. */
static SceKernelLwMutexWork g_vf_mtx;
static int g_vf_mtx_ok;
static void zw_file_locks_init(void) {
    if (!g_log_mtx_ok && sceKernelCreateLwMutex(&g_log_mtx, "zw_log", 0, 0, NULL) >= 0) g_log_mtx_ok = 1;
    if (!g_vf_mtx_ok && sceKernelCreateLwMutex(&g_vf_mtx, "zw_vf", 0, 0, NULL) >= 0) g_vf_mtx_ok = 1;
}
static VirtualFile *vf_alloc(void) {
    VirtualFile *vf = NULL;
    if (g_vf_mtx_ok) sceKernelLockLwMutex(&g_vf_mtx, 1, NULL);
    for (int i = 0; i < VIRTUAL_FILE_SLOTS; i++) {
        if (g_virtual_files[i].in_use) continue;
        vf = &g_virtual_files[i];
        memset(vf, 0, sizeof(*vf));
        vf->magic = VIRTUAL_FILE_MAGIC;
        vf->ungot = -1;
        vf->in_use = 1;
        break;
    }
    if (g_vf_mtx_ok) sceKernelUnlockLwMutex(&g_vf_mtx, 1);
    return vf;
}
static void vf_free(VirtualFile *vf) {
    if (g_vf_mtx_ok) sceKernelLockLwMutex(&g_vf_mtx, 1, NULL);
    memset(vf, 0, sizeof(*vf));
    vf->ungot = -1;
    if (g_vf_mtx_ok) sceKernelUnlockLwMutex(&g_vf_mtx, 1);
}

static VirtualFile *virtual_file_from_FILE(FILE *f) {
    if (!f) return NULL;
    for (int i = 0; i < VIRTUAL_FILE_SLOTS; i++) {
        if ((FILE *)&g_virtual_files[i] == f &&
            g_virtual_files[i].magic == VIRTUAL_FILE_MAGIC &&
            g_virtual_files[i].in_use) {
            return &g_virtual_files[i];
        }
    }
    return NULL;
}

/* ── v5.02: OBB streams served from a shared RAM block cache ──
 * Vox reopens main.obb for every sound it plays and does ~13 fseek + ~20 small fread on
 * it; through newlib stdio each seek discarded the buffer and cost ~4 ms on the memory
 * card (55-250 ms hitches when shooting/walking). OBB opens now return a VirtualFile that
 * reads with sceIoPread through a 4 MB LRU cache of 32 KB blocks, shared by all streams
 * and both threads (Vox decodes on the audio thread). Reads >= 64 KB bypass the cache. */
#define ZW_OBB_MAX       4
#define ZW_OBB_BLK       (32 * 1024)
#define ZW_OBB_NBLK      48    /* 1.5 MB; v5.06: the other 512 KB of the 2 MB block holds the small-read index */
#define ZW_OBB_HDR_BYTES (512 * 1024)
static struct { char path[128]; SceUID fd; long len; } g_obb[ZW_OBB_MAX];
static struct { int obb; long blk; unsigned stamp; } g_obb_tag[ZW_OBB_NBLK];
static uint8_t *g_obb_mem;
static unsigned g_obb_clock, g_obb_hits, g_obb_miss;
static SceKernelLwMutexWork g_obb_mtx;
static int g_obb_ready = -1;

static void hdr_init(uint8_t *mem);
static int obb_cache_init(void) {
    if (g_obb_ready >= 0) return g_obb_ready;
    g_obb_ready = 0;
    SceUID blk = sceKernelAllocMemBlock("zw_obbcache", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                        ZW_OBB_BLK * ZW_OBB_NBLK + ZW_OBB_HDR_BYTES, NULL);
    if (blk < 0 || sceKernelGetMemBlockBase(blk, (void **)&g_obb_mem) < 0) {
        LOGF("  v5.02 obb cache alloc failed 0x%08X\n", blk);
        return 0;
    }
    sceKernelCreateLwMutex(&g_obb_mtx, "zw_obb", 0, 0, NULL);
    for (int i = 0; i < ZW_OBB_NBLK; i++) g_obb_tag[i].obb = -1;
    for (int i = 0; i < ZW_OBB_MAX; i++) g_obb[i].fd = -1;
    hdr_init(g_obb_mem + (size_t)ZW_OBB_BLK * ZW_OBB_NBLK);
    g_obb_ready = 1;
    LOG("  v5.02 obb block cache ready (1.5 MB + 512 KB index)\n");
    return 1;
}
static int obb_index(const char *path) {
    for (int i = 0; i < ZW_OBB_MAX; i++)
        if (g_obb[i].fd >= 0 && strcmp(g_obb[i].path, path) == 0) return i;
    for (int i = 0; i < ZW_OBB_MAX; i++) {
        if (g_obb[i].fd >= 0) continue;
        SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (fd < 0) return -1;
        g_obb[i].fd = fd;
        g_obb[i].len = (long)sceIoLseek(fd, 0, SCE_SEEK_END);
        snprintf(g_obb[i].path, sizeof(g_obb[i].path), "%s", path);
        return i;
    }
    return -1;
}
/* ── v5.06: small-read index for the OBB ──
 * At boot glitch mounts the OBB and the 14 zip archives stored inside it by walking every
 * local file header (about 3500 entries, 4 small reads each). Each header sits in a
 * different 32 KB block, so the block cache missed on nearly all of them: thousands of
 * memory-card reads for about 200 KB of useful bytes. The bytes of every small read are now
 * recorded and saved to obbhdr.bin; the next boot loads that file with one read and serves
 * the same reads from RAM. The same walk happens in play when a level mounts an archive.
 * Extents are keyed by OBB length + offset, so a different OBB simply never matches. */
#ifndef ZW_OBB_HDR
#define ZW_OBB_HDR 1
#endif
#define ZW_HDR_MAXREAD   256
#define ZW_HDR_MAXEXT    8192
#define ZW_HDR_POOL      (384 * 1024)   /* + 8192 x 16 bytes of extents = ZW_OBB_HDR_BYTES */
#define ZW_HDR_PATH      "ux0:data/zombiewood/obbhdr.bin"
typedef struct { uint32_t obblen, pos, off, len; } ZwHdrExt;
static ZwHdrExt *g_hdr_ext;          /* [0, g_hdr_sorted) sorted (loaded); the rest recorded this session */
static uint8_t *g_hdr_pool;
static unsigned g_hdr_n, g_hdr_sorted, g_hdr_pool_n, g_hdr_barrier, g_hdr_saved_n;
static unsigned g_hdr_hits, g_hdr_full;
static SceUInt64 g_hdr_saved_t;
/* v5.06 measurement counters for OBB reads */
static unsigned g_obb_big_n, g_obb_big_kb; static SceUInt64 g_obb_big_us, g_obb_miss_us;

static int hdr_cmp(const void *a, const void *b) {
    const ZwHdrExt *x = a, *y = b;
    if (x->obblen != y->obblen) return x->obblen < y->obblen ? -1 : 1;
    if (x->pos != y->pos) return x->pos < y->pos ? -1 : 1;
    return x->len > y->len ? -1 : (x->len < y->len);
}
static void hdr_init(uint8_t *mem) {
    if (!ZW_OBB_HDR) return;
    ZwHdrExt *ext = (ZwHdrExt *)mem;
    uint8_t *pool = mem + ZW_HDR_MAXEXT * sizeof(ZwHdrExt);
    SceUID fd = sceIoOpen(ZW_HDR_PATH, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        uint32_t h[3] = { 0, 0, 0 };
        int ok = sceIoRead(fd, h, sizeof(h)) == (int)sizeof(h) && h[0] == 0x3148575Au &&
                 h[1] <= ZW_HDR_MAXEXT && h[2] <= ZW_HDR_POOL &&
                 sceIoRead(fd, ext, h[1] * sizeof(ZwHdrExt)) == (int)(h[1] * sizeof(ZwHdrExt)) &&
                 sceIoRead(fd, pool, h[2]) == (int)h[2];
        sceIoClose(fd);
        for (unsigned i = 0; ok && i < h[1]; i++)
            if (ext[i].len == 0 || ext[i].off > h[2] || ext[i].len > h[2] - ext[i].off) ok = 0;
        if (ok) {
            qsort(ext, h[1], sizeof(ZwHdrExt), hdr_cmp);
            g_hdr_n = g_hdr_sorted = g_hdr_barrier = g_hdr_saved_n = h[1];
            g_hdr_pool_n = h[2];
        }
        LOGF("  v5.06 obb index %s: %u extents, %u KB\n", ok ? "loaded" : "rejected", (unsigned)h[1], (unsigned)(h[2] >> 10));
    } else LOG("  v5.06 obb index: none yet, recording this session\n");
    g_hdr_pool = pool;
    g_hdr_ext = ext;
}
/* both called with g_obb_mtx held */
static int hdr_lookup(uint32_t obblen, uint32_t pos, void *dst, uint32_t n) {
    unsigned lo = 0, hi = g_hdr_sorted;
    while (lo < hi) {   /* last extent starting at or before pos */
        unsigned mid = (lo + hi) / 2;
        const ZwHdrExt *e = &g_hdr_ext[mid];
        if (e->obblen < obblen || (e->obblen == obblen && e->pos <= pos)) lo = mid + 1; else hi = mid;
    }
    /* extents that start at the same offset are sorted longest first; step back over them */
    for (unsigned i = lo; i > 0 && lo - i < 4; i--) {
        const ZwHdrExt *e = &g_hdr_ext[i - 1];
        if (e->obblen != obblen) break;
        if (pos >= e->pos && pos - e->pos <= e->len && n <= e->len - (pos - e->pos)) {
            memcpy(dst, g_hdr_pool + e->off + (pos - e->pos), n);
            g_hdr_hits++;
            return 1;
        }
    }
    return 0;
}
static void hdr_record(uint32_t obblen, uint32_t pos, const void *src, uint32_t n) {
    if (g_hdr_pool_n + n > ZW_HDR_POOL) { g_hdr_full = 1; return; }
    ZwHdrExt *last = g_hdr_n > g_hdr_barrier ? &g_hdr_ext[g_hdr_n - 1] : NULL;
    if (last && last->obblen == obblen && last->pos + last->len == pos && last->off + last->len == g_hdr_pool_n) {
        last->len += n;   /* sequential read: grow the previous extent */
    } else {
        if (g_hdr_n >= ZW_HDR_MAXEXT) { g_hdr_full = 1; return; }
        ZwHdrExt *e = &g_hdr_ext[g_hdr_n++];
        e->obblen = obblen; e->pos = pos; e->off = g_hdr_pool_n; e->len = n;
    }
    memcpy(g_hdr_pool + g_hdr_pool_n, src, n);
    g_hdr_pool_n += n;
}
/* Called from the power-tick thread every 5 s. Writes the index when it has grown: once
 * after boot, then at most once a minute. Extents below the barrier are never changed
 * again, so the write itself runs without the lock. */
static void zw_obb_hdr_save_if_dirty(void) {
    if (!g_hdr_ext || g_obb_ready != 1 || g_main_loop_frame < 60) return;
    SceUInt64 now = sceKernelGetProcessTimeWide();
    sceKernelLockLwMutex(&g_obb_mtx, 1, NULL);
    unsigned ne = g_hdr_n, np = g_hdr_pool_n;
    int due = ne > g_hdr_saved_n && (!g_hdr_saved_t || (ne - g_hdr_saved_n >= 16 && now - g_hdr_saved_t > 60000000ull));
    if (due) g_hdr_barrier = ne;
    sceKernelUnlockLwMutex(&g_obb_mtx, 1);
    if (!due) return;
    SceUID fd = sceIoOpen(ZW_HDR_PATH ".tmp", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return;
    uint32_t h[3] = { 0x3148575Au, ne, np };
    int ok = sceIoWrite(fd, h, sizeof(h)) == (int)sizeof(h) &&
             sceIoWrite(fd, g_hdr_ext, ne * sizeof(ZwHdrExt)) == (int)(ne * sizeof(ZwHdrExt)) &&
             sceIoWrite(fd, g_hdr_pool, np) == (int)np;
    sceIoClose(fd);
    if (ok) { sceIoRemove(ZW_HDR_PATH); ok = sceIoRename(ZW_HDR_PATH ".tmp", ZW_HDR_PATH) >= 0; }
    g_hdr_saved_n = ne; g_hdr_saved_t = now;
    LOGF("  v5.06 obb index %s: %u extents, %u KB (hits %u%s)\n", ok ? "saved" : "save FAILED", ne, np >> 10,
         g_hdr_hits, g_hdr_full ? ", FULL" : "");
}
static void zw_obb_stats(const char *when) {
    LOGF("  v5.06 obb %s: index hits %u ext %u/%u pool %u KB | block hits %u miss %u (%u ms) | direct %u reads %u KB (%u ms)\n",
         when, g_hdr_hits, g_hdr_sorted, g_hdr_n, g_hdr_pool_n >> 10, g_obb_hits, g_obb_miss,
         (unsigned)(g_obb_miss_us / 1000), g_obb_big_n, g_obb_big_kb, (unsigned)(g_obb_big_us / 1000));
}

/* copy n bytes at pos of obb idx into dst; returns bytes copied */
static long obb_read(int idx, long pos, void *dst, long n) {
    long len = g_obb[idx].len;
    if (pos >= len || n <= 0) return 0;
    if (n > len - pos) n = len - pos;
    if (n >= 2 * ZW_OBB_BLK) {
        SceUInt64 t0 = sceKernelGetProcessTimeWide();
        int r = sceIoPread(g_obb[idx].fd, dst, n, pos);
        g_obb_big_us += sceKernelGetProcessTimeWide() - t0; g_obb_big_n++; g_obb_big_kb += (unsigned)(n >> 10);
        return r > 0 ? r : 0;
    }
    long done = 0;
    sceKernelLockLwMutex(&g_obb_mtx, 1, NULL);
    int small = g_hdr_ext && n <= ZW_HDR_MAXREAD;
    if (small && hdr_lookup((uint32_t)len, (uint32_t)pos, dst, (uint32_t)n)) {
        sceKernelUnlockLwMutex(&g_obb_mtx, 1);
        return n;
    }
    while (done < n) {
        long b = (pos + done) / ZW_OBB_BLK, off = (pos + done) % ZW_OBB_BLK;
        int slot = -1, victim = 0;
        for (int i = 0; i < ZW_OBB_NBLK; i++) {
            if (g_obb_tag[i].obb == idx && g_obb_tag[i].blk == b) { slot = i; break; }
            if (g_obb_tag[i].stamp < g_obb_tag[victim].stamp) victim = i;
        }
        uint8_t *mem;
        if (slot >= 0) { g_obb_hits++; mem = g_obb_mem + (size_t)slot * ZW_OBB_BLK; }
        else {
            g_obb_miss++;
            slot = victim;
            mem = g_obb_mem + (size_t)slot * ZW_OBB_BLK;
            g_obb_tag[slot].obb = -1;
            SceUInt64 t0 = sceKernelGetProcessTimeWide();
            int pr = sceIoPread(g_obb[idx].fd, mem, ZW_OBB_BLK, (SceOff)b * ZW_OBB_BLK);
            g_obb_miss_us += sceKernelGetProcessTimeWide() - t0;
            if (pr <= 0) break;
            g_obb_tag[slot].obb = idx; g_obb_tag[slot].blk = b;
        }
        g_obb_tag[slot].stamp = ++g_obb_clock;
        long take = ZW_OBB_BLK - off; if (take > n - done) take = n - done;
        if ((b + 1) * ZW_OBB_BLK > len && take > len - (pos + done)) take = len - (pos + done);
        memcpy((uint8_t *)dst + done, mem + off, take);
        done += take;
    }
    if (small && done == n) hdr_record((uint32_t)len, (uint32_t)pos, dst, (uint32_t)n);
    sceKernelUnlockLwMutex(&g_obb_mtx, 1);
    return done;
}
static long vf_len(VirtualFile *vf) { return vf->obb ? g_obb[vf->obb_idx].len : vf->asset->len; }
static long vf_copy(VirtualFile *vf, long pos, void *dst, long n) {
    if (vf->obb) return obb_read(vf->obb_idx, pos, dst, n);
    long avail = vf->asset->len - pos; if (avail < 0) avail = 0;
    if (n > avail) n = avail;
    if (n > 0) memcpy(dst, vf->asset->data + pos, n);
    return n;
}
static int vf_byte(VirtualFile *vf) {
    unsigned char c;
    if (vf_copy(vf, vf->pos, &c, 1) != 1) return EOF;
    vf->pos++;
    return c;
}
static FILE *obb_vfopen(const char *path) {
    if (!obb_cache_init()) return NULL;
    sceKernelLockLwMutex(&g_obb_mtx, 1, NULL);
    int idx = obb_index(path);
    FILE *ret = NULL;
    if (idx >= 0) {
        VirtualFile *vf = vf_alloc();
        if (vf) { vf->obb = 1; vf->obb_idx = idx; ret = (FILE *)vf; }
    }
    sceKernelUnlockLwMutex(&g_obb_mtx, 1);
    return ret;
}

/* ── v5.03: RAM cache for loose sound files ──
 * Vox opens each loose .wav (436 files, median 33 KB) for every play and does ~13 fseek +
 * ~20 small fread; newlib refills its buffer from the memory card on each seek (~4 ms),
 * so every shot/footstep cost 55-250 ms. Small .wav files are read once, whole, into fixed
 * slots (own memblock, LRU, ref-counted while open) and served as VirtualFiles. Larger files
 * (ambient loops) keep using stdio.
 *
 * v5.14: measured in a busy level - the FIRST use of each sound still read the file on the
 * game thread: 6-40 ms per sound (card open + read), several per second in a fight, which is
 * the stutter in intense scenes (frames of 80-200 ms). Now:
 *  - the same 6 MB block is cut into four size classes (164 sounds instead of 72);
 *  - a background thread loads sounds ahead of use: at start the sounds used most recently in
 *    earlier sessions (ux0:data/zombiewood/wavhist.txt, written by this loader), and after
 *    every miss the numbered siblings of that sound (zombie_kill_3 -> _1.._12).
 * The game thread only reads a file itself when the sound is in neither set. */
static const struct { int n; long sz; } g_wav_cls[] = { { 64, 16 * 1024 }, { 56, 32 * 1024 }, { 40, 64 * 1024 }, { 4, 160 * 1024 } };
#define ZW_WAV_NCLS   4
#define ZW_WAV_NSLOT  (64 + 56 + 40 + 4)
#define ZW_WAV_MAX    (160 * 1024)
#ifndef ZW_WAV_PREFETCH
#define ZW_WAV_PREFETCH 1
#endif
#define ZW_WAV_HIST_N 320
#define ZW_WAV_HIST_PATH "ux0:data/zombiewood/wavhist.txt"
#define ZW_WAV_Q_N    64
typedef struct { VirtualAssetCache va; uint32_t hash; unsigned stamp; int refs; long cap; int loading; int cls; } WavSlot;
static WavSlot g_wav[ZW_WAV_NSLOT];
static unsigned g_wav_clock, g_wav_hits, g_wav_miss, g_wav_pre, g_wav_miss_us;
static SceKernelLwMutexWork g_wav_mtx;
static int g_wav_ready = -1;
static struct { char path[100]; uint32_t hash; unsigned stamp; } g_wav_hist[ZW_WAV_HIST_N];
static unsigned g_wav_hist_clock; static volatile int g_wav_hist_dirty;
static char g_wav_q[ZW_WAV_Q_N][128]; static volatile unsigned g_wav_q_r, g_wav_q_w;
static int wav_cache_init(void) {
    if (g_wav_ready >= 0) return g_wav_ready;
    g_wav_ready = 0;
    size_t total = 0;
    for (int c = 0; c < ZW_WAV_NCLS; c++) total += (size_t)g_wav_cls[c].n * (size_t)g_wav_cls[c].sz;
    total = (total + 0xfffff) & ~(size_t)0xfffff;
    SceUID blk = sceKernelAllocMemBlock("zw_wavcache", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, total, NULL);
    uint8_t *mem = NULL;
    if (blk < 0 || sceKernelGetMemBlockBase(blk, (void **)&mem) < 0) {
        LOGF("  v5.03 wav cache alloc failed 0x%08X\n", blk);
        return 0;
    }
    int i = 0;
    for (int c = 0; c < ZW_WAV_NCLS; c++)
        for (int k = 0; k < g_wav_cls[c].n; k++, i++) {
            g_wav[i].va.data = mem; g_wav[i].cap = g_wav_cls[c].sz; g_wav[i].cls = c;
            mem += g_wav_cls[c].sz;
            g_wav[i].va.path[0] = 0;
        }
    sceKernelCreateLwMutex(&g_wav_mtx, "zw_wav", 0, 0, NULL);
    g_wav_ready = 1;
    LOGF("  v5.14 wav cache ready (%u KB, %d slots)\n", (unsigned)(total >> 10), ZW_WAV_NSLOT);
    return 1;
}
static uint32_t zw_strhash(const char *s) { uint32_t h = 2166136261u; for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; } return h; }
/* all *_locked helpers: g_wav_mtx held */
static WavSlot *wav_find_locked(const char *path, uint32_t h) {
    for (int i = 0; i < ZW_WAV_NSLOT; i++)
        if (g_wav[i].va.path[0] && g_wav[i].hash == h && strcmp(g_wav[i].va.path, path) == 0) return &g_wav[i];
    return NULL;
}
/* least recently used free slot of the smallest class that fits; a prefetch (older_than != 0)
 * may only take slots that are empty or have not been used for a while */
static WavSlot *wav_victim_locked(long len, unsigned older_than) {
    for (int c = 0; c < ZW_WAV_NCLS; c++) {
        if (len > g_wav_cls[c].sz) continue;
        WavSlot *best = NULL;
        for (int i = 0; i < ZW_WAV_NSLOT; i++) {
            WavSlot *s = &g_wav[i];
            if (s->cls != c || s->refs || s->loading) continue;
            if (older_than && s->va.path[0] && s->stamp + older_than > g_wav_clock) continue;
            if (!s->va.path[0]) { best = s; break; }
            if (!best || s->stamp < best->stamp) best = s;
        }
        if (best) return best;
        if (older_than) return NULL;   /* a prefetch never spills into a bigger class */
    }
    return NULL;
}
static void wav_hist_touch_locked(const char *path, uint32_t h) {
    int old = 0;
    if (strlen(path) >= sizeof(g_wav_hist[0].path)) return;
    for (int i = 0; i < ZW_WAV_HIST_N; i++) {
        if (g_wav_hist[i].stamp && g_wav_hist[i].hash == h && strcmp(g_wav_hist[i].path, path) == 0) { g_wav_hist[i].stamp = ++g_wav_hist_clock; return; }
        if (g_wav_hist[i].stamp < g_wav_hist[old].stamp) old = i;
    }
    strcpy(g_wav_hist[old].path, path); g_wav_hist[old].hash = h; g_wav_hist[old].stamp = ++g_wav_hist_clock;
    g_wav_hist_dirty = 1;
}
static void wav_queue_locked(const char *path) {
    if (strlen(path) >= sizeof(g_wav_q[0]) || g_wav_q_w - g_wav_q_r >= ZW_WAV_Q_N) return;
    strcpy(g_wav_q[g_wav_q_w % ZW_WAV_Q_N], path); g_wav_q_w++;
}
/* names of the .wav files present at start (hash of the name after the last '/'), so siblings
 * that do not exist are never probed on the card */
static uint32_t g_wav_names[1024]; static int g_wav_names_n;
static int wav_exists_locked(const char *path) {
    const char *s = strrchr(path, '/'); uint32_t h = zw_strhash(s ? s + 1 : path);
    for (int i = 0; i < g_wav_names_n; i++) if (g_wav_names[i] == h) return 1;
    return 0;
}
/* "..._3.wav" / "..._03.wav": queue the other numbers of the same family */
static void wav_queue_siblings_locked(const char *path) {
    size_t n = strlen(path);
    if (n < 6 || n >= 120) return;
    size_t e = n - 4, d = e;   /* e = index of ".wav" */
    while (d > 0 && path[d - 1] >= '0' && path[d - 1] <= '9') d--;
    if (d == e || e - d > 2 || d == 0) return;
    int width = (path[d] == '0' && e - d == 2) ? 2 : 0, self = atoi(path + d);
    char buf[128];
    for (int k = 1; k <= 12; k++) {
        if (k == self) continue;
        if (width) snprintf(buf, sizeof(buf), "%.*s%02d.wav", (int)d, path, k);
        else snprintf(buf, sizeof(buf), "%.*s%d.wav", (int)d, path, k);
        if (wav_exists_locked(buf)) wav_queue_locked(buf);
    }
}
static FILE *wav_vfopen(const char *path) {
    if (!wav_cache_init()) return NULL;
    uint32_t h = zw_strhash(path);
    sceKernelLockLwMutex(&g_wav_mtx, 1, NULL);
    WavSlot *slot = wav_find_locked(path, h);
    if (slot) g_wav_hits++;
    else {
        SceUInt64 t0 = sceKernelGetProcessTimeWide();
        SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (fd < 0) { sceKernelUnlockLwMutex(&g_wav_mtx, 1); return NULL; }
        long len = (long)sceIoLseek(fd, 0, SCE_SEEK_END);
        if (len > ZW_WAV_MAX || len <= 0) { sceIoClose(fd); sceKernelUnlockLwMutex(&g_wav_mtx, 1); return NULL; }
        slot = wav_victim_locked(len, 0);
        if (!slot) { sceIoClose(fd); sceKernelUnlockLwMutex(&g_wav_mtx, 1); return NULL; }
        slot->va.path[0] = 0;
        int got = sceIoPread(fd, slot->va.data, len, 0);
        sceIoClose(fd);
        if (got != len) { sceKernelUnlockLwMutex(&g_wav_mtx, 1); return NULL; }
        slot->va.len = len;
        slot->hash = h;
        snprintf(slot->va.path, sizeof(slot->va.path), "%s", path);
        g_wav_miss++; g_wav_miss_us += (unsigned)(sceKernelGetProcessTimeWide() - t0);
        if (ZW_WAV_PREFETCH) wav_queue_siblings_locked(path);
    }
    slot->stamp = ++g_wav_clock;
    if (ZW_WAV_PREFETCH) wav_hist_touch_locked(path, h);
    FILE *ret = NULL;
    VirtualFile *vf = vf_alloc();
    if (vf) { vf->asset = &slot->va; slot->refs++; ret = (FILE *)vf; }
    sceKernelUnlockLwMutex(&g_wav_mtx, 1);
    return ret;
}
static void wav_release(VirtualAssetCache *va) {
    if (g_wav_ready != 1 || !va) return;
    if ((void *)va < (void *)&g_wav[0] || (void *)va >= (void *)&g_wav[ZW_WAV_NSLOT]) return;
    sceKernelLockLwMutex(&g_wav_mtx, 1, NULL);
    WavSlot *s = (WavSlot *)va;
    if (s->refs > 0) s->refs--;
    sceKernelUnlockLwMutex(&g_wav_mtx, 1);
}
/* background thread: the card read happens outside the lock, so the game thread never waits for it.
 * returns 1 loaded/already there, 0 no such small file, -1 no slot free for it */
static int wav_prefetch_one(const char *path, unsigned older_than) {
    uint32_t h = zw_strhash(path);
    sceKernelLockLwMutex(&g_wav_mtx, 1, NULL);
    int have = wav_find_locked(path, h) != NULL;
    sceKernelUnlockLwMutex(&g_wav_mtx, 1);
    if (have) return 1;
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    long len = (long)sceIoLseek(fd, 0, SCE_SEEK_END);
    if (len > ZW_WAV_MAX || len <= 0) { sceIoClose(fd); return 0; }
    sceKernelLockLwMutex(&g_wav_mtx, 1, NULL);
    WavSlot *slot = wav_victim_locked(len, older_than);
    if (slot) { slot->va.path[0] = 0; slot->loading = 1; }
    sceKernelUnlockLwMutex(&g_wav_mtx, 1);
    if (!slot) { sceIoClose(fd); return -1; }
    int got = sceIoPread(fd, slot->va.data, len, 0);
    sceIoClose(fd);
    sceKernelLockLwMutex(&g_wav_mtx, 1, NULL);
    if (got == len) {
        slot->va.len = len; slot->hash = h;
        snprintf(slot->va.path, sizeof(slot->va.path), "%s", path);
        slot->stamp = g_wav_clock > 100 ? g_wav_clock - 100 : 1;   /* ranks behind sounds really played lately */
        g_wav_pre++;
    }
    slot->loading = 0;
    sceKernelUnlockLwMutex(&g_wav_mtx, 1);
    return got == len;
}
static int wav_hist_cmp(const void *a, const void *b) {
    unsigned x = g_wav_hist[*(const uint16_t *)a].stamp, y = g_wav_hist[*(const uint16_t *)b].stamp;
    return x > y ? -1 : x < y;
}
static void wav_hist_save(void) {
    static char buf[ZW_WAV_HIST_N * 101]; static uint16_t ord[ZW_WAV_HIST_N];
    int n = 0, m = 0;
    sceKernelLockLwMutex(&g_wav_mtx, 1, NULL);
    g_wav_hist_dirty = 0;
    for (int i = 0; i < ZW_WAV_HIST_N; i++) if (g_wav_hist[i].stamp) ord[m++] = (uint16_t)i;
    qsort(ord, (size_t)m, sizeof(ord[0]), wav_hist_cmp);
    for (int i = 0; i < m; i++) n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s\n", g_wav_hist[ord[i]].path);
    sceKernelUnlockLwMutex(&g_wav_mtx, 1);
    SceUID fd = sceIoOpen(ZW_WAV_HIST_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) { sceIoWrite(fd, buf, (SceSize)n); sceIoClose(fd); }
}
static int wav_prefetch_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    {   /* sounds of earlier sessions, most recently used first, into empty slots only */
        static char buf[ZW_WAV_HIST_N * 101];
        SceUID fd = sceIoOpen(ZW_WAV_HIST_PATH, SCE_O_RDONLY, 0);
        int n = fd >= 0 ? sceIoRead(fd, buf, sizeof(buf) - 1) : 0, lines = 0, full = 0;
        if (fd >= 0) sceIoClose(fd);
        if (n < 0) n = 0;
        buf[n] = 0;
        SceUInt64 t0 = sceKernelGetProcessTimeWide();
        unsigned st = ZW_WAV_HIST_N + 1;
        for (char *p = buf; *p; ) {
            char *e = strchr(p, '\n'); if (e) *e = 0;
            if (*p && strlen(p) < sizeof(g_wav_hist[0].path) && lines < ZW_WAV_HIST_N) {
                sceKernelLockLwMutex(&g_wav_mtx, 1, NULL);
                int k = lines;   /* keep the saved order in the in-memory history */
                if (!g_wav_hist[k].stamp) { strcpy(g_wav_hist[k].path, p); g_wav_hist[k].hash = zw_strhash(p); g_wav_hist[k].stamp = st - (unsigned)lines; }
                sceKernelUnlockLwMutex(&g_wav_mtx, 1);
                lines++;
                if (full < 12 && wav_prefetch_one(p, 0x7fffffffu) < 0) full++;   /* that size class is full; later, smaller sounds may still fit */
            }
            if (!e) break;
            p = e + 1;
        }
        sceKernelLockLwMutex(&g_wav_mtx, 1, NULL);
        if (g_wav_hist_clock < st) g_wav_hist_clock = st;
        sceKernelUnlockLwMutex(&g_wav_mtx, 1);
        LOGF("  v5.14 wav prefetch: history %d sounds, %u loaded in %u ms\n", lines, g_wav_pre, (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));
    }
    {   /* directory scan for wav_exists_locked */
        SceUID d = sceIoDopen("ux0:data/zombiewood");
        SceIoDirent de; int n = 0; static uint32_t tmp[1024];
        if (d >= 0) {
            memset(&de, 0, sizeof(de));
            while (n < 1024 && sceIoDread(d, &de) > 0) {
                size_t l = strlen(de.d_name);
                if (l > 4 && !strcmp(de.d_name + l - 4, ".wav")) tmp[n++] = zw_strhash(de.d_name);
            }
            sceIoDclose(d);
        }
        sceKernelLockLwMutex(&g_wav_mtx, 1, NULL);
        memcpy(g_wav_names, tmp, sizeof(uint32_t) * (size_t)n); g_wav_names_n = n;
        sceKernelUnlockLwMutex(&g_wav_mtx, 1);
        LOGF("  v5.14 wav prefetch: %d sound files on the card\n", n);
    }
    unsigned idle = 0;
    for (;;) {
        char path[128]; int have = 0;
        sceKernelLockLwMutex(&g_wav_mtx, 1, NULL);
        if (g_wav_q_r != g_wav_q_w) { strcpy(path, g_wav_q[g_wav_q_r % ZW_WAV_Q_N]); g_wav_q_r++; have = 1; }
        sceKernelUnlockLwMutex(&g_wav_mtx, 1);
        /* one file, then a pause: the memory card serves one request at a time, and a burst of
         * background reads stalled the game thread's own reads (music stream) for 150-450 ms */
        if (have) { wav_prefetch_one(path, 60); sceKernelDelayThread(250 * 1000); continue; }
        sceKernelDelayThread(30 * 1000);
        if (++idle >= 400) { idle = 0; if (g_wav_hist_dirty) wav_hist_save(); }   /* about every 12 s */
    }
    return 0;
}
static void zw_wav_prefetch_start(void) {
    static int started;
    if (started || !ZW_WAV_PREFETCH || !wav_cache_init()) return;
    started = 1;
    SceUID t = sceKernelCreateThread("zw_wav_prefetch", wav_prefetch_thread, 0x10000100 + 16, 0x4000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
    else LOGF("  v5.14 wav prefetch thread failed 0x%08X\n", (unsigned)t);
}

static VirtualAssetCache *virtual_asset_cache_find(const char *path) {
    if (!path) return NULL;
    for (int i = 0; i < VIRTUAL_ASSET_CACHE_SLOTS; i++) {
        if (g_virtual_asset_cache[i].data &&
            strcmp(g_virtual_asset_cache[i].path, path) == 0) {
            return &g_virtual_asset_cache[i];
        }
    }
    return NULL;
}

static VirtualAssetCache *virtual_asset_cache_load(const char *path) {
    VirtualAssetCache *hit = virtual_asset_cache_find(path);
    if (hit) return hit;

#ifdef VITA3K_BUILD
    if (path && strstr(path, ".swf"))
        sceClibPrintf("[ZW] vasset cache load begin '%s'\n", path);
#endif
    FILE *f = fopen(path, "rb");
    if (!f) {
#ifdef VITA3K_BUILD
        if (path && strstr(path, ".swf"))
            sceClibPrintf("[ZW] vasset cache fopen failed '%s'\n", path);
#endif
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
#ifdef VITA3K_BUILD
        if (path && strstr(path, ".swf"))
            sceClibPrintf("[ZW] vasset cache fseek end failed '%s'\n", path);
#endif
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0) {
#ifdef VITA3K_BUILD
        if (path && strstr(path, ".swf"))
            sceClibPrintf("[ZW] vasset cache size/rewind failed '%s' len=%ld\n",
                          path, len);
#endif
        fclose(f);
        return NULL;
    }

    unsigned char *data = NULL;
    if (len > 0) {
        data = (unsigned char *)malloc((size_t)len);
        if (!data) {
#ifdef VITA3K_BUILD
            if (path && strstr(path, ".swf"))
                sceClibPrintf("[ZW] vasset cache malloc failed '%s' len=%ld\n",
                              path, len);
#endif
            fclose(f);
            return NULL;
        }
#ifdef VITA3K_BUILD
        if (path && strstr(path, ".swf"))
            sceClibPrintf("[ZW] vasset cache fread begin '%s' len=%ld\n", path, len);
#endif
        size_t got = fread(data, 1, (size_t)len, f);
        if (got != (size_t)len) {
#ifdef VITA3K_BUILD
            if (path && strstr(path, ".swf"))
                sceClibPrintf("[ZW] vasset cache fread short '%s' got=%u len=%ld\n",
                              path, (unsigned)got, len);
#endif
            free(data);
            fclose(f);
            return NULL;
        }
    }
    fclose(f);

    VirtualAssetCache *slot =
        &g_virtual_asset_cache[g_virtual_asset_cache_next++ % VIRTUAL_ASSET_CACHE_SLOTS];
    if (slot->data) free(slot->data);
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    slot->data = data;
    slot->len = len;
#ifdef VITA3K_BUILD
    if (path && strstr(path, ".swf"))
        sceClibPrintf("[ZW] vasset cache loaded '%s' len=%ld\n", path, len);
#endif
    return slot;
}

static FILE *virtual_asset_fopen(const char *path) {
    VirtualAssetCache *asset = virtual_asset_cache_load(path);
    if (!asset) return NULL;

    VirtualFile *vf = vf_alloc();
    if (vf) vf->asset = asset;
    return (FILE *)vf;
}

static FILE *virtual_sink_fopen(const char *path) {
    (void)path;
    VirtualFile *vf = vf_alloc();
    if (vf) vf->sink = 1;
    return (FILE *)vf;
}

#ifdef VITA3K_BUILD
static FILE *materialized_asset_fopen(const char *path, const char *mode) {
    VirtualAssetCache *asset = virtual_asset_cache_load(path);
    if (!asset) return NULL;

    const char *slash = strrchr(path, '/');
    const char *fname = slash ? slash + 1 : path;
    if (!fname || !fname[0]) return NULL;

    char out_path[512];
    snprintf(out_path, sizeof(out_path),
             "ux0:/data/zombiewood/writecache/%s", fname);

    SceIoStat st;
    memset(&st, 0, sizeof(st));
    if (sceIoGetstat(out_path, &st) >= 0 && st.st_size == asset->len) {
        FILE *f = fopen(out_path, mode);
        sceClibPrintf("[ZW] cached materialized fopen('%s' -> '%s','%s') -> %p len=%ld\n",
                      path, out_path, mode ? mode : "(null)", f, asset->len);
        return f;
    }

    SceUID fd = sceIoOpen(out_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        sceClibPrintf("[ZW] materialize('%s') open failed: 0x%08X\n", out_path, fd);
        return NULL;
    }

    long written_total = 0;
    while (written_total < asset->len) {
        long chunk = asset->len - written_total;
        if (chunk > 64 * 1024) chunk = 64 * 1024;
        int written = sceIoWrite(fd, asset->data + written_total, (SceSize)chunk);
        if (written <= 0) {
            sceClibPrintf("[ZW] materialize('%s') write failed after %ld/%ld: 0x%08X\n",
                          out_path, written_total, asset->len, written);
            sceIoClose(fd);
            return NULL;
        }
        written_total += written;
    }
    sceIoClose(fd);

    FILE *f = fopen(out_path, mode);
    sceClibPrintf("[ZW] materialized fopen('%s' -> '%s','%s') -> %p len=%ld\n",
                  path, out_path, mode ? mode : "(null)", f, asset->len);
    return f;
}
#endif

static const char *redirect_path_r(const char *android_path, char *out) {
    if (!android_path) return android_path;
    if (strcmp(android_path, "/proc/meminfo") == 0) {
        snprintf(out, ZW_PATH_MAX,
                 "ux0:/data/zombiewood/proc_meminfo.txt");
        return out;
    }
    if (strstr(android_path, "obb")       ||
        strstr(android_path, "gameloft")  ||
        strstr(android_path, "Gameloft")  ||              /* v4.45: ./Gameloft_logo.png */
        strncmp(android_path, "./", 2) == 0 ||             /* v4.45: any cwd-relative file */
        strstr(android_path, "zombiewood") ||
        strstr(android_path, "/sdcard")   ||
        strstr(android_path, "/data/data") ||
        strstr(android_path, "/mnt/sdcard") ||
        fuzzy_analytics_name(android_path) ||
        analytics_suffix_from_path(android_path) ||
        dollar_tail_filename(android_path) ||
        is_zombiewood_asset_path(android_path)) {
        /* Strip to just basename and place under Vita data dir */
        const char *slash = strrchr(android_path, '/');
        const char *fname = slash ? slash + 1 : android_path;
        if (fname[0] == '.' && fname[1] == '/') {
            fname += 2;
        }

        const char *analytics = fuzzy_analytics_name(fname);
        if (!analytics) analytics = analytics_suffix_from_path(fname);
        if (analytics) {
            fname = analytics;
            snprintf(out, ZW_PATH_MAX,
                     "ux0:/data/zombiewood/writecache/%s", fname);
        if (should_trace_file_path(out))
            LOGF("path redirect: %s  ->  %s (analytics normalized)\n",
                 android_path, out);
            return out;
        }

        const char *dollar_tail = dollar_tail_filename(fname);
        if (dollar_tail) {
            snprintf(out, ZW_PATH_MAX,
                     "ux0:/data/zombiewood/writecache/%s", dollar_tail);
            if (should_trace_file_path(out))
                LOGF("path redirect: %s  ->  %s (dollar tail normalized)\n",
                     android_path, out);
            return out;
        }

        snprintf(out, ZW_PATH_MAX,
                 "ux0:/data/zombiewood/%s", fname);
        if (should_trace_file_path(out))
            LOGF("path redirect: %s  ->  %s\n", android_path, out);
        return out;
    }
    return android_path;
}
#define redirect_path(p) redirect_path_r((p), (char[ZW_PATH_MAX]){0})

/* v4.58: negative cache for missing read-only game assets. The game keeps
 * probing optional variants (e.g. *_LOW.bdae, *_mask.TGA) every time an
 * object spawns; each probe was a synchronous memory-card stat/fopen plus a
 * log line, costing a lot of frame time on hardware. */
#define ZW_NEG_SLOTS 2048
static uint32_t g_neg_hash[ZW_NEG_SLOTS];
static unsigned g_neg_count = 0, g_neg_hits = 0;
static uint32_t zw_fnv(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h ? h : 1;
}
static int zw_neg_cacheable(const char *rp) {
    if (!rp || strncmp(rp, "ux0:/data/zombiewood/", 21) != 0) return 0;
    if (strstr(rp, "/writecache/") || strstr(rp, "/shaders/")) return 0;
    const char *dot = strrchr(rp, '.');
    if (!dot) return 0;
    return !strcasecmp(dot, ".bdae") || !strcasecmp(dot, ".tga") || !strcasecmp(dot, ".dds") ||
           !strcasecmp(dot, ".png") || !strcasecmp(dot, ".pvr") || !strcasecmp(dot, ".jpg") ||
           !strcasecmp(dot, ".ktx");
}
static int zw_neg_lookup(const char *rp) {
    if (!zw_neg_cacheable(rp)) return 0;
    uint32_t h = zw_fnv(rp);
    for (unsigned i = 0; i < ZW_NEG_SLOTS; i++) {
        uint32_t v = g_neg_hash[(h + i) % ZW_NEG_SLOTS];
        if (!v) return 0;
        if (v == h) { g_neg_hits++; return 1; }
    }
    return 0;
}
static void zw_neg_add(const char *rp) {
    if (!zw_neg_cacheable(rp) || g_neg_count >= ZW_NEG_SLOTS * 3 / 4) return;
    uint32_t h = zw_fnv(rp);
    for (unsigned i = 0; i < ZW_NEG_SLOTS; i++) {
        uint32_t *slot = &g_neg_hash[(h + i) % ZW_NEG_SLOTS];
        if (*slot == h) return;
        if (!*slot) { *slot = h; g_neg_count++; return; }
    }
}

/* v5.01: per-frame time spent in hitch-prone calls, reported only for slow frames.
 * 0 = fopen, 1 = fread, 2 = fseek, 3 = glCompileShader, 4 = glLinkProgram, 5 = tex upload */
volatile uint64_t g_zw_prof_us[6];
volatile unsigned g_zw_prof_n[6];
volatile uint64_t g_zw_wr_us, g_zw_cl_us; volatile unsigned g_zw_wr_n, g_zw_cl_n;   /* v5.10 write-path timing */
#define ZW_PROF(slot, call) do { SceUInt64 _pt = sceKernelGetProcessTimeWide(); call; \
    g_zw_prof_us[slot] += sceKernelGetProcessTimeWide() - _pt; g_zw_prof_n[slot]++; } while (0)
static FILE *vita_fopen_impl(const char *path, const char *mode);
static size_t vita_fread_impl(void *ptr, size_t size, size_t nmemb, FILE *stream);
static int vita_fseek_impl(FILE *stream, long offset, int whence);
#ifndef ZW_DRAW_STATS
#define ZW_DRAW_STATS 0   /* 1 = measurement: per-class draw call timing (dynlib.c) + slow fopen lines; never ship 1 */
#endif
FILE *vita_fopen(const char *path, const char *mode) {
#if ZW_DRAW_STATS
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    FILE *r0 = vita_fopen_impl(path, mode);
    SceUInt64 dt = sceKernelGetProcessTimeWide() - t0;
    g_zw_prof_us[0] += dt; g_zw_prof_n[0]++;
    static unsigned nlog;
    if (g_main_loop_frame > 600 && dt > 800 && nlog < 120) { nlog++; log_printf("  stats fopen f%d %u us '%s' '%s' -> %p\n", g_main_loop_frame, (unsigned)dt, path ? path : "?", mode ? mode : "?", (void *)r0); }
    return r0;
#else
    FILE *r; ZW_PROF(0, r = vita_fopen_impl(path, mode)); return r;
#endif
}
/* v5.17: the stream with the most fread time this frame (spike log names it) */
FILE *g_zw_fread_top; uint64_t g_zw_fread_top_us;
static FILE *s_fr_last; static uint64_t s_fr_last_us;
size_t vita_fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    size_t r; ZW_PROF(1, r = vita_fread_impl(ptr, size, nmemb, stream));
    uint64_t dt = sceKernelGetProcessTimeWide() - t0;
    if (stream != s_fr_last) { s_fr_last = stream; s_fr_last_us = 0; }
    s_fr_last_us += dt;
    if (s_fr_last_us > g_zw_fread_top_us) { g_zw_fread_top_us = s_fr_last_us; g_zw_fread_top = stream; }
    return r;
}
int vita_fseek(FILE *stream, long offset, int whence) {
    int r; ZW_PROF(2, r = vita_fseek_impl(stream, offset, whence)); return r;
}

/* Wrapped fopen that intercepts Android paths.
   Non-static so dynlib.c can take its address for the hook table. */
static FILE *vita_fopen_impl(const char *path, const char *mode) {
    char g_path_buf[ZW_PATH_MAX], g_write_path_buf[ZW_PATH_MAX];   /* v5.04: per call */
    /* v4.85: audio output is not implemented yet. The Vox sound engine keeps every
     * sound's .wav open until the voice finishes playing, which never happens, so
     * ~30 shots exhausted the 64 stdio handles and the game crashed. Report sound
     * files as missing until real audio playback exists (ZW_AUDIO_FILES 1). */
#ifndef ZW_AUDIO_FILES
#define ZW_AUDIO_FILES 1
#endif
    if (!ZW_AUDIO_FILES && path && mode && is_asset_read_mode(mode) && has_ext(path, ".wav")) {
        errno = ENOENT;
        return NULL;
    }
    if (path && mode && is_asset_read_mode(mode) && zw_neg_lookup(redirect_path(path))) {
        errno = ENOENT;
        return NULL;
    }
    if (path && mode && strchr(path, '$') && is_asset_write_mode(mode)) {
        const char *fname = fuzzy_analytics_name(path);
        if (!fname) fname = analytics_suffix_from_path(path);
        if (!fname) fname = dollar_tail_filename(path);
        if (!fname) fname = strstr(path, ".new") ? "tracker_tmp.dat.new" : "tracker_tmp.dat";

        snprintf(g_path_buf, sizeof(g_path_buf),
                 "ux0:/data/zombiewood/writecache/%s", fname);
        FILE *f = fopen(g_path_buf, mode);
#ifndef VITA3K_BUILD
        LOGF("fopen('%s','%s') -> forced tracker scratch %s -> %p\n",
             path, mode, g_path_buf, f);
#endif
        track_file(f, g_path_buf, mode);
        return f;
    }

    if (path && mode && is_asset_write_mode(mode) &&
        !strchr(path, '/') && !strchr(path, ':') &&
        !is_zombiewood_asset_path(path)) {
        snprintf(g_path_buf, sizeof(g_path_buf),
                 "ux0:/data/zombiewood/writecache/%s",
                 strstr(path, ".new") ? "tracker_tmp.dat.new" : "tracker_tmp.dat");
        FILE *f = fopen(g_path_buf, mode);
#ifndef VITA3K_BUILD
        LOGF("fopen('%s','%s') -> forced relative scratch %s -> %p\n",
             path, mode, g_path_buf, f);
#endif
        track_file(f, g_path_buf, mode);
        return f;
    }

    const char *rp = redirect_path(path);

    if (path && strcmp(path, "/proc/meminfo") == 0 && mode && strchr(mode, 'r')) {
        FILE *mf = fopen(rp, "w");
        if (mf) {
            fputs("MemTotal:         524288 kB\n", mf);
            fputs("MemFree:          262144 kB\n", mf);
            fputs("Buffers:           32768 kB\n", mf);
            fputs("Cached:           131072 kB\n", mf);
            fputs("SwapCached:            0 kB\n", mf);
            fputs("Active:           131072 kB\n", mf);
            fputs("Inactive:          65536 kB\n", mf);
            fclose(mf);
        }
        FILE *f = fopen(rp, mode);
        if (!f) LOGF("fopen('/proc/meminfo','%s') -> fake %p\n", mode, f);
        return f;
    }

    const char *slash = rp ? strrchr(rp, '/') : NULL;
    const char *fname = slash ? slash + 1 : rp;

    if (fname && mode && is_service_scratch_name(fname) && is_asset_write_mode(mode)) {
        FILE *vf = virtual_sink_fopen(rp);
        if (vf) {
            track_file(vf, rp, mode);
            return vf;
        }
    }

    if (fname && is_packaged_asset_name(fname) && is_asset_write_mode(mode)) {
        snprintf(g_write_path_buf, sizeof(g_write_path_buf),
                 "ux0:/data/zombiewood/writecache/%s", fname);
        if (should_trace_file_path(fname))
            LOGF("fopen('%s','%s') -> writecache %s\n", rp, mode, g_write_path_buf);
        rp = g_write_path_buf;
    }

    if (fname && mode &&
        strcmp(fname, "dlc.dat") == 0 &&
        strchr(mode, 'r') &&
        !strchr(mode, '+') &&
        !strchr(mode, 'w') &&
        !strchr(mode, 'a')) {
        LOGF("fopen('%s','%s') -> forced missing DLC cache\n", rp, mode);
        return NULL;
    }

#ifdef VITA3K_BUILD
    if (fname && mode && (has_ext(fname, ".swf") || has_ext(fname, ".mpc")) &&
        is_asset_read_mode(mode) &&
        !is_save_state_name(fname)) {
        FILE *mf = materialized_asset_fopen(rp, mode);
        if (mf) {
            track_file(mf, rp, mode);
            return mf;
        }
    }

    if (fname && mode && is_packaged_asset_name(fname) && is_asset_read_mode(mode) &&
        !is_save_state_name(fname) &&
        (has_ext(fname, ".json") ||
         has_ext(fname, ".xml") ||
         has_ext(fname, ".conf") ||
         has_ext(fname, ".list") ||
         has_ext(fname, ".txt"))) {
        FILE *mf = materialized_asset_fopen(rp, mode);
        if (mf) {
            track_file(mf, rp, mode);
            return mf;
        }
    }
#endif

#ifndef ZW_OBB_CACHE
#define ZW_OBB_CACHE 1
#endif
    if (ZW_OBB_CACHE && rp && fname && has_ext(fname, ".obb") && mode && is_asset_read_mode(mode) &&
        !strchr(mode, '+') && !strchr(mode, 'w') && !strchr(mode, 'a')) {
        FILE *vf = obb_vfopen(rp);   /* v5.02 */
        if (vf) { track_file(vf, rp, mode); return vf; }
    }
#ifndef ZW_WAV_CACHE
#define ZW_WAV_CACHE 1
#endif
    if (ZW_WAV_CACHE && rp && fname && has_ext(fname, ".wav") && is_asset_read_mode(mode)) {
        FILE *vf = wav_vfopen(rp);   /* v5.03; NULL for big/missing files -> normal fopen */
        if (vf) { track_file(vf, rp, mode); return vf; }
    }

    FILE *f = fopen(rp, mode);
    if (f && fname && is_save_state_name(fname) && is_asset_read_mode(mode) &&
        save_file_looks_blank(f)) {
        LOGF("fopen('%s','%s') -> blank save treated as missing\n", rp, mode);
        fclose(f);
        return NULL;
    }
    if (!f && fname && has_ext(fname, ".ttf") && is_asset_read_mode(mode)) {
        const char *fallbacks[] = {
            "ux0:/data/zombiewood/KGFallForYou.ttf",
            "app0:/KGFallForYou.ttf",
            "app0:/Bemio.ttf",
        };
        for (unsigned i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
            if (rp && strcmp(rp, fallbacks[i]) == 0) continue;
            f = fopen(fallbacks[i], mode);
            LOGF("fopen('%s','%s') -> font fallback %s -> %p\n",
                 rp ? rp : "(null)", mode ? mode : "(null)", fallbacks[i], f);
            if (f) {
                track_file(f, fallbacks[i], mode);
                return f;
            }
        }
    }
    /* v4.86: only a genuine "not found" goes into the negative cache. A transient failure
     * (out of memory, out of descriptors) used to blacklist main.obb for the rest of the run. */
    int zw_err = errno;
    if (!f && mode && is_asset_read_mode(mode) && zw_err == ENOENT) zw_neg_add(rp);   /* v4.58 */
    if (!f && g_main_loop_frame >= 60) {
        static unsigned zw_fail_diag;
        if (ZW_RUNTIME_LOG_OK() && zw_fail_diag++ < 12) zw_mem_report("fopen-fail", zw_err, rp);
    }
    /* v4.83: each log line costs ~15 ms on the memory card; footstep/sfx opens during play
     * caused visible stutter. Successful opens are only logged for the first 400 calls. */
    static unsigned zw_fopen_ok_logged, zw_fopen_fail_late;
    if (f) { g_zw_fopen_n++; zw_open_note(f, rp); }
    /* v5.02: the v4.98 16 KB setvbuf on OBB streams is gone (OBB reads use the block cache;
     * the bigger buffer only made every fseek+fread refill 16 KB). */
    if (f && g_main_loop_frame >= 900) {   /* v4.87: who opens files during play (leak hunt) */
        static unsigned n87;
        uintptr_t lr = (uintptr_t)__builtin_return_address(0);
        if (ZW_RUNTIME_LOG_OK() && n87++ < 10) {
            /* v4.88: crude backtrace - Thumb return addresses into the .so text found on the stack */
            char bt[300]; int o = 0; bt[0] = 0;
            uintptr_t base = g_zw_so_base_fwd();
            uint32_t *sp = (uint32_t *)__builtin_frame_address(0);
            for (int i = 0; i < 768 && o < 260; i++) {
                uint32_t v = sp[i];
                if ((v & 1) && v > base && v < base + 0xa00000)
                    o += snprintf(bt + o, sizeof(bt) - o, " %x", (unsigned)(v - base - 1));
            }
            log_printf("  v4.88 play-fopen f%d %p lr=so+0x%x bt:%s\n", g_main_loop_frame, (void *)f,
                       (unsigned)(lr - base), bt);
        }
    }
    if (should_log_fopen_result(rp, mode, f) &&
        (f ? (g_main_loop_frame < 60 && zw_fopen_ok_logged++ < 400) : (g_main_loop_frame < 60 || (ZW_PERIODIC_LOG && zw_fopen_fail_late++ < 40))))
        LOGF("fopen('%s','%s') -> %p\n", rp ? rp : "(null)", mode ? mode : "(null)", f);
    track_file(f, rp, mode);
    return f;
}

static size_t vita_fread_impl(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    VirtualFile *vf = virtual_file_from_FILE(stream);
    if (vf) {
        size_t bytes_req = size * nmemb;
        if (size != 0 && nmemb > ((size_t)-1 / size)) bytes_req = 0;
        size_t copied = 0;

        if (vf->ungot >= 0 && bytes_req > 0) {
            ((unsigned char *)ptr)[copied++] = (unsigned char)vf->ungot;
            vf->ungot = -1;
        }

        size_t take = bytes_req - copied;
        if (take > 0) {
            long got = vf_copy(vf, vf->pos, (unsigned char *)ptr + copied, (long)take);
            vf->pos += got;
            copied += (size_t)got;
        }
#ifdef VITA3K_BUILD
        if (vf->trace_ops < 24) {
            sceClibPrintf("[ZW] virtual fread(%s, size=%u nmemb=%u) -> %u pos=%ld/%ld\n",
                          vf->asset ? vf->asset->path : "(null)",
                          (unsigned)size, (unsigned)nmemb,
                          (unsigned)(size ? copied / size : 0),
                          vf->pos, vf->asset ? vf->asset->len : 0);
            vf->trace_ops++;
        }
#endif
        return size ? copied / size : 0;
    }

    size_t res = fread(ptr, size, nmemb, stream);
    const char *path = tracked_file_path(stream);
    if (path && should_trace_file_path(path)) {
        long pos = ftell(stream);
        LOGF("fread(%p,0x%X,0x%X,%p:%s) -> 0x%X pos=%ld\n",
             ptr, (unsigned)size, (unsigned)nmemb, stream, path, (unsigned)res, pos);
    }
    return res;
}

size_t vita_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    VirtualFile *vf = virtual_file_from_FILE(stream);
    if (vf && vf->sink) {
        size_t bytes = size * nmemb;
        vf->pos += (long)bytes;
        const char *path = tracked_file_path(stream);
        LOGF("fwrite(%p,0x%X,0x%X,%p:%s) -> 0x%X sink pos=%ld\n",
             ptr, (unsigned)size, (unsigned)nmemb, stream,
             path ? path : "(sink)", (unsigned)nmemb, vf->pos);
        return nmemb;
    }

    SceUInt64 _wt = sceKernelGetProcessTimeWide();   /* v5.10: write-path timing (purchase stall) */
    size_t res = fwrite(ptr, size, nmemb, stream);
    g_zw_wr_us += sceKernelGetProcessTimeWide() - _wt; g_zw_wr_n++;
    const char *path = tracked_file_path(stream);
    if (path && should_trace_file_path(path)) {
        long pos = ftell(stream);
        LOGF("fwrite(%p,0x%X,0x%X,%p:%s) -> 0x%X pos=%ld\n",
             ptr, (unsigned)size, (unsigned)nmemb, stream, path, (unsigned)res, pos);
    }
    return res;
}

int vita_fflush(FILE *stream) {
    VirtualFile *vf = virtual_file_from_FILE(stream);
    if (vf && vf->sink) {
        const char *path = tracked_file_path(stream);
        LOGF("fflush(%p:%s) -> 0 sink\n", stream, path ? path : "(sink)");
        return 0;
    }
    if (vf) return 0;   /* v5.02: read-only virtual file (glf Close does fflush before fclose) */

    const char *path = tracked_file_path(stream);
    int res = fflush(stream);
    if (path && should_trace_file_path(path)) {
        LOGF("fflush(%p:%s) -> %d\n", stream, path, res);
    }
    return res;
}

int vita_fputc(int c, FILE *stream) {
    VirtualFile *vf = virtual_file_from_FILE(stream);
    if (vf && vf->sink) {
        vf->pos++;
        const char *path = tracked_file_path(stream);
        LOGF("fputc(0x%X,%p:%s) -> 0x%X sink pos=%ld\n",
             (unsigned)(unsigned char)c, stream, path ? path : "(sink)",
             (unsigned)(unsigned char)c, vf->pos);
        return (unsigned char)c;
    }

    int res = fputc(c, stream);
    const char *path = tracked_file_path(stream);
    if (path && should_trace_file_path(path)) {
        long pos = ftell(stream);
        LOGF("fputc(0x%X,%p:%s) -> %d pos=%ld\n",
             (unsigned)(unsigned char)c, stream, path, res, pos);
    }
    return res;
}

int vita_putc(int c, FILE *stream) {
    return vita_fputc(c, stream);
}

int vita_fputs(const char *s, FILE *stream) {
    VirtualFile *vf = virtual_file_from_FILE(stream);
    if (vf && vf->sink) {
        size_t len = s ? strlen(s) : 0;
        vf->pos += (long)len;
        const char *path = tracked_file_path(stream);
        LOGF("fputs(%p,%p:%s) -> 0 sink pos=%ld\n",
             s, stream, path ? path : "(sink)", vf->pos);
        return 0;
    }

    int res = fputs(s, stream);
    const char *path = tracked_file_path(stream);
    if (path && should_trace_file_path(path)) {
        long pos = ftell(stream);
        LOGF("fputs(%p,%p:%s) -> %d pos=%ld\n", s, stream, path, res, pos);
    }
    return res;
}

int vita_vfprintf(FILE *stream, const char *fmt, va_list ap) {
    int res = vfprintf(stream, fmt, ap);
    const char *path = tracked_file_path(stream);
    if (path && should_trace_file_path(path)) {
        long pos = ftell(stream);
        LOGF("vfprintf(%p:%s,%p) -> %d pos=%ld\n", stream, path, fmt, res, pos);
    }
    return res;
}

int vita_fprintf(FILE *stream, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int res = vita_vfprintf(stream, fmt, ap);
    va_end(ap);
    return res;
}

static int vita_fseek_impl(FILE *stream, long offset, int whence) {
    VirtualFile *vf = virtual_file_from_FILE(stream);
    if (vf) {
        if (vf->sink) {
            long base = 0;
            if (whence == SEEK_SET) base = 0;
            else if (whence == SEEK_CUR) base = vf->pos;
            else if (whence == SEEK_END) base = vf->pos;
            else return -1;
            long next = base + offset;
            if (next < 0) next = 0;
            vf->pos = next;
            return 0;
        }

        long base = 0;
        if (whence == SEEK_SET) base = 0;
        else if (whence == SEEK_CUR) base = vf->pos;
        else if (whence == SEEK_END) base = vf_len(vf);
        else return -1;

        long next = base + offset;
        if (next < 0) return -1;
        if (next > vf_len(vf)) next = vf_len(vf);
        vf->pos = next;
        vf->ungot = -1;
#ifdef VITA3K_BUILD
        if (vf->trace_ops < 24) {
            sceClibPrintf("[ZW] virtual fseek(%s, %ld, %d) -> pos=%ld/%ld\n",
                          vf->asset ? vf->asset->path : "(null)",
                          offset, whence, vf->pos, vf->asset ? vf->asset->len : 0);
            vf->trace_ops++;
        }
#endif
        return 0;
    }

    int res = fseek(stream, offset, whence);
    const char *path = tracked_file_path(stream);
    if (path && should_trace_file_path(path)) {
        long pos = ftell(stream);
        LOGF("fseek(%p:%s,%ld,%d) -> %d pos=%ld\n",
             stream, path, offset, whence, res, pos);
    }
    return res;
}

long vita_ftell(FILE *stream) {
    VirtualFile *vf = virtual_file_from_FILE(stream);
    if (vf) {
        if (vf->sink) return vf->pos;
#ifdef VITA3K_BUILD
        if (vf->trace_ops < 24) {
            sceClibPrintf("[ZW] virtual ftell(%s) -> %ld/%ld\n",
                          vf->asset ? vf->asset->path : "(null)",
                          vf->pos, vf->asset ? vf->asset->len : 0);
            vf->trace_ops++;
        }
#endif
        return vf->pos;
    }

    long res = ftell(stream);
    const char *path = tracked_file_path(stream);
    if (path && should_trace_file_path(path)) {
        LOGF("ftell(%p:%s) -> %ld\n", stream, path, res);
    }
    return res;
}

int vita_fclose(FILE *stream) {
    VirtualFile *vf = virtual_file_from_FILE(stream);
    if (vf) {
        if (vf->sink) {
            const char *path = tracked_file_path(stream);
            LOGF("virtual sink fclose(%p:%s) pos=%ld\n",
                 stream, path ? path : "(sink)", vf->pos);
            untrack_file(stream);
            vf_free(vf);
            return 0;
        }

#ifdef VITA3K_BUILD
        sceClibPrintf("[ZW] virtual fclose(%s) pos=%ld/%ld\n",
                      vf->asset ? vf->asset->path : "(null)",
                      vf->pos, vf->asset ? vf->asset->len : 0);
#endif
        untrack_file(stream);
        if (!vf->obb) wav_release(vf->asset);   /* v5.03 */
        vf_free(vf);
        return 0;
    }

    const char *path = tracked_file_path(stream);
    if (path && should_trace_file_path(path)) {
        LOGF("fclose(%p:%s)\n", stream, path);
    }
    untrack_file(stream);
    g_zw_fclose_n++; zw_open_forget(stream);
    { SceUInt64 _ct = sceKernelGetProcessTimeWide(); int _r = fclose(stream); g_zw_cl_us += sceKernelGetProcessTimeWide() - _ct; g_zw_cl_n++; return _r; }
}

int vita_feof(FILE *stream) {
    VirtualFile *vf = virtual_file_from_FILE(stream);
    if (vf) return vf->pos >= vf_len(vf) && vf->ungot < 0;
    return feof(stream);
}

void vita_clearerr(FILE *stream) {
    VirtualFile *vf = virtual_file_from_FILE(stream);
    if (vf) {
        vf->ungot = -1;
        return;
    }
    clearerr(stream);
}

int vita_ferror(FILE *stream) {
    if (virtual_file_from_FILE(stream)) return 0;
    return ferror(stream);
}

int vita_fileno(FILE *stream) {
    if (virtual_file_from_FILE(stream)) {
        errno = EBADF;
        return -1;
    }
    return fileno(stream);
}

char *vita_fgets(char *s, int size, FILE *stream) {
    VirtualFile *vf = virtual_file_from_FILE(stream);
    if (!vf) return fgets(s, size, stream);
    if (!s || size <= 0) return NULL;

    int i = 0;
    while (i < size - 1) {
        int c;
        if (vf->ungot >= 0) {
            c = vf->ungot;
            vf->ungot = -1;
        } else if ((c = vf_byte(vf)) == EOF) {
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    s[i] = '\0';
    return s;
}

int vita_getc(FILE *stream) {
    VirtualFile *vf = virtual_file_from_FILE(stream);
    if (!vf) return getc(stream);
    if (vf->ungot >= 0) {
        int c = vf->ungot;
        vf->ungot = -1;
        return c;
    }
    return vf_byte(vf);
}

int vita_fgetc(FILE *stream) {
    return vita_getc(stream);
}

void vita_rewind(FILE *stream) {
    VirtualFile *vf = virtual_file_from_FILE(stream);
    if (vf) {
        vf->pos = 0;
        vf->ungot = -1;
        return;
    }
    rewind(stream);
}

int vita_ungetc(int c, FILE *stream) {
    VirtualFile *vf = virtual_file_from_FILE(stream);
    if (!vf) return ungetc(c, stream);
    if (c == EOF || vf->ungot >= 0) return EOF;
    vf->ungot = (unsigned char)c;
    return vf->ungot;
}

wint_t vita_getwc(FILE *stream) {
    int c = vita_getc(stream);
    return c == EOF ? WEOF : (wint_t)(unsigned char)c;
}

wint_t vita_ungetwc(wint_t wc, FILE *stream) {
    if (wc == WEOF) return WEOF;
    int res = vita_ungetc((int)(unsigned char)wc, stream);
    return res == EOF ? WEOF : (wint_t)res;
}

#define BIONIC_O_WRONLY     01
#define BIONIC_O_RDWR       02
#define BIONIC_O_CREAT      0100
#define BIONIC_O_EXCL       0200
#define BIONIC_O_TRUNC      01000
#define BIONIC_O_APPEND     02000
#define BIONIC_O_NONBLOCK   04000
#define BIONIC_O_DIRECTORY  00200000
#define BIONIC__O_TMPFILE   020000000
#define BIONIC_O_TMPFILE    (BIONIC__O_TMPFILE | BIONIC_O_DIRECTORY)

typedef struct __attribute__((__packed__)) {
    unsigned long long st_dev;
    unsigned char __pad0[4];
    unsigned long __st_ino;
    unsigned int st_mode;
    nlink_t st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    unsigned long long st_rdev;
    unsigned char __pad3[4];
    long long st_size;
    unsigned long st_blksize;
    unsigned long long st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
    unsigned long long st_ino;
} AndroidStat64;

typedef struct __attribute__((__packed__)) {
    int16_t d_ino;
    int64_t d_off;
    uint64_t d_reclen;
    unsigned char d_type;
    char d_name[256];
} AndroidDirent64;

static int oflags_bionic_to_newlib(int flags) {
    int out = 0;
    if (flags & BIONIC_O_RDWR) {
        out |= O_RDWR;
    } else if (flags & BIONIC_O_WRONLY) {
        out |= O_WRONLY;
    } else {
        out |= O_RDONLY;
    }
    if (flags & BIONIC_O_NONBLOCK) out |= O_NONBLOCK;
    if (flags & BIONIC_O_APPEND) out |= O_APPEND;
    if ((flags & BIONIC_O_CREAT) || (flags & BIONIC_O_TMPFILE)) out |= O_CREAT;
    if (flags & BIONIC_O_TRUNC) out |= O_TRUNC;
    if (flags & BIONIC_O_EXCL) out |= O_EXCL;
    return out;
}

static void stat_newlib_to_bionic(const struct stat *src, AndroidStat64 *dst) {
    memset(dst, 0, sizeof(*dst));
    dst->st_dev = src->st_dev;
    dst->__st_ino = src->st_ino;
    dst->st_ino = src->st_ino;
    dst->st_mode = src->st_mode;
    dst->st_nlink = src->st_nlink;
    dst->st_uid = src->st_uid;
    dst->st_gid = src->st_gid;
    dst->st_rdev = src->st_rdev;
    dst->st_size = src->st_size;
    dst->st_blksize = src->st_blksize;
    dst->st_blocks = src->st_blocks;
    dst->st_atim.tv_sec = src->st_atime;
    dst->st_mtim.tv_sec = src->st_mtime;
    dst->st_ctim.tv_sec = src->st_ctime;
}

int vita_open(const char *path, int flags, ...) {
    int mode = 0777;
    if ((flags & BIONIC_O_CREAT) || (flags & BIONIC_O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    const char *rp = redirect_path(path);
    int newlib_flags = oflags_bionic_to_newlib(flags);
    int fd = open(rp, newlib_flags, mode);
    int err = errno;
    if (fd >= 0) { g_zw_open_n++; if (++g_zw_fd_now > g_zw_fd_max) g_zw_fd_max = g_zw_fd_now; }
    if (g_main_loop_frame < 60 || fd < 0) {   /* v4.86: no per-call logging during play */
        static unsigned late;
        if (g_main_loop_frame < 60 || late++ < 20)
            LOGF("open('%s',bionic=0x%X,newlib=0x%X) -> 0x%08X errno=%d\n",
                 rp ? rp : "(null)", flags, newlib_flags, fd, fd < 0 ? err : 0);
    }
    errno = err;
    return fd;
}

int vita_close(int fd) {   /* v4.86: descriptor accounting */
    int r = close(fd);
    if (r == 0) { g_zw_close_n++; g_zw_fd_now--; }
    return r;
}

int vita_stat(const char *path, AndroidStat64 *out) {
    const char *rp = redirect_path(path);
    if (zw_neg_lookup(rp)) { errno = ENOENT; return -1; }   /* v4.58 */
    struct stat st;
    int res = stat(rp, &st);
    if (res != 0) zw_neg_add(rp);
    const char *slash = rp ? strrchr(rp, '/') : NULL;
    const char *fname = slash ? slash + 1 : rp;
    if (res == 0 && fname && is_save_state_name(fname)) {
        FILE *f = fopen(rp, "rb");
        if (f && save_file_looks_blank(f)) {
            fclose(f);
            errno = ENOENT;
            LOGF("stat('%s') -> blank save treated as missing\n",
                 rp ? rp : "(null)");
            return -1;
        }
        if (f) fclose(f);
    }
    if (res != 0 && fname && has_ext(fname, ".ttf") &&
        strcmp(fname, "KGFallForYou.ttf") != 0) {
        const char *fallback = "ux0:/data/zombiewood/KGFallForYou.ttf";
        res = stat(fallback, &st);
        LOGF("stat('%s') -> font fallback %s -> %d\n",
             rp ? rp : "(null)", fallback, res);
        if (res == 0 && out) stat_newlib_to_bionic(&st, out);
        return res;
    }
    if (res == 0 && out) stat_newlib_to_bionic(&st, out);
    if (ZW_RUNTIME_LOG_OK())
        LOGF("stat('%s') -> %d%s\n",
             rp ? rp : "(null)", res, res == 0 ? " (bionic)" : "");
    return res;
}

int vita_fstat(int fd, AndroidStat64 *out) {
    struct stat st;
    int res = fstat(fd, &st);
    if (res == 0 && out) stat_newlib_to_bionic(&st, out);
    LOGF("fstat(%d) -> %d%s\n", fd, res, res == 0 ? " (bionic)" : "");
    return res;
}

DIR *vita_opendir(const char *path) {
    const char *rp = redirect_path(path);
    DIR *res = opendir(rp);
    LOGF("opendir('%s') -> %p\n", rp ? rp : "(null)", (void *)res);
    return res;
}

AndroidDirent64 *vita_readdir(DIR *dir) {
    static AndroidDirent64 out;
    struct dirent *ent = readdir(dir);
    if (!ent) {
        LOGF("readdir(%p) -> NULL\n", (void *)dir);
        return NULL;
    }

    memset(&out, 0, sizeof(out));
#ifdef _DIRENT_HAVE_D_STAT
    out.d_ino = (int16_t)ent->d_stat.st_ino;
    out.d_type = S_ISDIR(ent->d_stat.st_mode) ? 4 : 8;
#else
    out.d_ino = 0;
    out.d_type = 8;
#endif
    out.d_off = 0;
    out.d_reclen = 0;
    strncpy(out.d_name, ent->d_name, sizeof(out.d_name) - 1);
    LOGF("readdir(%p) -> '%s' type=%u (bionic)\n",
         (void *)dir, out.d_name, (unsigned)out.d_type);
    return &out;
}

int vita_remove(const char *path) {
    const char *rp = redirect_path(path);
    int res = remove(rp);
    LOGF("remove('%s') -> %d\n", rp ? rp : "(null)", res);
    return res;
}

int vita_rename(const char *oldpath, const char *newpath) {
    char old_buf[512];
    char new_buf[512];

    const char *old_rp = redirect_path(oldpath);
    snprintf(old_buf, sizeof(old_buf), "%s", old_rp ? old_rp : "");

    const char *new_rp = redirect_path(newpath);
    snprintf(new_buf, sizeof(new_buf), "%s", new_rp ? new_rp : "");

    int res = rename(old_buf, new_buf);
    LOGF("rename('%s','%s') -> %d\n", old_buf, new_buf, res);
    return res;
}

int vita_unlink(const char *path) {
    const char *rp = redirect_path(path);
    int res = unlink(rp);
    LOGF("unlink('%s') -> %d\n", rp ? rp : "(null)", res);
    return res;
}

int vita_mkdir(const char *path, mode_t mode) {
    const char *rp = redirect_path(path);
    int res = mkdir(rp, mode);
    LOGF("mkdir('%s') -> %d\n", rp ? rp : "(null)", res);
    return res;
}

char *vita_setlocale(int category, const char *locale) {
    (void)category;
    LOGF("setlocale(%d,'%s') -> C\n", category, locale ? locale : "(null)");
    return "C";
}

/* ─────────────────────────────────────────────
   Audio — OpenSL ES redirect to SceAudio
   The game uses a Vox audio engine (VoxSoundManager)
   which calls slCreateEngine. We intercept that entry
   point and open the Vita audio port instead.
   ───────────────────────────────────────────── */
/* v4.99: the OpenSL ES emulation (engine, output mix, PCM buffer-queue player on
 * sceAudioOut) lives in audio_sl.c. */

/* ─────────────────────────────────────────────
   Fake JNI environment
   The .so's Java_com_* functions receive (JNIEnv*, jobject, ...).
   Our stub implementations never dereference env, so we pass a
   simple table of no-op function pointers.  Each slot is filled
   with a generic stub that returns NULL/0 for any type.
   ───────────────────────────────────────────── */
static void *jni_no_op() { return NULL; }

#define JNI_METHOD_ID_SLOTS 256
#define JNI_METHOD_ID_LEN 64
static char g_jni_method_ids[JNI_METHOD_ID_SLOTS][JNI_METHOD_ID_LEN];
static unsigned g_jni_method_id_next = 0;

static const char *jni_store_id_string(const char *name, const char *fallback) {
    const char *src = name ? name : (fallback ? fallback : "");
    char *dst = g_jni_method_ids[g_jni_method_id_next++ % JNI_METHOD_ID_SLOTS];
    size_t i = 0;
    for (; i + 1 < JNI_METHOD_ID_LEN && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
    return dst;
}

static const char *jni_id_cstr(const void *id) {
    return id ? (const char *)id : "";
}

static jint jni_GetVersion(JNIEnv *env) {
    (void)env;
    return JNI_VERSION_1_4;
}

static jclass jni_DefineClass(JNIEnv *env, const char *name, jobject loader,
                              const jbyte *buf, jsize len) {
    (void)env;
    (void)loader;
    (void)buf;
    JNI_TRACEF("JNI DefineClass('%s', len=%d)\n", name ? name : "(null)", (int)len);
    return (jclass)(name ? name : "defined-class");
}

static jclass jni_FindClass(JNIEnv *env, const char *name) {
    (void)env;
    JNI_TRACEF("JNI FindClass('%s')\n", name ? name : "(null)");
    return (jclass)name;
}

static jobject jni_NewGlobalRef(JNIEnv *env, jobject obj) {
    (void)env;
    JNI_TRACEF("JNI NewGlobalRef(%p)\n", obj);
    return obj;
}

static void jni_DeleteGlobalRef(JNIEnv *env, jobject obj) {
    (void)env;
    (void)obj;
}

static void jni_DeleteLocalRef(JNIEnv *env, jobject obj) {
    (void)env;
    (void)obj;
}

static jmethodID jni_GetStaticMethodID(JNIEnv *env, jclass clazz,
                                       const char *name, const char *sig) {
    (void)env;
    (void)clazz;
    const char *stored = jni_store_id_string(name, sig);
    JNI_TRACEF("JNI GetStaticMethodID('%s')\n", stored);
    return (jmethodID)stored;
}

static jmethodID jni_GetMethodID(JNIEnv *env, jclass clazz,
                                 const char *name, const char *sig) {
    (void)env;
    (void)clazz;
    const char *stored = jni_store_id_string(name, sig);
    JNI_TRACEF("JNI GetMethodID('%s')\n", stored);
    return (jmethodID)stored;
}

static jfieldID jni_GetStaticFieldID(JNIEnv *env, jclass clazz,
                                     const char *name, const char *sig) {
    (void)env;
    (void)clazz;
    const char *stored = jni_store_id_string(name, sig);
    JNI_TRACEF("JNI GetStaticFieldID('%s')\n", stored);
    return (jfieldID)stored;
}

static jstring jni_NewStringUTF(JNIEnv *env, const char *bytes) {
    (void)env;
    JNI_TRACEF("JNI NewStringUTF('%s')\n", bytes ? bytes : "(null)");
    return (jstring)bytes;
}

static jsize jni_GetStringUTFLength(JNIEnv *env, jstring string) {
    (void)env;
    return string ? (jsize)strlen((const char *)string) : 0;
}

static const char *jni_GetStringUTFChars(JNIEnv *env, jstring string, jboolean *isCopy) {
    (void)env;
    if (isCopy) *isCopy = JNI_FALSE;
    JNI_TRACEF("JNI GetStringUTFChars(%p) -> '%s'\n",
         string, string ? (const char *)string : "(null)");
    return string ? (const char *)string : "";
}

static void jni_ReleaseStringUTFChars(JNIEnv *env, jstring string, const char *utf) {
    (void)env;
    (void)string;
    (void)utf;
}

#define FAKE_BYTE_ARRAY_MAGIC 0x5A574241u

typedef struct FakeByteArray {
    uint32_t magic;
    jsize length;
    jbyte data[];
} FakeByteArray;

static FakeByteArray *fake_byte_array_from_jarray(jarray array) {
    FakeByteArray *ba = (FakeByteArray *)array;
    if (!ba || ba->magic != FAKE_BYTE_ARRAY_MAGIC || ba->length < 0) {
        return NULL;
    }
    return ba;
}

static const char *jni_string_cstr(jstring string) {
    return string ? (const char *)string : "";
}

#define RESOURCE_CACHE_SLOTS 8
typedef struct ResourceCacheEntry {
    char path[384];
    unsigned char *data;
    long len;
} ResourceCacheEntry;

static ResourceCacheEntry g_resource_cache[RESOURCE_CACHE_SLOTS];
static unsigned g_resource_cache_next = 0;

static int resolve_resource_path(const char *name, char *out, size_t out_sz) {
    if (!name || !name[0] || !out || out_sz == 0) return 0;

    while (name[0] == '.' && name[1] == '/') name += 2;

    if (strstr(name, "ux0:") == name || strstr(name, "app0:") == name ||
        name[0] == '/' || strstr(name, "/sdcard") || strstr(name, "/data/data") ||
        strstr(name, "/mnt/sdcard") || strstr(name, "gameloft") ||
        strstr(name, "zombiewood")) {
        const char *rp = redirect_path(name);
        snprintf(out, out_sz, "%s", rp ? rp : name);
    } else {
        const char *slash = strrchr(name, '/');
        const char *fname = slash ? slash + 1 : name;
        snprintf(out, out_sz, "ux0:/data/zombiewood/%s", fname);
    }

    return out[0] != '\0';
}

static ResourceCacheEntry *resource_cache_find(const char *path) {
    if (!path) return NULL;
    for (unsigned i = 0; i < RESOURCE_CACHE_SLOTS; i++) {
        if (g_resource_cache[i].data && strcmp(g_resource_cache[i].path, path) == 0) {
            return &g_resource_cache[i];
        }
    }
    return NULL;
}

static ResourceCacheEntry *resource_cache_load(const char *path) {
    ResourceCacheEntry *hit = resource_cache_find(path);
    if (hit) return hit;

    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return NULL;

    SceOff len64 = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (len64 <= 0 || len64 > 4 * 1024 * 1024) {
        sceIoClose(fd);
        return NULL;
    }
    sceIoLseek(fd, 0, SCE_SEEK_SET);

    unsigned char *data = (unsigned char *)calloc(1, (size_t)len64 + 1);
    if (!data) {
        sceIoClose(fd);
        return NULL;
    }

    int got = sceIoRead(fd, data, (SceSize)len64);
    sceIoClose(fd);
    if (got < 0) got = 0;

    ResourceCacheEntry *slot = &g_resource_cache[g_resource_cache_next++ % RESOURCE_CACHE_SLOTS];
    free(slot->data);
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    slot->data = data;
    slot->len = got;
    return slot;
}

static long resource_file_length(jstring string) {
    const char *name = jni_string_cstr(string);
    char path[384];
    if (!resolve_resource_path(name, path, sizeof(path))) {
        LOGF("ResLoader.getLength('%s') -> 0 (bad path)\n", name ? name : "(null)");
        return 0;
    }

    ResourceCacheEntry *cached = resource_cache_load(path);
    if (!cached) {
        LOGF("ResLoader.getLength('%s') -> 0 (missing %s)\n", name, path);
        return 0;
    }

    long len = cached->len;
    LOGF("ResLoader.getLength('%s') -> %ld (%s)\n", name, len, path);
    return len;
}

static jbyteArray jni_NewByteArray(JNIEnv *env, jsize length) {
    (void)env;
    if (length < 0) length = 0;
    FakeByteArray *ba = (FakeByteArray *)calloc(1, sizeof(FakeByteArray) + (size_t)length + 1);
    if (!ba) {
        JNI_TRACEF("JNI NewByteArray(%d) -> NULL\n", (int)length);
        return NULL;
    }
    ba->magic = FAKE_BYTE_ARRAY_MAGIC;
    ba->length = length;
    JNI_TRACEF("JNI NewByteArray(%d) -> %p\n", (int)length, ba);
    return (jbyteArray)ba;
}

static jsize jni_GetArrayLength(JNIEnv *env, jarray array) {
    (void)env;
    FakeByteArray *ba = fake_byte_array_from_jarray(array);
    jsize len = ba ? ba->length : 0;
    JNI_TRACEF("JNI GetArrayLength(%p) -> %d\n", array, (int)len);
    return len;
}

static jbyte *jni_GetByteArrayElements(JNIEnv *env, jbyteArray array, jboolean *isCopy) {
    (void)env;
    if (isCopy) *isCopy = JNI_FALSE;
    FakeByteArray *ba = fake_byte_array_from_jarray((jarray)array);
    JNI_TRACEF("JNI GetByteArrayElements(%p) -> %p len=%d\n",
         array, ba ? ba->data : NULL, ba ? (int)ba->length : 0);
    return ba ? ba->data : NULL;
}

static void jni_ReleaseByteArrayElements(JNIEnv *env, jbyteArray array,
                                         jbyte *elems, jint mode) {
    (void)env;
    (void)array;
    (void)elems;
    (void)mode;
}

static void jni_GetByteArrayRegion(JNIEnv *env, jbyteArray array,
                                   jsize start, jsize len, jbyte *buf) {
    (void)env;
    FakeByteArray *ba = fake_byte_array_from_jarray((jarray)array);
    if (!ba || !buf || start < 0 || len < 0 || start > ba->length) return;
    if (start + len > ba->length) len = ba->length - start;
    memcpy(buf, ba->data + start, (size_t)len);
}

static void jni_SetByteArrayRegion(JNIEnv *env, jbyteArray array,
                                   jsize start, jsize len, const jbyte *buf) {
    (void)env;
    FakeByteArray *ba = fake_byte_array_from_jarray((jarray)array);
    if (!ba || !buf || start < 0 || len < 0 || start > ba->length) return;
    if (start + len > ba->length) len = ba->length - start;
    memcpy(ba->data + start, buf, (size_t)len);
}

static jbyteArray resource_file_to_byte_array(jstring string) {
    const char *name = jni_string_cstr(string);
    char path[384];
    if (!resolve_resource_path(name, path, sizeof(path))) {
        LOGF("ResLoader.byteArray('%s') -> NULL (bad path)\n", name ? name : "(null)");
        return NULL;
    }

    ResourceCacheEntry *cached = resource_cache_find(path);
    if (!cached) cached = resource_cache_load(path);
    if (!cached) {
        LOGF("ResLoader.byteArray('%s') -> NULL (missing %s)\n", name, path);
        return NULL;
    }

    long len = cached->len;
    jbyteArray arr = jni_NewByteArray(NULL, (jsize)len);
    FakeByteArray *ba = fake_byte_array_from_jarray((jarray)arr);
    int got = 0;
    if (ba && len > 0) {
        memcpy(ba->data, cached->data, (size_t)len);
        got = (int)len;
    }

    LOGF("ResLoader.byteArray('%s') -> %p len=%ld read=%u (%s)\n",
         name, arr, len, (unsigned)got, path);
    return arr;
}

static int hook_AResLoader_GetLength(const char *name) {
    long len = resource_file_length((jstring)name);
    if (len > 0x7fffffffL) len = 0x7fffffffL;
    LOGF("AResLoader_GetLength('%s') -> %ld\n",
         name ? name : "(null)", len);
    return (int)len;
}

static jbyteArray hook_AResLoader_GetData(const char *name) {
    jbyteArray arr = resource_file_to_byte_array((jstring)name);
    FakeByteArray *ba = fake_byte_array_from_jarray((jarray)arr);
    LOGF("AResLoader_GetData('%s') -> %p len=%d\n",
         name ? name : "(null)", arr, ba ? (int)ba->length : 0);
    return arr;
}

static jthrowable jni_ExceptionOccurred(JNIEnv *env) {
    (void)env;
    return NULL;
}

static void jni_ExceptionDescribe(JNIEnv *env) {
    (void)env;
}

static void jni_ExceptionClear(JNIEnv *env) {
    (void)env;
}

static jboolean jni_ExceptionCheck(JNIEnv *env) {
    (void)env;
    return JNI_FALSE;
}

static jobject jni_GetStaticObjectField(JNIEnv *env, jclass clazz, jfieldID fieldID) {
    (void)env;
    JNI_TRACEF("JNI GetStaticObjectField(clazz=%p, field='%s')\n",
         clazz, fieldID ? (const char *)fieldID : "(null)");
    return fieldID ? (jobject)fieldID : (jobject)clazz;
}

static jint jni_GetStaticIntField(JNIEnv *env, jclass clazz, jfieldID fieldID) {
    (void)env;
    (void)clazz;
    const char *field = fieldID ? (const char *)fieldID : "";
    if (strstr(field, "Width") || strstr(field, "width")) return 960;
    if (strstr(field, "Height") || strstr(field, "height")) return 544;
    if (strstr(field, "DPI") || strstr(field, "Density") ||
        strstr(field, "density")) return 160;
    if (strstr(field, "Orientation") || strstr(field, "orientation")) return 0;
    if (strstr(field, "SDK") || strstr(field, "Version") ||
        strstr(field, "version")) return 19;
    if (strstr(field, "Language")) return 0;
    if (strstr(field, "TextureFormat")) return 0;
    if (strstr(field, "CPUCores")) return 4;
    return 0;
}

static void jni_SetStaticObjectField(JNIEnv *env, jclass clazz,
                                     jfieldID fieldID, jobject value) {
    (void)env;
    (void)clazz;
    (void)fieldID;
    (void)value;
}

static void jni_SetStaticIntField(JNIEnv *env, jclass clazz,
                                  jfieldID fieldID, jint value) {
    (void)env;
    (void)clazz;
    (void)fieldID;
    (void)value;
}

static void jni_CallStaticVoidMethod(JNIEnv *env, jclass clazz,
                                     jmethodID methodID, ...) {
    (void)env;
    (void)clazz;
    { static int zw_n; if (zw_n < 20) { zw_n++; JNI_TRACEF("JNI CallStaticVoidMethod('%s')\n", jni_id_cstr(methodID)); } }
}

static void jni_CallStaticVoidMethodV(JNIEnv *env, jclass clazz,
                                      jmethodID methodID, va_list args) {
    (void)env;
    (void)clazz;
    (void)args;
    JNI_TRACEF("JNI CallStaticVoidMethodV('%s')\n", jni_id_cstr(methodID));
}

static void jni_CallStaticVoidMethodA(JNIEnv *env, jclass clazz,
                                      jmethodID methodID, jvalue *args) {
    (void)env;
    (void)clazz;
    (void)args;
    JNI_TRACEF("JNI CallStaticVoidMethodA('%s')\n", jni_id_cstr(methodID));
}

static jboolean jni_CallStaticBooleanMethod(JNIEnv *env, jclass clazz,
                                            jmethodID methodID, ...) {
    (void)env;
    (void)clazz;
    const char *method = jni_id_cstr(methodID);
    if (strstr(method, "IsLoggedIn") ||
        strstr(method, "hasPermission") ||
        strstr(method, "Permission") ||
        strstr(method, "GLLive") ||
        strstr(method, "Facebook") ||
        strstr(method, "Wifi") ||
        strstr(method, "Internet") ||
        strstr(method, "Network")) {
        return JNI_FALSE;
    }
    return JNI_FALSE;
}

static jboolean jni_CallStaticBooleanMethodV(JNIEnv *env, jclass clazz,
                                             jmethodID methodID, va_list args) {
    (void)args;
    return jni_CallStaticBooleanMethod(env, clazz, methodID);
}

static jboolean jni_CallStaticBooleanMethodA(JNIEnv *env, jclass clazz,
                                             jmethodID methodID, jvalue *args) {
    (void)args;
    return jni_CallStaticBooleanMethod(env, clazz, methodID);
}

static jint jni_CallStaticIntMethod(JNIEnv *env, jclass clazz,
                                    jmethodID methodID, ...) {
    (void)env;
    (void)clazz;
    const char *method = jni_id_cstr(methodID);
    JNI_TRACEF("JNI CallStaticIntMethod('%s')\n", method);
    if (!method[0]) {
        static unsigned int empty_count = 0;
        if (empty_count < 64 || (empty_count % 120) == 0) {
            LOGF("JNI CallStaticIntMethod empty id=%p caller=%p count=%u\n",
                 methodID, __builtin_return_address(0), empty_count);
        }
        empty_count++;
    }
    if (strstr(method, "getLength") || strstr(method, "GetLength")) {
        va_list ap;
        va_start(ap, methodID);
        jstring path = va_arg(ap, jstring);
        va_end(ap);
        long len = resource_file_length(path);
        return len > 0x7fffffffL ? 0x7fffffff : (jint)len;
    }
    if (strstr(method, "Language")) return 0;
    if (strstr(method, "TextureFormat")) return 0;
    if (strstr(method, "Manufacture")) return 4;
    if (strstr(method, "CPUCores")) return 4;
    if (strstr(method, "getNativeOutputSampleRate")) return 44100;
    if (strstr(method, "getMinBufferSize")) return 4096;
    if (strstr(method, "Wifi") || strstr(method, "Jail") ||
        strstr(method, "Xperia") || strstr(method, "ClosedActivity")) {
        return 0;
    }
    return 0;
}

static jint jni_CallStaticIntMethodV(JNIEnv *env, jclass clazz,
                                     jmethodID methodID, va_list args) {
    (void)env;
    (void)clazz;
    const char *method = jni_id_cstr(methodID);
    JNI_TRACEF("JNI CallStaticIntMethodV('%s')\n", method);
    if (!method[0]) {
        static unsigned int empty_count = 0;
        if (empty_count < 96 || (empty_count % 120) == 0) {
            uintptr_t parent_lr = 0;
            uintptr_t *wrapper_args = NULL;
            memcpy(&wrapper_args, &args, sizeof(wrapper_args));
            if (wrapper_args) {
                parent_lr = wrapper_args[-2];
            }
            LOGF("JNI CallStaticIntMethodV empty id=%p caller=%p parent=%p count=%u\n",
                 methodID, __builtin_return_address(0),
                 (void *)parent_lr, empty_count);
        }
        empty_count++;
    }
    if (strstr(method, "getLength") || strstr(method, "GetLength")) {
        va_list copy;
        va_copy(copy, args);
        jstring path = va_arg(copy, jstring);
        va_end(copy);
        long len = resource_file_length(path);
        return len > 0x7fffffffL ? 0x7fffffff : (jint)len;
    }
    if (strstr(method, "Language")) return 0;
    if (strstr(method, "TextureFormat")) return 0;
    if (strstr(method, "Manufacture")) return 4;
    if (strstr(method, "CPUCores")) return 4;
    if (strstr(method, "getNativeOutputSampleRate")) return 44100;
    if (strstr(method, "getMinBufferSize")) return 4096;
    if (strstr(method, "Wifi") || strstr(method, "Jail") ||
        strstr(method, "Xperia") || strstr(method, "ClosedActivity")) {
        return 0;
    }
    return 0;
}

static jint jni_CallStaticIntMethodA(JNIEnv *env, jclass clazz,
                                     jmethodID methodID, jvalue *args) {
    (void)env;
    (void)clazz;
    const char *method = jni_id_cstr(methodID);
    JNI_TRACEF("JNI CallStaticIntMethodA('%s')\n", method);
    if (strstr(method, "getLength") || strstr(method, "GetLength")) {
        long len = resource_file_length(args ? (jstring)args[0].l : NULL);
        return len > 0x7fffffffL ? 0x7fffffff : (jint)len;
    }
    if (strstr(method, "Language")) return 0;
    if (strstr(method, "TextureFormat")) return 0;
    if (strstr(method, "Manufacture")) return 4;
    if (strstr(method, "CPUCores")) return 4;
    if (strstr(method, "getNativeOutputSampleRate")) return 44100;
    if (strstr(method, "getMinBufferSize")) return 4096;
    if (strstr(method, "Wifi") || strstr(method, "Jail") ||
        strstr(method, "Xperia") || strstr(method, "ClosedActivity")) {
        return 0;
    }
    return 0;
}

static jlong jni_CallStaticLongMethod(JNIEnv *env, jclass clazz,
                                      jmethodID methodID, ...) {
    (void)env;
    (void)clazz;
    const char *method = jni_id_cstr(methodID);
    if (strstr(method, "UpTime") || strstr(method, "CurrentTime")) {
        return (jlong)sceKernelGetProcessTimeWide() / 1000;
    }
    return 0;
}

static jlong jni_CallStaticLongMethodV(JNIEnv *env, jclass clazz,
                                       jmethodID methodID, va_list args) {
    (void)args;
    return jni_CallStaticLongMethod(env, clazz, methodID);
}

static jlong jni_CallStaticLongMethodA(JNIEnv *env, jclass clazz,
                                       jmethodID methodID, jvalue *args) {
    (void)args;
    return jni_CallStaticLongMethod(env, clazz, methodID);
}

static jfloat jni_CallStaticFloatMethod(JNIEnv *env, jclass clazz,
                                        jmethodID methodID, ...) {
    (void)env;
    (void)clazz;
    const char *method = jni_id_cstr(methodID);
    if (strstr(method, "CPUFreq")) return 444.0f;
    return 0.0f;
}

static jfloat jni_CallStaticFloatMethodV(JNIEnv *env, jclass clazz,
                                         jmethodID methodID, va_list args) {
    (void)args;
    return jni_CallStaticFloatMethod(env, clazz, methodID);
}

static jfloat jni_CallStaticFloatMethodA(JNIEnv *env, jclass clazz,
                                         jmethodID methodID, jvalue *args) {
    (void)args;
    return jni_CallStaticFloatMethod(env, clazz, methodID);
}

static jobject jni_object_result_for_method(const char *method) {
    if (!method) method = "";
    if (strstr(method, "Save") || strstr(method, "save") ||
        strstr(method, "SD") || strstr(method, "Prefs") ||
        strstr(method, "Data") || strstr(method, "Preference")) {
        return (jobject)"ux0:/data/zombiewood/";
    }
    if (strstr(method, "Package") || strstr(method, "Context")) {
        return (jobject)"com.gameloft.android.ANMP.GloftZOHM";
    }
    if (strstr(method, "Identifier") || strstr(method, "IMEI")) {
        return (jobject)"000000000000000";
    }
    if (strstr(method, "Mac") || strstr(method, "MAC")) {
        return (jobject)"00:00:00:00:00:00";
    }
    if (strstr(method, "Carrier")) {
        return (jobject)"";
    }
    if (strstr(method, "Language") || strstr(method, "Locale")) {
        return (jobject)"en";
    }
    if (strstr(method, "Country") || strstr(method, "Locale")) {
        return (jobject)"US";
    }
    if (strstr(method, "Firmware")) {
        return (jobject)"3.74";
    }
    if (strstr(method, "Manufacturer") || strstr(method, "Manufacture")) {
        return (jobject)"Sony";
    }
    if (strstr(method, "Model") || strstr(method, "Device")) {
        return (jobject)"PS Vita";
    }
    if (strstr(method, "CPUName")) {
        return (jobject)"ARM Cortex-A9";
    }
    if (strstr(method, "GPUName")) {
        return (jobject)"PowerVR SGX543MP4+";
    }
    if (strstr(method, "ObbPath")) {
        return (jobject)"ux0:/data/zombiewood/";
    }
    if (strstr(method, "MainObbName")) {
        return (jobject)"main.1501.com.gameloft.android.ANMP.GloftZOHM.obb";
    }
    if (strstr(method, "PatchObbName")) {
        return (jobject)"patch.15325.com.gameloft.android.ANMP.GloftZOHM.obb";
    }
    if (strstr(method, "ExtraObbName") || strstr(method, "ObbName")) {
        return (jobject)"";
    }
    if (strstr(method, "UserAgent")) {
        return (jobject)"gt-i9100";
    }
    return (jobject)"";
}

static jobject jni_CallStaticObjectMethod(JNIEnv *env, jclass clazz,
                                          jmethodID methodID, ...) {
    (void)env;
    (void)clazz;
    const char *method = jni_id_cstr(methodID);
    JNI_TRACEF("JNI CallStaticObjectMethod('%s')\n", method);
    if (strstr(method, "getResource") || strstr(method, "getBuffer") || strstr(method, "getBytes") ||
        strstr(method, "getData") || strstr(method, "loadResource")) {
        va_list ap;
        va_start(ap, methodID);
        jstring path = va_arg(ap, jstring);
        va_end(ap);
        return (jobject)resource_file_to_byte_array(path);
    }
    return jni_object_result_for_method(method);
}

static jobject jni_CallStaticObjectMethodV(JNIEnv *env, jclass clazz,
                                           jmethodID methodID, va_list args) {
    (void)env;
    (void)clazz;
    const char *method = jni_id_cstr(methodID);
    JNI_TRACEF("JNI CallStaticObjectMethodV('%s')\n", method);
    if (strstr(method, "getResource") || strstr(method, "getBuffer") || strstr(method, "getBytes") ||
        strstr(method, "getData") || strstr(method, "loadResource")) {
        va_list copy;
        va_copy(copy, args);
        jstring path = va_arg(copy, jstring);
        va_end(copy);
        return (jobject)resource_file_to_byte_array(path);
    }
    return jni_object_result_for_method(method);
}

static jobject jni_CallStaticObjectMethodA(JNIEnv *env, jclass clazz,
                                           jmethodID methodID, jvalue *args) {
    (void)env;
    (void)clazz;
    const char *method = jni_id_cstr(methodID);
    JNI_TRACEF("JNI CallStaticObjectMethodA('%s')\n", method);
    if (strstr(method, "getResource") || strstr(method, "getBuffer") || strstr(method, "getBytes") ||
        strstr(method, "getData") || strstr(method, "loadResource")) {
        return (jobject)resource_file_to_byte_array(args ? (jstring)args[0].l : NULL);
    }
    return jni_object_result_for_method(method);
}

static jobject jni_CallObjectMethod(JNIEnv *env, jobject obj,
                                    jmethodID methodID, ...) {
    (void)env;
    (void)obj;
    const char *method = jni_id_cstr(methodID);
    JNI_TRACEF("JNI CallObjectMethod('%s')\n", method);
    if (strstr(method, "Save") || strstr(method, "save") ||
        strstr(method, "SD") || strstr(method, "Prefs") ||
        strstr(method, "Data")) {
        return (jobject)"ux0:/data/zombiewood/";
    }
    if (strstr(method, "Package") || strstr(method, "Context")) {
        return (jobject)"com.gameloft.android.ANMP.GloftZOHM";
    }
    if (strstr(method, "Identifier") || strstr(method, "IMEI")) {
        return (jobject)"000000000000000";
    }
    if (strstr(method, "Mac") || strstr(method, "MAC")) {
        return (jobject)"00:00:00:00:00:00";
    }
    if (strstr(method, "Carrier")) {
        return (jobject)"";
    }
    if (strstr(method, "Language")) {
        return (jobject)"en";
    }
    if (strstr(method, "Country") || strstr(method, "Locale")) {
        return (jobject)"US";
    }
    if (strstr(method, "Firmware")) {
        return (jobject)"3.74";
    }
    if (strstr(method, "Manufacturer") || strstr(method, "Manufacture")) {
        return (jobject)"Sony";
    }
    if (strstr(method, "Model") || strstr(method, "Device")) {
        return (jobject)"PS Vita";
    }
    if (strstr(method, "CPUName")) {
        return (jobject)"ARM Cortex-A9";
    }
    if (strstr(method, "GPUName")) {
        return (jobject)"PowerVR SGX543MP4+";
    }
    if (strstr(method, "ObbPath")) {
        return (jobject)"ux0:/data/zombiewood/";
    }
    if (strstr(method, "MainObbName")) {
        return (jobject)"main.1501.com.gameloft.android.ANMP.GloftZOHM.obb";
    }
    if (strstr(method, "PatchObbName")) {
        return (jobject)"patch.15325.com.gameloft.android.ANMP.GloftZOHM.obb";
    }
    if (strstr(method, "ExtraObbName") || strstr(method, "ObbName")) {
        return (jobject)"";
    }
    if (strstr(method, "UserAgent")) {
        return (jobject)"gt-i9100";
    }
    return (jobject)"";
}

static jobject jni_CallObjectMethodV(JNIEnv *env, jobject obj,
                                     jmethodID methodID, va_list args) {
    (void)env;
    (void)obj;
    (void)args;
    const char *method = jni_id_cstr(methodID);
    JNI_TRACEF("JNI CallObjectMethodV('%s')\n", method);
    return jni_object_result_for_method(method);
}

static jobject jni_CallObjectMethodA(JNIEnv *env, jobject obj,
                                     jmethodID methodID, jvalue *args) {
    (void)env;
    (void)obj;
    (void)args;
    const char *method = jni_id_cstr(methodID);
    JNI_TRACEF("JNI CallObjectMethodA('%s')\n", method);
    return jni_object_result_for_method(method);
}

static jobject jni_NewObject(JNIEnv *env, jclass clazz,
                             jmethodID methodID, ...) {
    (void)env;
    (void)clazz;
    const char *method = jni_id_cstr(methodID);
    static uintptr_t next_fake_object = 0x81208000;
    next_fake_object += 0x20;
    JNI_TRACEF("JNI NewObject('%s') -> %p\n", method, (void *)next_fake_object);
    return (jobject)next_fake_object;
}

static jobject jni_NewObjectV(JNIEnv *env, jclass clazz,
                              jmethodID methodID, va_list args) {
    (void)args;
    return jni_NewObject(env, clazz, methodID);
}

static jobject jni_NewObjectA(JNIEnv *env, jclass clazz,
                              jmethodID methodID, jvalue *args) {
    (void)args;
    return jni_NewObject(env, clazz, methodID);
}

static jboolean jni_CallBooleanMethod(JNIEnv *env, jobject obj,
                                      jmethodID methodID, ...) {
    (void)env;
    (void)obj;
    (void)methodID;
    return JNI_TRUE;
}

static void jni_CallVoidMethod(JNIEnv *env, jobject obj,
                               jmethodID methodID, ...) {
    (void)env;
    (void)obj;
    const char *method = jni_id_cstr(methodID);
    JNI_TRACEF("JNI CallVoidMethod('%s')\n", method);
}

static void jni_CallVoidMethodV(JNIEnv *env, jobject obj,
                                jmethodID methodID, va_list args) {
    (void)args;
    jni_CallVoidMethod(env, obj, methodID);
}

static void jni_CallVoidMethodA(JNIEnv *env, jobject obj,
                                jmethodID methodID, jvalue *args) {
    (void)args;
    jni_CallVoidMethod(env, obj, methodID);
}

static jint jni_CallIntMethod(JNIEnv *env, jobject obj,
                              jmethodID methodID, ...) {
    (void)env;
    (void)obj;
    const char *method = jni_id_cstr(methodID);
    JNI_TRACEF("JNI CallIntMethod('%s')\n", method);
    if (strstr(method, "getPlayState")) return 3;
    return 0;
}

static jint jni_CallIntMethodV(JNIEnv *env, jobject obj,
                               jmethodID methodID, va_list args) {
    (void)args;
    return jni_CallIntMethod(env, obj, methodID);
}

static jint jni_CallIntMethodA(JNIEnv *env, jobject obj,
                               jmethodID methodID, jvalue *args) {
    (void)args;
    return jni_CallIntMethod(env, obj, methodID);
}

static jlong jni_CallLongMethod(JNIEnv *env, jobject obj,
                                jmethodID methodID, ...) {
    (void)env;
    (void)obj;
    const char *method = jni_id_cstr(methodID);
    if (strstr(method, "Time") || strstr(method, "UpTime")) {
        return (jlong)sceKernelGetProcessTimeWide() / 1000;
    }
    return 0;
}

static jint jni_RegisterNatives(JNIEnv *env, jclass clazz,
                                const JNINativeMethod *methods, jint nMethods) {
    (void)env;
    (void)clazz;
    (void)methods;
    JNI_TRACEF("JNI RegisterNatives(%d)\n", nMethods);
    return JNI_OK;
}

/* JNINativeInterface_ has 232 function pointer slots.
   Populate all of them with jni_no_op via a plain C99 array, then
   cast to the struct so the ABI layout matches. */
#define JNI_SLOT_COUNT 232
static void *g_jni_func_table[JNI_SLOT_COUNT];

static void jni_fake_env_init(void) {
    for (int i = 0; i < JNI_SLOT_COUNT; i++)
        g_jni_func_table[i] = (void *)jni_no_op;

#define JNI_SET_SLOT(name, fn) \
    g_jni_func_table[offsetof(JNINativeInterface, name) / sizeof(void *)] = (void *)(fn)
    JNI_SET_SLOT(GetVersion, jni_GetVersion);
    JNI_SET_SLOT(DefineClass, jni_DefineClass);
    JNI_SET_SLOT(FindClass, jni_FindClass);
    JNI_SET_SLOT(ExceptionOccurred, jni_ExceptionOccurred);
    JNI_SET_SLOT(ExceptionDescribe, jni_ExceptionDescribe);
    JNI_SET_SLOT(ExceptionClear, jni_ExceptionClear);
    JNI_SET_SLOT(NewGlobalRef, jni_NewGlobalRef);
    JNI_SET_SLOT(DeleteGlobalRef, jni_DeleteGlobalRef);
    JNI_SET_SLOT(DeleteLocalRef, jni_DeleteLocalRef);
    JNI_SET_SLOT(GetStaticMethodID, jni_GetStaticMethodID);
    JNI_SET_SLOT(GetMethodID, jni_GetMethodID);
    JNI_SET_SLOT(GetStaticFieldID, jni_GetStaticFieldID);
    JNI_SET_SLOT(NewObject, jni_NewObject);
    JNI_SET_SLOT(NewObjectV, jni_NewObjectV);
    JNI_SET_SLOT(NewObjectA, jni_NewObjectA);
    JNI_SET_SLOT(CallObjectMethod, jni_CallObjectMethod);
    JNI_SET_SLOT(CallObjectMethodV, jni_CallObjectMethodV);
    JNI_SET_SLOT(CallObjectMethodA, jni_CallObjectMethodA);
    JNI_SET_SLOT(CallBooleanMethod, jni_CallBooleanMethod);
    JNI_SET_SLOT(CallIntMethod, jni_CallIntMethod);
    JNI_SET_SLOT(CallIntMethodV, jni_CallIntMethodV);
    JNI_SET_SLOT(CallIntMethodA, jni_CallIntMethodA);
    JNI_SET_SLOT(CallLongMethod, jni_CallLongMethod);
    JNI_SET_SLOT(CallVoidMethod, jni_CallVoidMethod);
    JNI_SET_SLOT(CallVoidMethodV, jni_CallVoidMethodV);
    JNI_SET_SLOT(CallVoidMethodA, jni_CallVoidMethodA);
    JNI_SET_SLOT(NewStringUTF, jni_NewStringUTF);
    JNI_SET_SLOT(GetStringUTFLength, jni_GetStringUTFLength);
    JNI_SET_SLOT(GetStringUTFChars, jni_GetStringUTFChars);
    JNI_SET_SLOT(ReleaseStringUTFChars, jni_ReleaseStringUTFChars);
    JNI_SET_SLOT(GetArrayLength, jni_GetArrayLength);
    JNI_SET_SLOT(NewByteArray, jni_NewByteArray);
    JNI_SET_SLOT(GetByteArrayElements, jni_GetByteArrayElements);
    JNI_SET_SLOT(ReleaseByteArrayElements, jni_ReleaseByteArrayElements);
    JNI_SET_SLOT(GetByteArrayRegion, jni_GetByteArrayRegion);
    JNI_SET_SLOT(SetByteArrayRegion, jni_SetByteArrayRegion);
    JNI_SET_SLOT(GetStaticObjectField, jni_GetStaticObjectField);
    JNI_SET_SLOT(GetStaticIntField, jni_GetStaticIntField);
    JNI_SET_SLOT(SetStaticObjectField, jni_SetStaticObjectField);
    JNI_SET_SLOT(SetStaticIntField, jni_SetStaticIntField);
    JNI_SET_SLOT(CallStaticVoidMethod, jni_CallStaticVoidMethod);
    JNI_SET_SLOT(CallStaticVoidMethodV, jni_CallStaticVoidMethodV);
    JNI_SET_SLOT(CallStaticVoidMethodA, jni_CallStaticVoidMethodA);
    JNI_SET_SLOT(CallStaticBooleanMethod, jni_CallStaticBooleanMethod);
    JNI_SET_SLOT(CallStaticBooleanMethodV, jni_CallStaticBooleanMethodV);
    JNI_SET_SLOT(CallStaticBooleanMethodA, jni_CallStaticBooleanMethodA);
    JNI_SET_SLOT(CallStaticIntMethod, jni_CallStaticIntMethod);
    JNI_SET_SLOT(CallStaticIntMethodV, jni_CallStaticIntMethodV);
    JNI_SET_SLOT(CallStaticIntMethodA, jni_CallStaticIntMethodA);
    JNI_SET_SLOT(CallStaticLongMethod, jni_CallStaticLongMethod);
    JNI_SET_SLOT(CallStaticLongMethodV, jni_CallStaticLongMethodV);
    JNI_SET_SLOT(CallStaticLongMethodA, jni_CallStaticLongMethodA);
    JNI_SET_SLOT(CallStaticFloatMethod, jni_CallStaticFloatMethod);
    JNI_SET_SLOT(CallStaticFloatMethodV, jni_CallStaticFloatMethodV);
    JNI_SET_SLOT(CallStaticFloatMethodA, jni_CallStaticFloatMethodA);
    JNI_SET_SLOT(CallStaticObjectMethod, jni_CallStaticObjectMethod);
    JNI_SET_SLOT(CallStaticObjectMethodV, jni_CallStaticObjectMethodV);
    JNI_SET_SLOT(CallStaticObjectMethodA, jni_CallStaticObjectMethodA);
    JNI_SET_SLOT(ExceptionCheck, jni_ExceptionCheck);
    JNI_SET_SLOT(RegisterNatives, jni_RegisterNatives);
#undef JNI_SET_SLOT
}

/* g_jni_env is typed as void*** which is compatible with JNIEnv**
   and avoids pulling in jni.h internals. The .so just uses it as an
   opaque pointer whose first dereference gives the vtable. */
static void **g_jni_vtable_ptr = (void **)g_jni_func_table;
static void ***g_jni_env       = &g_jni_vtable_ptr;

static void log_jni_table_layout(void) {
    unsigned define_slot = (unsigned)(offsetof(JNINativeInterface, DefineClass) / sizeof(void *));
    unsigned find_slot = (unsigned)(offsetof(JNINativeInterface, FindClass) / sizeof(void *));
    unsigned get_static_method_slot = (unsigned)(offsetof(JNINativeInterface, GetStaticMethodID) / sizeof(void *));
    unsigned call_static_object_slot = (unsigned)(offsetof(JNINativeInterface, CallStaticObjectMethod) / sizeof(void *));
    LOGF("  JNI table: env=%p table=%p DefineClass[%u]=%p FindClass[%u]=%p GetStaticMethodID[%u]=%p CallStaticObjectMethod[%u]=%p\n",
         g_jni_env, g_jni_vtable_ptr,
         define_slot, g_jni_func_table[define_slot],
         find_slot, g_jni_func_table[find_slot],
         get_static_method_slot, g_jni_func_table[get_static_method_slot],
         call_static_object_slot, g_jni_func_table[call_static_object_slot]);
}

/* ─────────────────────────────────────────────
   Fake JavaVM (JNIInvokeInterface)
   The .so's JNI_OnLoad takes (JavaVM* vm, void* reserved). It typically
   calls vm->GetEnv(vm, &env, JNI_VERSION_1_4) and saves both vm and env
   into static globals. Helpers like NVThreadGetCurrentJNIEnv then call
   vm->AttachCurrentThread on the saved global to recover env.

   Slot layout of JNIInvokeInterface (each slot 4 bytes):
     0..2  reserved
     3     DestroyJavaVM
     4     AttachCurrentThread     (= byte offset 0x10 — slot the crash hit)
     5     DetachCurrentThread
     6     GetEnv
     7     AttachCurrentThreadAsDaemon
   ───────────────────────────────────────────── */
static int jvm_AttachCurrentThread(void *vm, void ***p_env, void *args) {
    if (p_env) *p_env = (void **)g_jni_env;
    return 0;  /* JNI_OK */
}
static int jvm_DetachCurrentThread(void *vm) { return 0; }
static int jvm_GetEnv(void *vm, void ***p_env, int version) {
    if (p_env) *p_env = (void **)g_jni_env;
    return 0;  /* JNI_OK */
}
static int jvm_DestroyJavaVM(void *vm) { return 0; }

#define JVM_SLOT_COUNT 8
static void *g_jvm_func_table[JVM_SLOT_COUNT];
static void **g_jvm_vtable_ptr = (void **)g_jvm_func_table;
static void ***g_jvm           = &g_jvm_vtable_ptr;

static void java_vm_init(void) {
    g_jvm_func_table[3] = (void*)jvm_DestroyJavaVM;
    g_jvm_func_table[4] = (void*)jvm_AttachCurrentThread;
    g_jvm_func_table[5] = (void*)jvm_DetachCurrentThread;
    g_jvm_func_table[6] = (void*)jvm_GetEnv;
}

/* Direct replacement for the .so's NVThreadGetCurrentJNIEnv.
   The original looks up a JavaVM* from a global that JNI_OnLoad
   populates and calls AttachCurrentThread on it; we just hand
   back our pre-built env. We hook this via hook_addr so even if
   JNI_OnLoad doesn't run for any reason, this entry point works. */
static void *hook_NVThreadGetCurrentJNIEnv(void) {
    return g_jni_env;
}

static void *hook_AndroidOS_GetEnv(void) {
#ifndef VITA3K_BUILD
    static unsigned int count = 0;
    if (count < 16) {
        LOGF("  AndroidOS_GetEnv hook -> fake JNI env %p table %p FindClass=%p\n",
             g_jni_env, g_jni_vtable_ptr,
             g_jni_func_table[offsetof(JNINativeInterface, FindClass) / sizeof(void *)]);
    }
    count++;
#endif
    return g_jni_env;
}

static void **g_ZombiesGame_singleton_ptr = NULL;

static void hook_ZombiesGame_SetInternet(void *self, int enabled) {
    if (self && g_ZombiesGame_singleton_ptr && !*g_ZombiesGame_singleton_ptr) {
        *g_ZombiesGame_singleton_ptr = self;
        LOGF("  ZombiesGame singleton captured from SetInternet -> %p\n", self);
    }

    if (!self) {
        LOGF("  ZombiesGame::SetInternet(%d) ignored because ZombiesGame singleton is NULL\n",
             enabled ? 1 : 0);
        return;
    }

    ((unsigned char *)self)[0x228] = enabled ? 1 : 0;
}

static void hook_ZombiesGame_PlayBackgroundMusic(void *self) {
    (void)self;
    static unsigned int count = 0;
    if (count < 8) {
        LOG("  ZombiesGame::PlayBackgroundMusic suppressed\n");
    }
    count++;
}

static void hook_ZombiesGame_PlayMusic(void *self, const char *name) {
    (void)self;
    static unsigned int count = 0;
    if (count < 16) {
        LOGF("  ZombiesGame::PlayMusic('%s') suppressed\n", name ? name : "(null)");
    }
    count++;
}

static void (*g_Application_Update_once)(void *self) = NULL;
static void (*g_Application_SetupDevice_once)(void *self) = NULL;
static void (*g_GlfApp_Update_once)(void *self) = NULL;
static void **g_GlfApp_global_slot = NULL;
static void *(*g_android_operator_new)(unsigned int size) = NULL;
static void (*g_CGlfDevice_ctor)(void *self, void *app) = NULL;
static void **g_device_ptr = NULL;  /* address of the g_device global singleton */
static void (*g_Application_InternalUpdate_orig)(void *self, int dt) = NULL;
static void (*g_Application_InternalDraw_orig)(void *self) = NULL;
static void (*g_SNManager_Update_orig)(void *self, int dt) = NULL;
static void (*g_CGameConfigManager_Update_orig)(void *self) = NULL;
static void (*g_CPlayerManager_Update_orig)(void *self, int dt) = NULL;
static void (*g_TrackingManager_Update_orig)(void *self) = NULL;
static int hook_service_noop_int(void);
static int hook_service_success_int(void);
static int hook_TrackingManager_virtual_noop(void *self);
static void (*g_ZombiesGame_Update_orig)(void *self) = NULL;
static void (*g_CTutorialManager_Update_orig)(void *self, int dt) = NULL;
static void (*g_CDownloadManager_Update_orig)(void *self) = NULL;
static void (*g_GLOTManager_Update_orig)(void *self, int dt) = NULL;
static uintptr_t g_zw_text_base = 0;
/* v4.50: on-screen debug overlay inputs */
static so_module *g_zw_mod = NULL;
static volatile void *g_zw_cur_state = NULL;
static volatile void *g_zw_cur_vtable = NULL;
static volatile int g_dbg_touch_x = -1, g_dbg_touch_y = -1, g_dbg_touch_down = 0;
static volatile unsigned g_dbg_touch_events = 0, g_dbg_cross_count = 0;
static volatile int g_zw_trace_request = 0;      /* v4.76 */
static volatile int g_overlay_on = 0;   /* v4.75: off by default (cost 52 ms/frame); SELECT shows it */
/* v4.59: per-phase frame timing (us) */
static SceUInt64 g_t_step, g_t_render, g_t_swap, g_t_frame_max;
static unsigned g_draws_step, g_draws_render;   /* v4.60 */
static int g_zw_single_pass = 1;                /* v4.73: only nativeRender (appUpdate -> glf::AndroidUpdate) per frame */
static SceUInt64 g_acc_step, g_acc_render, g_acc_swap, g_acc_wall_t0, g_acc_pre, g_acc_ovl;   /* v4.73: 60-frame timing log */
static uintptr_t g_GSLoading_Update_sym = 0;
/* g_main_loop_frame is defined above (v4.84) */

static void hook_Application_Run(void *self) {
    static unsigned int count = 0;
    if (count < 8) {
        LOGF("  Application::Run one-tick hook self=%p update=%p\n",
             self, (void *)g_Application_Update_once);
    }
    count++;

    if (g_Application_Update_once && self) {
        g_Application_Update_once(self);
    }
}

static void hook_Application_Run_thunk_48(void *self) {
    hook_Application_Run(self ? (void *)((uintptr_t)self - 48) : NULL);
}

static void hook_Application_Update_thunk_48_setup_once(void *self) {
    static unsigned int count = 0;
    static int unlocked_update = 0;
    void *real_self = self ? (void *)((uintptr_t)self - 48) : NULL;

    if (count < 2) {
        LOGF("  Application::Update thunk pass-through self=%p real_self=%p update=%p unlocked=%d\n",
             self, real_self, (void *)g_Application_Update_once, unlocked_update);
    }
    count++;

    if (real_self && !unlocked_update) {
        unlocked_update = 1;
        *(uint8_t *)((uintptr_t)real_self + 0x11F74) = 1;
        LOG("  Application::Update thunk: forced update setup gate open\n");
    }

    if (real_self && g_Application_Update_once) {
        g_Application_Update_once(real_self);
        if (count <= 2) {
            LOG("  Application::Update thunk: returned from real update\n");
        }
    }
}

static void hook_GlfApp_Run(void *self) {
    static unsigned int count = 0;
    if (count < 8) {
        LOGF("  glf::App::Run one-tick hook self=%p update=%p\n",
             self, (void *)g_GlfApp_Update_once);
    }
    count++;

    if (g_GlfApp_Update_once && self) {
        g_GlfApp_Update_once(self);
    }
}

static void hook_GlfApp_Update_probe(void *self) {
    (void)self;
    static unsigned int count = 0;
    if (count < 8) {
        LOG("  glf::App::Update probe skipped\n");
    }
    count++;
}

static void hook_glitch_createDeviceEx_nonblocking(void **out, void *app) {
    LOGF("  glitch::createDeviceEx nonblocking hook out=%p app=%p new=%p ctor=%p\n",
         out, app, (void *)g_android_operator_new, (void *)g_CGlfDevice_ctor);

    if (!out) {
        return;
    }

    *out = NULL;
    if (!g_android_operator_new || !g_CGlfDevice_ctor) {
        LOG("  glitch::createDeviceEx nonblocking hook missing constructor/new\n");
        return;
    }

    void *device = g_android_operator_new(0xA4);
    if (!device) {
        LOG("  glitch::createDeviceEx nonblocking hook operator new returned NULL\n");
        return;
    }

    g_CGlfDevice_ctor(device, app);
    *out = device;

    /* Keep one extra reference alive for Application::SetupDevice. The original
     * Android device loop owns this lifetime; our nonblocking replacement must
     * not let the temporary intrusive_ptr cleanup destroy the device immediately.
     */
    volatile int *refcount = (volatile int *)((uintptr_t)device + 4);
    *refcount = *refcount + 2;

    /* Set the g_device singleton so SWFInit can find the device. */
    if (g_device_ptr) {
        *g_device_ptr = device;
        LOGF("  glitch::createDeviceEx set g_device global at %p -> %p\n", g_device_ptr, device);
    }

    LOGF("  glitch::createDeviceEx nonblocking hook returned device=%p ref=%d video=%p\n",
         device, *refcount, *(void **)((uintptr_t)device + 8));
}

static int hook_glitch_IDevice_run_noop(void *self) {
    LOGF("  glitch::IDevice::run suppressed self=%p\n", self);
    return 1;
}

static void hook_CGlfDevice_createDriver_bypass(void *self) {
    LOGF("  CGlfDevice::createDriver bypassed self=%p\n", self);
    if (self) {
        *(void **)((uintptr_t)self + 8) = NULL;
    }
}

static void hook_ZombiesGame_FirstTimeInit_probe(void *self) {
    LOGF("  ZombiesGame::FirstTimeInit probe called unexpectedly self=%p\n", self);
}

static void hook_Application_PostInit_probe(void *self) {
    LOGF("  Application::PostInit suppressed self=%p\n", self);
}

static void hook_firsttime_online_void(void *self) {
    LOGF("  FirstTimeInit online/service call suppressed self=%p\n", self);
}

static int hook_CDownloadManager_InitParsePack(void *self) {
    LOGF("  CDownloadManager::InitParsePack suppressed self=%p\n", self);
    return 0;
}

static int hook_CNotificationManager_Init(void *self) {
    LOGF("  CNotificationManager::Init suppressed self=%p\n", self);
    return 0;
}

static void hook_CNotificationManager_AddNotification(void *self, int type, int id, int seconds, const void *payload) {
    static unsigned int count = 0;
    if (count < 24) {
        LOGF("  CNotificationManager::AddNotification suppressed self=%p type=%d id=%d seconds=%d payload=%p\n",
             self, type, id, seconds, payload);
    }
    count++;
}

static int hook_CNotificationManager_CanDisplay(void *self) {
    static unsigned int count = 0;
    if (count < 8) {
        LOGF("  CNotificationManager::CanDisplay suppressed self=%p -> 0\n", self);
    }
    count++;
    return 0;
}

static int hook_CNotificationManager_AddLocalNotifications(void *self) {
    static unsigned int count = 0;
    if (count < 8) {
        LOGF("  CNotificationManager::AddLocalNotifications suppressed self=%p\n", self);
    }
    count++;
    return 0;
}

static void hook_ZombiesGame_GetCountryIpCode(void *self) {
    if (self && g_ZombiesGame_singleton_ptr && !*g_ZombiesGame_singleton_ptr) {
        *g_ZombiesGame_singleton_ptr = self;
        LOGF("  ZombiesGame singleton captured from GetCountryIpCode -> %p\n", self);
    }
    LOGF("  ZombiesGame::GetCountryIpCode suppressed self=%p\n", self);
}

static int hook_UrlRedirect_QueueRequest(void) {
    static unsigned int count = 0;
    if (count < 16) {
        LOG("  UrlRedirect::QueueRequest suppressed\n");
    }
    count++;
    return 0;
}

static void hook_TapJoyManager_UpdateRequest(void *self) {
    static unsigned int count = 0;
    if (count < 8) {
        LOGF("  TapJoyManager::UpdateRequest suppressed self=%p\n", self);
    }
    count++;
}

static void hook_GlfEventManager_DispatchEvents(void *self) {
    (void)self;
}

static void hook_GlfDebugger_UpdateFrame(void *self) {
    (void)self;
}

static void hook_GlfPerfCounters_UpdateValue(const char *name, unsigned int value) {
    (void)name;
    (void)value;
}

static void hook_GlfInputManager_UpdateFrame(void *self) {
    (void)self;
}

static void hook_SNManager_Update_trace(void *self, int dt) {
    static unsigned int count = 0;
    int verbose = count < 24;
    if (verbose) LOGF("  _Update: before SNManager::Update self=%p dt=%d\n", self, dt);
    count++;
    if (g_SNManager_Update_orig) g_SNManager_Update_orig(self, dt);
    if (verbose) LOG("  _Update: after SNManager::Update\n");
}

static void hook_SNManager_Update_noop(void *self, int dt) {
    static unsigned int count = 0;
    if (count < 24) {
        LOGF("  SNManager::Update suppressed self=%p dt=%d\n", self, dt);
    }
    count++;
}

static void hook_CGameConfigManager_Update_trace(void *self) {
    static unsigned int count = 0;
    int verbose = count < 24;
    if (verbose) LOGF("  _Update: before CGameConfigManager::Update self=%p\n", self);
    count++;
    if (g_CGameConfigManager_Update_orig) g_CGameConfigManager_Update_orig(self);
    if (verbose) LOG("  _Update: after CGameConfigManager::Update\n");
}

static void hook_CGameConfigManager_Update_noop(void *self) {
    static unsigned int count = 0;
    if (count < 24) {
        LOGF("  CGameConfigManager::Update suppressed self=%p\n", self);
    }
    count++;
}

static void hook_CPlayerManager_Update_trace(void *self, int dt) {
    static unsigned int count = 0;
    int verbose = count < 24;
    if (verbose) LOGF("  _Update: before CPlayerManager::Update self=%p dt=%d\n", self, dt);
    count++;
    if (g_CPlayerManager_Update_orig) g_CPlayerManager_Update_orig(self, dt);
    if (verbose) LOG("  _Update: after CPlayerManager::Update\n");
}

static void hook_CPlayerManager_Update_noop(void *self, int dt) {
    static unsigned int count = 0;
    if (count < 24) {
        LOGF("  CPlayerManager::Update suppressed self=%p dt=%d\n", self, dt);
    }
    count++;
}

static void hook_gameswf_initialize_probe(void *params) {
    LOGF("  gameswf::initialize probe params=%p -> suppressed (hang test)\n", params);
}

static void hook_gameswf_createRenderHandler_probe(void *params) {
    LOGF("  gameswf::createRenderHandler probe params=%p -> suppressed\n", params);
}

static void hook_Application_SetupDevice_noop(void *self) {
    LOGF("  Application::SetupDevice suppressed self=%p (CResFileManager::Inst is null)\n", self);
}

static void hook_TrackingManager_Update_trace(void *self) {
    static unsigned int count = 0;
    int verbose = count < 24;
    if (verbose) LOGF("  _Update: before glot::TrackingManager::Update self=%p\n", self);
    count++;
    if (g_TrackingManager_Update_orig) g_TrackingManager_Update_orig(self);
    if (verbose) LOG("  _Update: after glot::TrackingManager::Update\n");
}

static void hook_TrackingManager_Update_noop(void *self) {
    static unsigned int count = 0;
    if (count < 24) {
        LOGF("  glot::TrackingManager::Update suppressed self=%p\n", self);
    }
    count++;
}

static void hook_ZombiesGame_Update_trace(void *self) {
    static unsigned int count = 0;
    int verbose = count < 8;
    if (verbose) LOGF("  _Update: before ZombiesGame::Update self=%p\n", self);
    count++;
    if (g_ZombiesGame_Update_orig) g_ZombiesGame_Update_orig(self);
    if (verbose) LOG("  _Update: after ZombiesGame::Update\n");
}

static void hook_ZombiesGame_Update_noop(void *self) {
    static unsigned int count = 0;
    if (count < 24) {
        LOGF("  ZombiesGame::Update suppressed self=%p\n", self);
    }
    count++;
}

static void hook_CComponentLeaderboardReward_Load(void *self, void *stream) {
    static unsigned int count = 0;
    if (count < 24) {
        LOGF("  CComponentLeaderboardReward::Load suppressed self=%p stream=%p\n",
             self, stream);
    }
    count++;
}

static void hook_CDownloadManager_Update_noop(void *self) {
    static unsigned int count = 0;
    if (count < 24) {
        LOGF("  CDownloadManager::Update suppressed self=%p\n", self);
    }
    count++;
}

static void hook_CTutorialManager_Update_trace(void *self, int dt) {
    static unsigned int count = 0;
    int verbose = count < 24;
    if (verbose) LOGF("  _Update: before CTutorialManager::Update self=%p dt=%d\n", self, dt);
    count++;
    if (g_CTutorialManager_Update_orig) g_CTutorialManager_Update_orig(self, dt);
    if (verbose) LOG("  _Update: after CTutorialManager::Update\n");
}

static void hook_GLOTManager_Update_noop(void *self, int dt) {
    static unsigned int count = 0;
    if (count < 24) {
        LOGF("  GLOTManager::Update suppressed self=%p dt=%d\n", self, dt);
    }
    count++;
}

static void hook_CDownloadManager_Update_trace(void *self) {
    static unsigned int count = 0;
    int verbose = count < 24;
    if (verbose) LOGF("  _Update: before CDownloadManager::Update self=%p\n", self);
    count++;
    if (g_CDownloadManager_Update_orig) g_CDownloadManager_Update_orig(self);
    if (verbose) LOG("  _Update: after CDownloadManager::Update\n");
}

static void hook_GLOTManager_Update_trace(void *self, int dt) {
    static unsigned int count = 0;
    int verbose = count < 24;
    if (verbose) LOGF("  _Update: before GLOTManager::Update self=%p dt=%d\n", self, dt);
    count++;
    if (g_GLOTManager_Update_orig) g_GLOTManager_Update_orig(self, dt);
    if (verbose) LOG("  _Update: after GLOTManager::Update\n");
}

static void hook_Application_InternalUpdate(void *self, int dt) {
    static unsigned int count = 0;
    if (count < 16) {
        LOGF("  Application::_Update suppressed self=%p dt=%d\n", self, dt);
    }
    count++;
}

static void hook_Application_InternalDraw(void *self) {
    static unsigned int count = 0;
    if (count < 24 || (count % 60) == 0) {
        uintptr_t sentinel = (uintptr_t)self + 0x20;
        void *first = self ? *(void **)sentinel : NULL;
        void *tail = self ? *(void **)((uintptr_t)self + 0x24) : NULL;
        void *tail_obj = NULL;
        void *vtable = NULL;
        void *draw_fn = NULL;

        if (tail && (uintptr_t)tail >= 0x80000000u && (uintptr_t)tail < 0xA0000000u) {
            tail_obj = *(void **)((uintptr_t)tail + 8);
            if (tail_obj && (uintptr_t)tail_obj >= 0x80000000u && (uintptr_t)tail_obj < 0xA0000000u) {
                vtable = *(void **)tail_obj;
                if (vtable && (uintptr_t)vtable >= 0x80000000u && (uintptr_t)vtable < 0xA0000000u) {
                    draw_fn = *(void **)((uintptr_t)vtable + 0x18);
                }
            }
        }

        LOGF("  Application::_Draw skipped self=%p list=%p first=%p tail=%p tail_obj=%p vtable=%p draw_fn=%p\n",
             self, (void *)sentinel, first, tail, tail_obj, vtable, draw_fn);
    }
    count++;
}

static void *maybe_unlock_gsloading_state(void *app) {
    if (!app || !g_GSLoading_Update_sym) return NULL;

    uintptr_t sentinel = (uintptr_t)app + 0x20;
    void *tail = *(void **)((uintptr_t)app + 0x24);
    if (!tail || (uintptr_t)tail == sentinel ||
        (uintptr_t)tail < 0x80000000u || (uintptr_t)tail >= 0xA0000000u) {
        return NULL;
    }

    void *state = *(void **)((uintptr_t)tail + 8);
    if (!state || (uintptr_t)state < 0x80000000u || (uintptr_t)state >= 0xA0000000u) {
        return NULL;
    }

    void **vtable = *(void ***)state;
    if (!vtable || (uintptr_t)vtable < 0x80000000u || (uintptr_t)vtable >= 0xA0000000u) {
        return NULL;
    }

    uintptr_t update_fn = (uintptr_t)*(void **)((uintptr_t)vtable + 0x10);
    g_zw_cur_state = state;
    g_zw_cur_vtable = vtable;

    /* v4.45: report the active state (as .so offsets) every 300 frames or on change. */
    {
        static void **last_vtable = NULL;
        if (vtable != last_vtable) {   /* v5.01: on change only (periodic copy cost 15 ms every 5 s) */
            last_vtable = vtable;
            LOGF("  v4.45 state frame=%d state=%p vtable=+0x%x update=+0x%x\n",
                 g_main_loop_frame, state,
                 (unsigned)((uintptr_t)vtable - g_zw_text_base),
                 (unsigned)((update_fn & ~1u) - g_zw_text_base));
        }
    }

    if ((update_fn & ~1u) != (g_GSLoading_Update_sym & ~1u)) {
        return NULL;
    }

    uint32_t elapsed = *(uint32_t *)((uintptr_t)state + 0x108);

    /*
     * v4.47: field forcing disabled. GSLoading::Update itself sets
     * state+0x11c = 1 once the real CMenu::Loading completes (0x5a1a18);
     * clearing it every frame here undid that and kept GSLoading forever.
     */
    if (0 && g_main_loop_frame >= 120) {
        *(unsigned char *)((uintptr_t)state + 0x120) = 0;
        *(unsigned char *)((uintptr_t)state + 0x11f) = 0;
        *(unsigned char *)((uintptr_t)state + 0x11c) = 0;
        *(unsigned char *)((uintptr_t)state + 0x11d) = 1;
        if (elapsed < 3000) {
            *(uint32_t *)((uintptr_t)state + 0x108) = 3000;
        }
    }

    return state;
}

static void hook_Application_UpdateApp_trace(void *self, int dt) {
    static unsigned int count = 0;
    count++;

    if (dt < 0) {
        return;
    }

    if (g_Application_InternalUpdate_orig) {
        void *gsloading_state = maybe_unlock_gsloading_state(self);
        g_Application_InternalUpdate_orig(self, dt);
        if (gsloading_state && g_main_loop_frame >= 120) {
            static int last_direct_frame = -1;
            if (last_direct_frame == g_main_loop_frame) {
                goto after_direct_gsloading_drive;
            }
            last_direct_frame = g_main_loop_frame;

            static unsigned int direct_calls = 0;
            if (direct_calls >= 24 && (g_main_loop_frame % 4) != 0) {
                goto after_direct_gsloading_drive;
            }
            if (direct_calls < 2) {
                LOGF("  direct GSLoading::Update drive frame=%d state=%p app=%p dt=%d\n",
                     g_main_loop_frame, gsloading_state, self, dt);
            }
            direct_calls++;
            ((void (*)(void *, void *, int))g_GSLoading_Update_sym)(gsloading_state, self, dt);
        }
after_direct_gsloading_drive:
        ;
    }

    if (g_Application_InternalDraw_orig) {
        uintptr_t sentinel = self ? (uintptr_t)self + 0x20 : 0;
        void *first = sentinel ? *(void **)sentinel : NULL;
        int draw_ready = first && (uintptr_t)first != sentinel;

        if (draw_ready) {
            g_Application_InternalDraw_orig(self);
        } else {
            hook_Application_InternalDraw(self);
        }
    }
}

static void hook_SNManager_SetOfflineName(void *self, const void *name) {
    LOGF("  SNManager::SetOfflineName suppressed self=%p name=%p\n", self, name);
}

static void hook_AndroidUpdate_probe(void) {
    static unsigned int count = 0;
    void *app = (g_GlfApp_global_slot ? *g_GlfApp_global_slot : NULL);
    void **vtable = app ? *(void ***)app : NULL;
    void *slot_24 = vtable ? *(void **)((uintptr_t)vtable + 0x24) : NULL;

    if (count < 16) {
        LOGF("  AndroidUpdate probe: gAppSlot=%p app=%p vtable=%p slot24=%p\n",
             (void *)g_GlfApp_global_slot, app, vtable, slot_24);
    }
    count++;
}

static int hook_DLC_CheckForDLC(void *self) {
    (void)self;
    LOG("  CDownloadManager::CheckForDLC suppressed\n");
    return 0;
}

static int hook_DLC_LoadDLCData(void *self, const char *path, int *out_count) {
    (void)self;
    if (out_count) *out_count = 0;
    LOGF("  CDownloadManager::LoadDLCData('%s') suppressed\n",
         path ? path : "(null)");
    return 0;
}

static void hook_DLC_CleanDLCData(void *self) {
    (void)self;
}

static int hook_GLOTManager_GetDLCStatus(void *self, int pack_id) {
    (void)self;
    LOGF("  GLOTManager::GetDLCStatus(%d) -> 0\n", pack_id);
    return 0;
}

static int hook_GLOTManager_InitManager(void *self) {
    (void)self;
    LOG("  GLOTManager::InitManager suppressed\n");
    return 0;
}

static void *hook_glwebtools_thread_update(void *self) {
    (void)self;
    LOG("  glwebtools::ThreadPosixImpl_Data::Update suppressed\n");
    return NULL;
}

static int hook_TrackingManager_Initialize(void *self, void *server_config,
                                           unsigned int a, const void *b,
                                           const void *c, const void *d,
                                           const void *e) {
    (void)self;
    (void)server_config;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    LOG("  glot::TrackingManager::Initialize suppressed\n");
    return 0;
}

static void *hook_TrackingManager_GetInstance(void) {
    static unsigned int count = 0;
    static uintptr_t fake_vtable[256];
    static uintptr_t fake_object[64];
    static int initialized = 0;

    if (!initialized) {
        for (unsigned int i = 0; i < sizeof(fake_vtable) / sizeof(fake_vtable[0]); i++) {
            fake_vtable[i] = (uintptr_t)hook_TrackingManager_virtual_noop | 1u;
        }
        memset(fake_object, 0, sizeof(fake_object));
        fake_object[0] = (uintptr_t)fake_vtable;
        initialized = 1;
    }

    if (count < 8) {
        LOGF("  glot::TrackingManager::GetInstance suppressed -> fake %p\n",
             (void *)fake_object);
    }
    count++;
    return fake_object;
}

static int hook_TrackingManager_virtual_noop(void *self) {
    static unsigned int count = 0;
    if (count < 24) {
        LOGF("  glot::TrackingManager virtual noop self=%p\n", self);
    }
    count++;
    return 0;
}

static void hook_TrackingManager_Update(void *self) {
    (void)self;
}

static void hook_TrackingManager_AddEvent(void) {
    static unsigned int count = 0;
    if (count < 8) {
        LOG("  glot::TrackingManager::AddEvent suppressed\n");
    }
    count++;
}

static void hook_TrackingEventValue_ctor_cstr(void *self, const char *value) {
    static unsigned int count = 0;
    if (self) memset(self, 0, 16);
    if (count < 8) {
        LOGF("  glot::EventValue(const char *=%p) suppressed\n", value);
    }
    count++;
}

static void hook_TrackingEventValue_ctor_string(void *self, const void *value) {
    static unsigned int count = 0;
    if (self) memset(self, 0, 16);
    if (count < 8) {
        LOGF("  glot::EventValue(string=%p) suppressed\n", value);
    }
    count++;
}

static void hook_TrackingEventValue_ctor_copy(void *self, const void *other) {
    static unsigned int count = 0;
    if (self) memset(self, 0, 16);
    if (count < 8) {
        LOGF("  glot::EventValue(copy=%p) suppressed\n", other);
    }
    count++;
}

static void hook_TrackingEventValue_dtor(void *self) {
    (void)self;
}

void vita_qsort(void *base, size_t nmemb, size_t size,
                int (*compar)(const void *, const void *)) {
    if (!base || nmemb < 2 || size == 0 || size > 0x1000 ||
        !compar || (uintptr_t)compar < 0x80000000u) {
        return;
    }

    qsort(base, nmemb, size, compar);
}

char *vita_strstr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if ((uintptr_t)haystack < 0x80000000u || (uintptr_t)needle < 0x80000000u) {
        return NULL;
    }

    size_t needle_len = 0;
    while (needle_len < 256 && needle[needle_len]) needle_len++;
    if (needle_len == 0) return (char *)haystack;
    if (needle_len >= 256) return NULL;

    for (size_t i = 0; i < 4096; i++) {
        if (!haystack[i]) return NULL;
        if (haystack[i] != needle[0]) continue;

        size_t j = 1;
        while (j < needle_len && haystack[i + j] == needle[j]) j++;
        if (j == needle_len) return (char *)(haystack + i);
    }

    static unsigned int count = 0;
    if (count < 8) {
        sceClibPrintf("[ZW] strstr(%p,%p) bounded out\n", haystack, needle);
    }
    count++;
    return NULL;
}

static void patch_import_symbol(so_module *mod, const char *symbol, uintptr_t replacement) {
    if (!mod || !symbol || !replacement) return;

    unsigned int patched = 0;
    for (int i = 0; i < mod->num_reldyn + mod->num_relplt; i++) {
        Elf32_Rel *rel = i < mod->num_reldyn ? &mod->reldyn[i] : &mod->relplt[i - mod->num_reldyn];
        Elf32_Sym *sym = &mod->dynsym[ELF32_R_SYM(rel->r_info)];
        if (sym->st_shndx != SHN_UNDEF || !sym->st_name) continue;

        const char *name = mod->dynstr + sym->st_name;
        if (strcmp(name, symbol) != 0) continue;

        uintptr_t *slot = (uintptr_t *)(mod->text_base + rel->r_offset);
        uintptr_t old = *slot;
#ifdef VITA3K_BUILD
        sceKernelOpenVMDomain();
        memcpy(slot, &replacement, sizeof(replacement));
        sceKernelSyncVMDomain(0, slot, sizeof(replacement));
        sceKernelCloseVMDomain();
#else
        memcpy(slot, &replacement, sizeof(replacement));
#endif
        patched++;
        LOGF("  patched import %s slot=%p old=0x%08X new=0x%08X\n",
             symbol, slot, (unsigned)old, (unsigned)replacement);
    }

    if (patched) so_flush_caches(mod);
}

static void patch_thumb_halfwords(so_module *mod, uintptr_t addr,
                                  const uint16_t *words, unsigned int count,
                                  const char *label) {
    if (!addr || !words || !count) return;

    sceKernelOpenVMDomain();
#ifdef VITA3K_BUILD
    memcpy((void *)addr, (const void *)words, count * sizeof(uint16_t));
    sceKernelSyncVMDomain(0, (void *)addr, count * sizeof(uint16_t));
#else
    kuKernelCpuUnrestrictedMemcpy((void *)addr, (const void *)words,
                                  count * sizeof(uint16_t));
#endif
    sceKernelCloseVMDomain();
    if (mod) so_flush_caches(mod);
    LOGF("  patched %s at 0x%08X (%u halfwords)\n",
         label ? label : "thumb code", (unsigned)addr, count);
    if (count <= 4) {
        const uint16_t *readback = (const uint16_t *)addr;
        LOGF("    readback: %04x %04x %04x %04x\n",
             count > 0 ? readback[0] : 0,
             count > 1 ? readback[1] : 0,
             count > 2 ? readback[2] : 0,
             count > 3 ? readback[3] : 0);
    }
}

static void hook_TrackingManager_OnPause(void *self) {
    (void)self;
}

static void hook_TrackingManager_OnResume(void *self) {
    (void)self;
}

static int hook_service_noop_int(void) {
    return 0;
}

static int hook_service_success_int(void) {
    return 1;
}

static void hook_service_noop_void(void) {
}

/* v5.04: the v4.41/v4.43 watchdogs wrote to address 0 to get a core dump when boot looked
 * stuck (no frame/log progress for 60 s). A slow card or a long level load must not crash
 * a release build, so this is off unless a hang is being debugged. */
#ifndef ZW_FORCE_COREDUMP
#define ZW_FORCE_COREDUMP 0
#endif
/*
 * v4.42: keep the Vita awake. Nothing ticked the power timer, so auto-standby
 * and screen dimming could kick in during long loads/hangs (and drop Wi-Fi,
 * which breaks VitaCompanion log/dump pulls). Tick from our own thread so it
 * keeps working even while the game thread is stuck.
 */
static int zw_power_tick_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
#ifndef VITA3K_BUILD
    /*
     * v4.43: boot-time stall watchdog (hardware). If neither the log nor the
     * main-loop frame counter moves for 60 s, force a core dump so the .psp2dmp shows where it stopped.
     */
    uint32_t last_bytes = g_log_bytes;
    int last_frame = g_main_loop_frame;
    int idle_s = 0;
#endif
    for (;;) {
        /* v5.10: keep awake only through boot (no input while loading); after the menu the
         * system auto-sleep applies again (buttons and touch reset its idle timer). */
        if (g_main_loop_frame < 60) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        sceKernelDelayThread(5 * 1000 * 1000);
        zw_log_flush();   /* v5.04: the log is RAM-buffered; at most 5 s is lost on a crash */
        zw_obb_hdr_save_if_dirty();   /* v5.06 */
#ifndef VITA3K_BUILD
        if (g_log_bytes != last_bytes ||
            g_main_loop_frame != last_frame) {
            last_bytes = g_log_bytes;
            last_frame = g_main_loop_frame;
            idle_s = 0;
            continue;
        }
        idle_s += 5;
        if (ZW_FORCE_COREDUMP && idle_s >= 60) {
            LOGF("  v4.43 stall watchdog: no log/frame progress for %d s (frame=%d), forcing core dump\n",
                 idle_s, g_main_loop_frame);
            sceKernelDelayThread(500 * 1000);
            *(volatile uint32_t *)0x00000000 = 0x5354414Cu; /* "STAL" */
        }
#endif
    }
    return 0;
}

static void zw_start_power_tick(void) {
    SceUID t = sceKernelCreateThread("zw_power_tick", zw_power_tick_thread,
                                     0x10000100, 0x1000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
    LOGF("  v4.42 power tick thread uid=0x%x\n", (unsigned)t);
}

static int hook_language_index0(void) {
    LOG("  language index hook -> 0\n");
    return 0;
}

static const char *hook_language_en(void) {
    LOG("  language string hook -> en\n");
    return "en";
}

static int hook_InAppBilling_return0(void) {
    static unsigned int count = 0;
    if (count < 24) {
        LOG("  Android IAP call suppressed -> 0\n");
    }
    count++;
    return 0;
}

static void *hook_InAppBilling_return_null(void) {
    static unsigned int count = 0;
    if (count < 16) {
        LOG("  Android IAP pointer call suppressed -> NULL\n");
    }
    count++;
    return NULL;
}

static void hook_InAppBilling_void(void) {
    static unsigned int count = 0;
    if (count < 16) {
        LOG("  Android IAP void call suppressed\n");
    }
    count++;
}

/* ─────────────────────────────────────────────
   v5.05: local in-app store.
   The Java/JNI billing layer is gone, so the bottom of InAppBilling is replaced
   by a fixed 6-pack list and the game's own callbacks are driven from the main loop:
     CIapManager::Init / ForceRefreshProducts -> QueryProductListFromServer (state=1)
       -> InAppBilling_GetItemList  [hook: queue]  -> next frame: real GetItemListCB(0, N)
          which reads our items via IAB_STORE_ITEM::getProperty [hook] and sets state=2.
     CMenuIap -> CIapManager::BuyProduct -> InAppBilling_BuyItem [hook: queue] (state=3)
       -> next frame: real BuyItemCB(id, 0=ok / 1=fail, "")
          -> CIapManager::OnPurchaseSuccess (grants pack, CPlayerManager::Save).
   Owner rule (2026-11-12): Cash packs are free; Coin packs cost in-game Cash.
   IAB_STORE_ITEM (0x20 bytes): +0 std::string id, +4 std::string type ("cash" = cash
   pack), +8 std::map<string,string> properties (header +0xc, root +0x10).
   ───────────────────────────────────────────── */
typedef struct { const char *id, *type, *amount, *price, *symbol, *name; } zw_iap_pack;
static const zw_iap_pack g_zw_iap_packs[] = {
    { "zw_vita_cash_1",  "cash",  "25",    "0",  "FREE", "25 Cash"     },
    { "zw_vita_cash_2",  "cash",  "60",    "0",  "FREE", "60 Cash"     },
    { "zw_vita_cash_3",  "cash",  "150",   "0",  "FREE", "150 Cash"    },
    { "zw_vita_coins_1", "coins", "5000",  "10", "CASH", "5000 Coins"  },
    { "zw_vita_coins_2", "coins", "12000", "20", "CASH", "12000 Coins" },
    { "zw_vita_coins_3", "coins", "30000", "45", "CASH", "30000 Coins" },
};
#define ZW_IAP_N ((int)(sizeof(g_zw_iap_packs) / sizeof(g_zw_iap_packs[0])))
#define ZW_IAP_OFF_STR_CTOR      0xaea650   /* std::string(const char*, const alloc&), ARM */
#define ZW_IAP_OFF_LIST_CB       0x356fa0   /* InAppBilling_GetItemListCB(int, int) */
#define ZW_IAP_OFF_BUY_CB        0x357afc   /* InAppBilling_BuyItemCB(const char*, int, const char*) */
#define ZW_IAP_OFF_PLAYERMGR_VAR 0xce0bd8   /* CPlayerManager* global (from BuyFakeProduct) */
#define ZW_IAP_OFF_GETCASH       0x40edf4   /* CPlayerManager::GetCash() const */
#define ZW_IAP_OFF_REMOVECASH    0x40ff50   /* CPlayerManager::RemoveCash(int) */
static uintptr_t g_zw_iap_base;
static unsigned char *g_zw_iap_items;          /* new[]-style: 8-byte header, then N items */
static volatile int g_zw_iap_list_pending;
static volatile int g_zw_iap_buy_pending;
static char g_zw_iap_buy_id[64];

static void zw_iap_make_string(void *dst, const char *s) {
    static const unsigned int alloc_dummy = 0;
    void (*ctor)(void *, const char *, const void *) =
        (void (*)(void *, const char *, const void *))(g_zw_iap_base + ZW_IAP_OFF_STR_CTOR);
    ctor(dst, s ? s : "", &alloc_dummy);
}

static const zw_iap_pack *zw_iap_find(const char *id) {
    for (int i = 0; id && i < ZW_IAP_N; i++)
        if (strcmp(g_zw_iap_packs[i].id, id) == 0) return &g_zw_iap_packs[i];
    return NULL;
}

static void *hook_InAppBilling_GetItemInfoArray_local(void) {
    if (!g_zw_iap_items) {
        g_zw_iap_items = (unsigned char *)calloc(1, 8 + ZW_IAP_N * 0x20);
        if (!g_zw_iap_items) return NULL;
        ((uint32_t *)g_zw_iap_items)[0] = 0x20;
        ((uint32_t *)g_zw_iap_items)[1] = ZW_IAP_N;
        for (int i = 0; i < ZW_IAP_N; i++) {
            unsigned char *it = g_zw_iap_items + 8 + i * 0x20;
            zw_iap_make_string(it + 0, g_zw_iap_packs[i].id);
            zw_iap_make_string(it + 4, g_zw_iap_packs[i].type);
            *(uint32_t *)(it + 0x0c) = 0;                       /* rb header color */
            *(uint32_t *)(it + 0x10) = 0;                       /* root */
            *(uintptr_t *)(it + 0x14) = (uintptr_t)(it + 0x0c); /* leftmost */
            *(uintptr_t *)(it + 0x18) = (uintptr_t)(it + 0x0c); /* rightmost */
            *(uint32_t *)(it + 0x1c) = 0;                       /* count */
        }
    }
    return g_zw_iap_items + 8;
}

static void hook_InAppBilling_FreeItemInfoArray_local(void *arr) {
    (void)arr;   /* the local array is static for the life of the process */
}

/* IAB_STORE_ITEM::getProperty(const char*) const -> std::string (sret in r0) */
static void *hook_IAB_STORE_ITEM_getProperty(void *ret, void *item, const char *key) {
    const char *v = "";
    int idx = g_zw_iap_items ? (int)(((unsigned char *)item - (g_zw_iap_items + 8)) / 0x20) : -1;
    if (idx >= 0 && idx < ZW_IAP_N && key) {
        const zw_iap_pack *p = &g_zw_iap_packs[idx];
        if (!strcmp(key, "amount")) v = p->amount;
        else if (!strcmp(key, "price") || !strcmp(key, "old_price")) v = p->price;   /* equal -> no promo */
        else if (!strcmp(key, "currency") || !strcmp(key, "currency_symbol")) v = p->symbol;
        else if (!strcmp(key, "name") || !strcmp(key, "description")) v = p->name;
        else {
            static unsigned other_logged;
            if (other_logged++ < 16) LOGF("  v5.05 IAP getProperty(%s, %s) -> \"\"\n", p->id, key);
        }
    }
    zw_iap_make_string(ret, v);
    return ret;
}

/* getAttByID / getShopAttributeByName return std::string via sret; no server data here. */
static void *hook_InAppBilling_getAttByID_local(void *ret, const char *id, const char *key) {
    static unsigned logged;
    if (logged++ < 8) LOGF("  v5.05 IAP getAttByID(%s, %s) -> \"\"\n", id ? id : "-", key ? key : "-");
    zw_iap_make_string(ret, "");
    return ret;
}
static void *hook_InAppBilling_getShopAttributeByName_local(void *ret, const char *name) {
    (void)name;
    zw_iap_make_string(ret, "");
    return ret;
}

static int hook_InAppBilling_GetItemList_local(const char *arg) {
    (void)arg;
    g_zw_iap_list_pending = 1;
    LOG("  v5.05 IAP item list request queued\n");
    return 1;
}

static int hook_InAppBilling_BuyItem_local(const char *id) {
    snprintf(g_zw_iap_buy_id, sizeof(g_zw_iap_buy_id), "%s", id ? id : "");
    g_zw_iap_buy_pending = 1;
    LOGF("  v5.05 IAP buy request queued: %s\n", g_zw_iap_buy_id);
    return 1;
}

static void hook_InAppBilling_sendNotifyConfirmation_local(const char *s) { (void)s; }

static int hook_InAppBilling_GetState_local(void) { return 2; }
static int hook_InAppBilling_getTotalItems_local(void) { return ZW_IAP_N; }

/* Main thread, once per frame, before the game update. */
static void zw_iap_pump(void) {
    if (!g_zw_iap_base) return;
    if (g_zw_iap_list_pending) {
        g_zw_iap_list_pending = 0;
        void (*list_cb)(int, int) = (void (*)(int, int))(g_zw_iap_base + ZW_IAP_OFF_LIST_CB + 1);
        list_cb(0, ZW_IAP_N);
        LOGF("  v5.05 IAP item list delivered (%d packs)\n", ZW_IAP_N);
    }
    if (g_zw_iap_buy_pending) {
        g_zw_iap_buy_pending = 0;
        const zw_iap_pack *p = zw_iap_find(g_zw_iap_buy_id);
        int result = p ? 0 : 1;
        int cost = p ? atoi(p->price) : 0;
        if (p && cost > 0) {
            void *pm = *(void **)(g_zw_iap_base + ZW_IAP_OFF_PLAYERMGR_VAR);
            int (*get_cash)(void *) = (int (*)(void *))(g_zw_iap_base + ZW_IAP_OFF_GETCASH + 1);
            void (*remove_cash)(void *, int) = (void (*)(void *, int))(g_zw_iap_base + ZW_IAP_OFF_REMOVECASH + 1);
            int cash = pm ? get_cash(pm) : -1;
            if (!pm || cash < cost) {
                result = 1;
                LOGF("  v5.05 IAP %s: needs %d cash, have %d -> failed\n", p->id, cost, cash);
            } else {
                remove_cash(pm, cost);   /* saved together with the grant by OnPurchaseSuccess */
                LOGF("  v5.05 IAP %s: paid %d cash (had %d)\n", p->id, cost, cash);
            }
        }
        void (*buy_cb)(const char *, int, const char *) =
            (void (*)(const char *, int, const char *))(g_zw_iap_base + ZW_IAP_OFF_BUY_CB + 1);
        SceUInt64 t0 = sceKernelGetProcessTimeWide();
        uint64_t o_us = g_zw_prof_us[0], w_us = g_zw_wr_us, c_us = g_zw_cl_us;
        unsigned o_n = g_zw_prof_n[0], w_n = g_zw_wr_n, c_n = g_zw_cl_n;
        buy_cb(g_zw_iap_buy_id, result, "");
        LOGF("  v5.05 IAP buy %s -> %s | v5.10 %u ms: fopen %u ms/%u fwrite %u ms/%u fclose %u ms/%u\n",
             g_zw_iap_buy_id, result ? "failed" : "granted", (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000),
             (unsigned)((g_zw_prof_us[0] - o_us) / 1000), g_zw_prof_n[0] - o_n,
             (unsigned)((g_zw_wr_us - w_us) / 1000), g_zw_wr_n - w_n,
             (unsigned)((g_zw_cl_us - c_us) / 1000), g_zw_cl_n - c_n);
    }
}

static void hook_glf_console_noop(void) {
}

static void hook_AndroidOnStartThread(void *arg) {
    (void)arg;
}

static void hook_AndroidOnExitThread(void *arg) {
    (void)arg;
}

static char (*g_glf_alternate_data_dirs)[64] = NULL;

static void hook_glf_fs_SetAlternateDataDir(unsigned int index, const char *path) {
    if (!g_glf_alternate_data_dirs || index >= 8) {
        return;
    }

    char *slot = g_glf_alternate_data_dirs[index];
    const char *src = path ? path : "";
    size_t i = 0;
    for (; i < 63 && src[i]; i++) {
        slot[i] = src[i];
    }
    slot[i] = '\0';
}

static void hook_glf_console_symbols(so_module *mod) {
    static const char *symbols[] = {
        "_ZN3glf7Console5PrintEPKcz",
        "_ZN3glf7Console6VPrintEPKcSt9__va_list",
        "_ZN3glf7Console7PrintlnEPKcz",
        "_ZN3glf7Console8VPrintlnEPKcSt9__va_list",
        "_ZN3glf7Console8PrintRawEPKc",
    };

    int hooked = 0;
    for (unsigned int i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++) {
        uintptr_t sym = so_symbol(mod, symbols[i]);
        if (sym) {
            hook_addr(sym, (uintptr_t)hook_glf_console_noop);
            hooked++;
        }
    }

    if (hooked > 0) {
        so_flush_caches(mod);
    }
    LOGF("  ✓ Hooked %d GLF console print functions to no-op\n", hooked);
}

static void hook_glf_fs_symbols(so_module *mod) {
    uintptr_t dirs_sym = so_symbol(mod, "_ZN3glf2fs18gAlternateDataDirsE");
    if (dirs_sym) {
        g_glf_alternate_data_dirs = (char (*)[64])dirs_sym;
        LOGF("  ✓ glf::fs::gAlternateDataDirs at 0x%08X\n", (unsigned)dirs_sym);
    } else {
        LOG("  ! glf::fs::gAlternateDataDirs not found\n");
    }

    uintptr_t set_dir_sym = so_symbol(mod, "_ZN3glf2fs19SetAlternateDataDirEjPKc");
    if (set_dir_sym) {
        hook_addr(set_dir_sym, (uintptr_t)hook_glf_fs_SetAlternateDataDir);
        so_flush_caches(mod);
        LOGF("  ✓ Hooked glf::fs::SetAlternateDataDir at 0x%08X\n",
             (unsigned)set_dir_sym);
    } else {
        LOG("  ! glf::fs::SetAlternateDataDir symbol not found\n");
    }
}

/* v4.89: Vox streams sounds out of main.obb through vox::openGLF (DecoderMSWav ->
 * StreamCFileCursor -> FileSystemInterface::_OpenFile -> openGLF -> glf::FileStream).
 * With no audio backend no voice ever finishes, so every footstep/shot leaked a stdio
 * handle on main.obb until fopen failed with EMFILE (~60 open) and the game crashed.
 * openGLF already returns NULL on a failed open, so refuse all Vox file opens until
 * real audio exists (ZW_AUDIO_FILES 1). */
static void *hook_vox_openGLF(const char *path, int mode) {
    static unsigned n;
    if (n++ < 8) LOGF("  v4.89 vox::openGLF('%s',%d) refused (no audio)\n", path ? path : "(null)", mode);
    return NULL;
}

static void hook_glf_thread_symbols(so_module *mod) {
    int hooked = 0;

#ifndef ZW_AUDIO_FILES
#define ZW_AUDIO_FILES 1
#endif
    if (!ZW_AUDIO_FILES) {
        uintptr_t vox_sym = so_symbol(mod, "_ZN3vox7openGLFEPKcNS_17VoxFileAccessModeE");
        if (vox_sym) { hook_addr(vox_sym, (uintptr_t)hook_vox_openGLF); LOG("  v4.89 hooked vox::openGLF\n"); }
        else LOG("  ! vox::openGLF symbol not found\n");
    }

    uintptr_t start_sym = so_symbol(mod, "_ZN3glf20AndroidOnStartThreadEPv");
    if (start_sym) {
        hook_addr(start_sym, (uintptr_t)hook_AndroidOnStartThread);
        hooked++;
        LOGF("  ✓ Hooked glf::AndroidOnStartThread at 0x%08X\n",
             (unsigned)start_sym);
    } else {
        LOG("  ! glf::AndroidOnStartThread symbol not found\n");
    }

    uintptr_t exit_sym = so_symbol(mod, "_ZN3glf19AndroidOnExitThreadEPv");
    if (exit_sym) {
        hook_addr(exit_sym, (uintptr_t)hook_AndroidOnExitThread);
        hooked++;
        LOGF("  ✓ Hooked glf::AndroidOnExitThread at 0x%08X\n",
             (unsigned)exit_sym);
    } else {
        LOG("  ! glf::AndroidOnExitThread symbol not found\n");
    }

    if (hooked > 0) {
        so_flush_caches(mod);
    }
}

/* v4.77: palm-tree camera fade investigation.
 * Chain in the game: CContactListener (Box2D) -> CActorsCollisionComponent::
 * BeginCollision/EndCollision -> event 0xF/0x10 to listeners ->
 * CTriggerComponent::OnEvent -> CMeshComponent::SwitchFadeInInstant on the
 * trigger's linked objects. These are C re-implementations of the small
 * functions (hook_addr has no trampoline) with logging. */
static uintptr_t g_zw_so_base;
static unsigned g_zw_begin_n, g_zw_end_n, g_zw_trig_ev_n, g_zw_trig_match_n, g_zw_fade_n;
#define ZW_SO_FN(off) (g_zw_so_base + (off) + 1)
typedef struct { void *owner; void *other; float pos[3]; } zw_coll_event;
static void hook_ActorsCollision_BeginCollision(void *self, void *other, const float *pos) {
    g_zw_begin_n++;
    if (!*((uint8_t *)self + 0xc)) return;
    zw_coll_event ev = { *(void **)((uintptr_t)other + 4), other, { pos[0], pos[1], pos[2] } };
    static int n;
    if (n < 40) { n++; LOGF("  v4.77 BeginCollision self=%p owner=%p other_owner=%p listeners=%d pos=%.0f,%.0f,%.0f\n",
                            self, *(void **)((uintptr_t)self + 4), ev.owner, *(int *)((uintptr_t)self + 0x48), pos[0], pos[1], pos[2]); }
    ((void (*)(void *, int, const void *))ZW_SO_FN(0x3e08c8))((uint8_t *)self + 0x18, 0xF, &ev);
}
static void hook_ActorsCollision_EndCollision(void *self, void *other) {
    g_zw_end_n++;
    if (!*((uint8_t *)self + 0xc)) return;
    zw_coll_event ev = { *(void **)((uintptr_t)other + 4), other, { 0, 0, 0 } };
    ((void (*)(void *, int, const void *))ZW_SO_FN(0x3e08c8))((uint8_t *)self + 0x18, 0x10, &ev);
}
static void hook_CTriggerComponent_OnEvent(void *self, int ev, void **arg) {
    uintptr_t t = (uintptr_t)self;
    if (ev != 0xF && ev != 0x10) return;
    g_zw_trig_ev_n++;
    void *target = *(void **)(t + 0x18);
    if (ev == 0xF) {
        int *data = *(int **)(t + 0x14);
        if (data[3] < 0) {
            void *zg = (g_ZombiesGame_singleton_ptr) ? *g_ZombiesGame_singleton_ptr : NULL;
            void *lvl = zg ? *(void **)((uintptr_t)zg + 0x1c4) : NULL;
            if (lvl) { target = *(void **)(*(uintptr_t *)((uintptr_t)lvl + 0x2ac) + 4); *(void **)(t + 0x18) = target; }
        }
    }
    void *l1 = *(void **)(t + 0x1c), *l2 = *(void **)(t + 0x20);
    static int n;
    if (n < 60) { n++; LOGF("  v4.77 Trigger::OnEvent self=%p ev=0x%x obj=%p target=%p linked=%p,%p data(tid=%d fade=%d ids=%d,%d)\n",
                            self, ev, arg ? *arg : NULL, target, l1, l2, (*(int **)(t + 0x14))[3],
                            *((uint8_t *)*(void **)(t + 0x14) + 0x10), (*(int **)(t + 0x14))[5], (*(int **)(t + 0x14))[6]); }
    if (!arg || *arg != target) return;
    g_zw_trig_match_n++;
    int on = (ev == 0xF);
    ((void (*)(void *, int))ZW_SO_FN(0x5354c4))(self, on ? 3 : 2);          /* DoAction */
    *((uint8_t *)t + 0x38) = (uint8_t)on;
    if (l1) { g_zw_fade_n++; ((void (*)(void *, int))ZW_SO_FN(0x52e4b8))(*(void **)((uintptr_t)l1 + 0x94), on); }
    if (l2) { g_zw_fade_n++; ((void (*)(void *, int))ZW_SO_FN(0x52e4b8))(*(void **)((uintptr_t)l2 + 0x94), on); }
}
#define ZW_TRIGGER_PROBES 0
/* v4.80: physical analog sticks.
 * The game's virtual sticks live in CGameUI::UpdateJoypad / UpdateTrigger:
 *   player = level->[0x2ac] (CPlayer); joystick touch: active byte +0x21,
 *   origin +0x28/+0x2c, current +0x30/+0x34; trigger touch: +0x38, +0x40..+0x4c.
 *   Result = (clamped current - origin) / 60 px, written to level+0x200/0x204
 *   (move) and level+0x208/0x20c (aim + fire).
 * These hooks re-implement both functions and feed the Vita sticks in when
 * they are deflected; touch input keeps working exactly as before. */
static volatile float g_zw_stick[4];   /* lx, ly, rx, ry in -1..1 (y down), dead zone applied */
static void zw_ui_stick(void *ui, int is_trigger) {
    void *zg = g_ZombiesGame_singleton_ptr ? *g_ZombiesGame_singleton_ptr : NULL;
    if (!zg) return;
    uintptr_t lvl = *(uintptr_t *)((uintptr_t)zg + 0x1c4);
    if (!lvl) return;
    uintptr_t pl = *(uintptr_t *)(lvl + 0x2ac);
    float *out = (float *)(lvl + (is_trigger ? 0x208 : 0x200));
    if (!pl) { out[0] = out[1] = 0.0f; return; }
    const float *o = (const float *)(pl + (is_trigger ? 0x40 : 0x28));   /* origin xy, current xy */
    float ox = o[0], oy = o[1], cx = o[2], cy = o[3];
    int touch_active = *(uint8_t *)(pl + (is_trigger ? 0x38 : 0x21));
    int enabled = ((int (*)(void *))ZW_SO_FN(is_trigger ? 0x42d1c0 : 0x42d0a4))(ui) & 0xff;   /* TriggerEnabled / JoypadEnabled */
    float sx = g_zw_stick[is_trigger ? 2 : 0], sy = g_zw_stick[is_trigger ? 3 : 1];
    int stick_active = (sx != 0.0f || sy != 0.0f);
    int controls = ((int (*)(void *))ZW_SO_FN(0x5527c0))((void *)pl) & 0xff;                /* CPlayer::IsControlsEnabled */
    {   /* v4.81 diag: say what the stick sees (first deflections + every 5 s while deflected) */
        static unsigned calls[2], last[2]; static int n[2];
        calls[is_trigger]++;
        if (stick_active && (n[is_trigger] < 6 || calls[is_trigger] - last[is_trigger] > 300)) {
            n[is_trigger]++; last[is_trigger] = calls[is_trigger];
            LOGF("  v4.81 %s stick %.2f,%.2f: hud_visible=%d touch=%d controls=%d out(prev)=%.2f,%.2f\n",
                 is_trigger ? "right" : "left", sx, sy, enabled, touch_active, controls, out[0], out[1]);
        }
    }
    /* v4.81: the HUD stick clip is only visible while a finger is down, so the
     * physical stick must not depend on it - only touch input does. */
    if ((!touch_active && !stick_active) || (touch_active && !enabled) || !controls) {
        out[0] = out[1] = 0.0f;
        return;
    }
    if (stick_active && !touch_active) {
        static int n[2];
        if (n[is_trigger] < 4) { n[is_trigger]++; LOGF("  v4.80 %s stick -> game %.2f,%.2f\n", is_trigger ? "right(aim)" : "left(move)", sx, sy); }
        out[0] = sx; out[1] = sy; return;
    }
    float dx = cx - ox, dy = cy - oy, len2 = dx * dx + dy * dy;
    if (len2 > 60.0f * 60.0f) { float k = 60.0f / sqrtf(len2); dx *= k; dy *= k; }
    void *fm = *(void **)(*(void **)(g_zw_so_base + 0xcd2ad8));                            /* FlashManager singleton */
    ((void (*)(void *, const char *, int, int))ZW_SO_FN(0x4decf4))(fm, is_trigger ? "menu_Hud.myTrigger" : "menu_Hud.myJoystick",
                                                                  (int)(ox + dx), (int)(oy + dy));   /* FlashManager::SetPosition */
    out[0] = dx / 60.0f; out[1] = dy / 60.0f;
}
static void hook_CGameUI_UpdateJoypad(void *ui)  { zw_ui_stick(ui, 0); }
static void hook_CGameUI_UpdateTrigger(void *ui) { zw_ui_stick(ui, 1); }
static void zw_read_sticks(const SceCtrlData *pad) {
    const uint8_t raw[4] = { pad->lx, pad->ly, pad->rx, pad->ry };
    for (int s = 0; s < 2; s++) {
        float x = (raw[s * 2] - 128) / 127.0f, y = (raw[s * 2 + 1] - 128) / 127.0f;
        float len = sqrtf(x * x + y * y);
        if (len < 0.25f) { x = y = 0.0f; }
        else { float k = (len > 1.0f ? 1.0f : (len - 0.25f) / 0.75f) / len; x *= k; y *= k; if (x == 0.0f && y == 0.0f) x = 0.001f; }
        g_zw_stick[s * 2] = x; g_zw_stick[s * 2 + 1] = y;
    }
}
/* v4.82: the HUD SWF reports its virtual sticks to an unnamed GSLevel handler
 * (.so 0x5cd4b0) which stores move -> level+0x200/0x204 and aim -> level+0x208/
 * 0x20c, but only when a touch event arrives. Nothing rewrites them in between,
 * so the physical sticks are written straight into the same fields each frame
 * and zeroed once on release (touch input is left alone otherwise). */
static int zw_ptr_ok(const void *p);
static void zw_apply_sticks(void) {
    static int was[2];
    void *zg = g_ZombiesGame_singleton_ptr ? *g_ZombiesGame_singleton_ptr : NULL;
    if (!zw_ptr_ok(zg)) return;
    void *lvl = *(void **)((uintptr_t)zg + 0x1c4);
    if (!zw_ptr_ok(lvl)) { was[0] = was[1] = 0; return; }
    for (int s = 0; s < 2; s++) {
        float *out = (float *)((uintptr_t)lvl + 0x200 + s * 8);
        /* v4.83: CPlayer (level+0x2ac) only honours the vectors while its "stick held"
         * flags are set: +0x21 joystick, +0x38 trigger (CPlayer::UpdateOrientation). */
        uint8_t *pl = *(uint8_t **)((uintptr_t)lvl + 0x2ac);
        uint8_t *held = zw_ptr_ok(pl) ? pl + (s ? 0x38 : 0x21) : NULL;
        float x = g_zw_stick[s * 2], y = g_zw_stick[s * 2 + 1];
        int active = (x != 0.0f || y != 0.0f);
        if (active) {
            static int n[2];
            if (n[s] < 4) { n[s]++; LOGF("  v4.82 %s stick %.2f,%.2f -> level %p (was %.2f,%.2f)\n", s ? "right(aim)" : "left(move)", x, y, lvl, out[0], out[1]); }
            out[0] = x; out[1] = y;
            if (held) *held = 1;
        } else if (was[s] > 0) {
            /* v4.84: firing only stops while the trigger is still "held" with a zero
             * vector (UpdateOrientation -> EndFiring), so hold it 6 more frames. */
            out[0] = out[1] = 0.0f;
            if (held) *held = (was[s] > 1);
        }
        was[s] = active ? 7 : (was[s] > 0 ? was[s] - 1 : 0);
    }
}
/* v5.08: physical controls for the gameplay HUD.
 * 1. On-screen sticks hidden: the game's own Xperia Play path, Application::XperiaToggleJoysticks
 *    (C wrapper so+0x3564d0), sends menu_Hud.myJoystick / myTrigger to another frame. It acts
 *    only while the top state is GSLevel, so it is re-sent every 30 frames (cheap: a strcmp and
 *    4 GotoFrame calls) to cover level loads and pause menus.
 * 2. Power-ups: Square / Triangle / Circle = slots 0 / 1 / 2. Same sequence as the HUD tap
 *    handler (so+0x5d211c): if CPlayer::IsControlsEnabled and !HasActiveConsumable ->
 *    CLevel::UsePowerup(i), CLevel::SetPowerupUIText(i). UsePowerup bounds-checks i itself. */
#ifndef ZW_HIDE_STICKS
#define ZW_HIDE_STICKS 1
#endif
#ifndef ZW_HIDE_STICKS_ARG
#define ZW_HIDE_STICKS_ARG 1
#endif
static void *g_zw_app_impl;   /* Application* (glf impl), set at startup */
static void zw_hud_buttons(SceUInt32 pressed) {
    /* v5.11: START = pause / resume during a level (GSLevel only; START does nothing in the
     * menus). Back is sent as a phone would: Application::OnAndroidKeyEvent(app, up=false, 4)
     * (so+0x4d1d10) -> CKeyManager::OnEvent, the game's own Back handling. */
    if ((pressed & SCE_CTRL_START) && zw_ptr_ok(g_zw_app_impl) && g_zw_so_base &&
        (uintptr_t)g_zw_cur_vtable == g_zw_so_base + 0xc9c068 && zw_ptr_ok((void *)g_zw_cur_state)) {
        /* - a Settings popup open (pause-menu Settings is Popups.inGameSound, the main
         *   Options is Popups.Options): send Back; the game's own CKeyManager presses its
         *   Close / back button
         * - pause menu (GSLevel+0x179, IsInPauseMenu): press the menu's own Resume button, as
         *   CKeyManager does for gamepads (Back would press Quit there)
         * - playing: send Back, which opens the pause menu */
        void *fm = *(void **)(*(void **)(g_zw_so_base + 0xcd2ad8));
        int (*is_visible)(void *, const char *) = (void *)ZW_SO_FN(0x4df164);
        int paused = *((volatile uint8_t *)g_zw_cur_state + 0x179);
        int settings = zw_ptr_ok(fm) && (is_visible(fm, "Popups.inGameSound") || is_visible(fm, "Popups.Options"));
        if (paused && !settings) {
            void (*invoke)(void *, const char *, const char *) = (void *)ZW_SO_FN(0x4dc144);
            invoke(fm, "_root.menu_Ingame.ResumeBTN", "OnRelease");
        } else {
            int (*key_event)(void *, int, int) = (void *)ZW_SO_FN(0x4d1d10);
            key_event(g_zw_app_impl, 0, 4);
        }
    }
    void *zg = g_ZombiesGame_singleton_ptr ? *g_ZombiesGame_singleton_ptr : NULL;
    if (!zw_ptr_ok(zg)) return;
    void *lvl = *(void **)((uintptr_t)zg + 0x1c4);
    if (!zw_ptr_ok(lvl)) return;
    if (ZW_HIDE_STICKS && (g_main_loop_frame % 30) == 0) {
        /* v5.08: Options > Notifications > "Inactivity" (the last switch, BTN_06) is a push-
         * notification setting with no use on the Vita. It now chooses stick visibility:
         * ON = on-screen sticks shown, OFF = hidden. The game stores it itself as bit 5 of
         * CNotificationManager+0x34 (handler so+0x44f2fe) and saves it with CSaveManager::Save. */
        uint8_t *nm = *(uint8_t **)(*(void **)(g_zw_so_base + 0xcd2ac4));
        int show = zw_ptr_ok(nm) && ((*(uint32_t *)(nm + 0x34) >> 5) & 1);
        void (*toggle)(int) = (void *)ZW_SO_FN(0x3564d0);
        toggle(show ? !ZW_HIDE_STICKS_ARG : ZW_HIDE_STICKS_ARG);
    }
    static const SceUInt32 btn[3] = { SCE_CTRL_SQUARE, SCE_CTRL_TRIANGLE, SCE_CTRL_CIRCLE };
    for (int i = 0; i < 3; i++) {
        if (!(pressed & btn[i])) continue;
        void *pl = *(void **)((uintptr_t)lvl + 0x2ac);
        if (!zw_ptr_ok(pl)) return;
        int (*controls_on)(void *) = (void *)ZW_SO_FN(0x5527c0);
        int (*has_active)(void *) = (void *)ZW_SO_FN(0x551b58);
        int ok = controls_on(pl) && !has_active(pl);
        LOGF("  v5.08 power-up button %d: %s\n", i, ok ? "use" : "ignored (controls off or one active)");
        if (!ok) return;
        void (*use)(void *, int) = (void *)ZW_SO_FN(0x418104);
        void (*ui_text)(void *, int) = (void *)ZW_SO_FN(0x4176b0);
        use(lvl, i);
        ui_text(lvl, i);
        return;
    }
}
/* v5.09: button badges (Square / Triangle / Circle) drawn on the three power-up icons.
 * The icons are the Flash clips menu_Hud.powerups.buyitem1..3. Every 6 frames the loader
 * asks the game's FlashManager whether each is visible and where it is (FindCharacter +
 * CharacterHandle::getWorldBound, as GSShop::Update does); every frame it draws the badges
 * at the lower-right corner of each visible icon, after the game's frame and before the swap.
 * glitch caches GL state, so every piece of GL state touched here is read first and restored. */
#include "hud_badges.h"
#ifndef ZW_POWERUP_BADGES
#define ZW_POWERUP_BADGES 1
#endif
static GLuint g_zw_badge_tex[3], g_zw_badge_prog;
static GLint g_zw_badge_u_tex;
static int g_zw_badge_state;   /* 0 = not set up, 1 = ready, -1 = failed */
static float g_zw_badge_rect[3][4];
static int g_zw_badge_vis[3];
static void zw_badges_setup(void) {
    static const char *vs =
        "attribute vec2 aPos;\n"
        "attribute vec2 aUV;\n"
        "varying vec2 vUV;\n"
        "void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
    static const char *fs =
        "precision mediump float;\n"
        "varying vec2 vUV;\n"
        "uniform sampler2D uTex;\n"
        "void main() { gl_FragColor = texture2D(uTex, vUV); }\n";
    GLint prev_tex = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGenTextures(3, g_zw_badge_tex);
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, g_zw_badge_tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_zw_badge_rgba[i]);
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 1, &vs, NULL); glCompileShader(v);
    glShaderSource(f, 1, &fs, NULL); glCompileShader(f);
    g_zw_badge_prog = glCreateProgram();
    glAttachShader(g_zw_badge_prog, v); glAttachShader(g_zw_badge_prog, f);
    glBindAttribLocation(g_zw_badge_prog, 0, "aPos");
    glBindAttribLocation(g_zw_badge_prog, 1, "aUV");
    glLinkProgram(g_zw_badge_prog);
    GLint ok = 0; glGetProgramiv(g_zw_badge_prog, GL_LINK_STATUS, &ok);
    g_zw_badge_u_tex = glGetUniformLocation(g_zw_badge_prog, "uTex");
    g_zw_badge_state = ok ? 1 : -1;
    LOGF("  v5.09 power-up badges: %s (prog %u)\n", ok ? "ready" : "shader link FAILED", (unsigned)g_zw_badge_prog);
}
/* query the three icon clips (every 6 frames: each lookup walks the Flash path) */
static void zw_badges_query(void) {
    void *fm = *(void **)(*(void **)(g_zw_so_base + 0xcd2ad8));
    if (!zw_ptr_ok(fm)) { g_zw_badge_vis[0] = g_zw_badge_vis[1] = g_zw_badge_vis[2] = 0; return; }
    int (*is_visible)(void *, const char *) = (void *)ZW_SO_FN(0x4df164);
    void (*handle_ctor)(void *, void *) = (void *)ZW_SO_FN(0x77c8ec);
    void (*handle_dtor)(void *) = (void *)ZW_SO_FN(0x77c6bc);
    void (*find)(void *, void *, const char *, void *) = (void *)ZW_SO_FN(0x4df370);
    void (*world_bound)(void *, float *) = (void *)ZW_SO_FN(0x77da0c);
    int (*valid)(void *) = (void *)ZW_SO_FN(0x77ca38);
    int panel = is_visible(fm, "menu_Hud") && is_visible(fm, "menu_Hud.powerups");
    {   /* hide while paused or in cutscenes: same test the power-up tap handler uses */
        void *zg = g_ZombiesGame_singleton_ptr ? *g_ZombiesGame_singleton_ptr : NULL;
        void *lvl = zw_ptr_ok(zg) ? *(void **)((uintptr_t)zg + 0x1c4) : NULL;
        void *pl = zw_ptr_ok(lvl) ? *(void **)((uintptr_t)lvl + 0x2ac) : NULL;
        int (*controls_on)(void *) = (void *)ZW_SO_FN(0x5527c0);
        if (!zw_ptr_ok(pl) || !controls_on(pl)) panel = 0;
    }
    for (int i = 0; i < 3; i++) {
        char path[48];
        snprintf(path, sizeof(path), "menu_Hud.powerups.buyitem%d", i + 1);
        g_zw_badge_vis[i] = 0;
        if (!panel || !is_visible(fm, path)) continue;
        uint32_t root[16], h[16];   /* gameswf::CharacterHandle (0x24 bytes), with room to spare */
        float r[4] = { 0, 0, 0, 0 };
        handle_ctor(root, NULL);
        find(h, fm, path, root);   /* the by-value handle is passed by address (non-trivial copy) */
        handle_dtor(root);
        if (valid(h)) { world_bound(h, r); g_zw_badge_vis[i] = r[1] > r[0] && r[3] > r[2]; }
        handle_dtor(h);
        memcpy(g_zw_badge_rect[i], r, sizeof(r));
        static int logged;
        if (logged < 6 && g_zw_badge_vis[i]) { logged++; LOGF("  v5.09 badge %d rect x %.1f..%.1f y %.1f..%.1f\n", i, r[0], r[1], r[2], r[3]); }
    }
}
/* v5.09: relabel Options > Notifications > last switch (now the on-screen stick switch, see
 * zw_hud_buttons). The text comes from the language pack via FlashManager::ProcessLocalization,
 * which can rewrite it whenever the popup opens, so while the Options popup is visible the
 * label is set again every 10 frames (one Flash path lookup each time). */
static void zw_options_frame(int frame) {
    if (!g_zw_so_base || (frame % 10) != 3) return;
    void *fm = *(void **)(*(void **)(g_zw_so_base + 0xcd2ad8));
    if (!zw_ptr_ok(fm)) return;
    int (*is_visible)(void *, const char *) = (void *)ZW_SO_FN(0x4df164);
    if (!is_visible(fm, "Popups.Options")) return;
    void (*set_text)(void *, const char *, const char *) = (void *)ZW_SO_FN(0x4de9a4);
    set_text(fm, "Popups.Options.Fade.NotificationPage.BTN_06.text", "SHOW STICKS");
    static int logged;
    if (!logged) { logged = 1; LOG("  v5.09 options: stick switch relabelled\n"); }
}
/* v5.15: split in two. zw_badges_frame runs on the game thread (it asks the game what is visible);
 * zw_badges_gl does the drawing. */
static int zw_badges_frame(int frame) {
    zw_options_frame(frame);
    if (!ZW_POWERUP_BADGES || !g_zw_so_base) return 0;
    if ((uintptr_t)g_zw_cur_vtable != g_zw_so_base + 0xc9c068) return 0;   /* GSLevel only */
    if ((frame % 6) == 0) zw_badges_query();
    if (!g_zw_badge_vis[0] && !g_zw_badge_vis[1] && !g_zw_badge_vis[2]) return 0;
    return g_zw_badge_state <= 1;
}
static void zw_badges_gl(uintptr_t vis_mask) {
    if (g_zw_badge_state == 0) zw_badges_setup();
    if (g_zw_badge_state != 1) return;

    /* save */
    GLint prog, atex, tex, abuf, vp[4], bsrc_rgb, bdst_rgb, bsrc_a, bdst_a, beq_rgb, beq_a, en[2];
    GLboolean blend, depth, cull, scissor, stencil, dmask, cmask[4];
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &atex);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc_rgb); glGetIntegerv(GL_BLEND_DST_RGB, &bdst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsrc_a); glGetIntegerv(GL_BLEND_DST_ALPHA, &bdst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &beq_rgb); glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &beq_a);
    blend = glIsEnabled(GL_BLEND); depth = glIsEnabled(GL_DEPTH_TEST); cull = glIsEnabled(GL_CULL_FACE);
    scissor = glIsEnabled(GL_SCISSOR_TEST); stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask); glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en[0]);
    glGetVertexAttribiv(1, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en[1]);

    /* draw */
    glViewport(0, 0, 960, 544);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    glDepthMask(GL_FALSE); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(g_zw_badge_prog);
    glUniform1i(g_zw_badge_u_tex, 0);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    for (int i = 0; i < 3; i++) {
        if (!(vis_mask & (1u << i))) continue;
        /* The Flash bounds include invisible parts and move with the icons' idle animation, so
         * they only decide visibility. The HUD layout is fixed at 960x544; badge centres sit on
         * the inner corner of each icon (measured from an on-device screenshot, v5.09).
         */
        static const float ctr[3][2] = { { 108.0f, 178.0f }, { 108.0f, 290.0f }, { 884.0f, 176.0f } };
        const float sz = 36.0f, cx = ctr[i][0], cy = ctr[i][1];
        float x0 = cx - sz * 0.5f, y0 = cy - sz * 0.5f, x1 = x0 + sz, y1 = y0 + sz;
        float X0 = x0 / 480.0f - 1.0f, X1 = x1 / 480.0f - 1.0f, Y0 = 1.0f - y0 / 272.0f, Y1 = 1.0f - y1 / 272.0f;
        float pos[8] = { X0, Y0, X1, Y0, X0, Y1, X1, Y1 };
        static const float uv[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
        glBindTexture(GL_TEXTURE_2D, g_zw_badge_tex[i]);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    /* restore */
    if (!en[0]) glDisableVertexAttribArray(0);
    if (!en[1]) glDisableVertexAttribArray(1);
    glUseProgram((GLuint)prog);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)abuf);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    glActiveTexture((GLenum)atex);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glBlendEquationSeparate((GLenum)beq_rgb, (GLenum)beq_a);
    glBlendFuncSeparate((GLenum)bsrc_rgb, (GLenum)bdst_rgb, (GLenum)bsrc_a, (GLenum)bdst_a);
    if (!blend) glDisable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (stencil) glEnable(GL_STENCIL_TEST);
    glDepthMask(dmask); glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
}
static void hook_trigger_fade(so_module *mod) {
    uintptr_t on_event = so_symbol(mod, "_ZN17CTriggerComponent7OnEventEiPKv");
    if (!on_event) { LOG("  ! v4.77 CTriggerComponent::OnEvent not found\n"); return; }
    g_zw_so_base = (on_event & ~1u) - 0x535554;
    if (ZW_TRIGGER_PROBES) {
        hook_addr(on_event, (uintptr_t)hook_CTriggerComponent_OnEvent);
        hook_addr(ZW_SO_FN(0x3e0eb8), (uintptr_t)hook_ActorsCollision_BeginCollision);
        hook_addr(ZW_SO_FN(0x3e0ee0), (uintptr_t)hook_ActorsCollision_EndCollision);
        LOGF("  v4.77 trigger/fade probes installed, so base=0x%08X\n", (unsigned)g_zw_so_base);
    }
    uintptr_t upd_joy = so_symbol(mod, "_ZN7CGameUI12UpdateJoypadEv");
    uintptr_t upd_trg = so_symbol(mod, "_ZN7CGameUI13UpdateTriggerEv");
    if (0 && upd_joy && upd_trg) {   /* v4.82: these two only run from CPlayer::EnableControls, not per frame - see zw_apply_sticks */
        hook_addr(upd_joy, (uintptr_t)hook_CGameUI_UpdateJoypad);
        hook_addr(upd_trg, (uintptr_t)hook_CGameUI_UpdateTrigger);
        LOG("  v4.80 analog sticks wired: left = move (CGameUI::UpdateJoypad), right = aim/fire (CGameUI::UpdateTrigger)\n");
    } else {
        LOG("  ! v4.80 CGameUI::UpdateJoypad/UpdateTrigger not found - sticks not wired\n");
    }
}

static void hook_zombies_music_symbols(so_module *mod) {
    int hooked = 0;
    /* v5.00: audio output works (audio_sl.c), so music is no longer suppressed.
     * Set ZW_MUSIC 0 to silence it again. */
#ifndef ZW_MUSIC
#define ZW_MUSIC 1
#endif
    if (ZW_MUSIC) { LOG("  v5.00 music enabled (PlayBackgroundMusic/PlayMusic not hooked)\n"); return; }

    uintptr_t bgm_sym = so_symbol(mod, "_ZN11ZombiesGame19PlayBackgroundMusicEv");
    if (bgm_sym) {
        hook_addr(bgm_sym, (uintptr_t)hook_ZombiesGame_PlayBackgroundMusic);
        hooked++;
        LOGF("  ✓ Hooked ZombiesGame::PlayBackgroundMusic at 0x%08X\n",
             (unsigned)bgm_sym);
    } else {
        LOG("  ! ZombiesGame::PlayBackgroundMusic symbol not found\n");
    }

    uintptr_t play_music_sym = so_symbol(mod, "_ZN11ZombiesGame9PlayMusicEPKc");
    if (play_music_sym) {
        hook_addr(play_music_sym, (uintptr_t)hook_ZombiesGame_PlayMusic);
        hooked++;
        LOGF("  ✓ Hooked ZombiesGame::PlayMusic at 0x%08X\n",
             (unsigned)play_music_sym);
    } else {
        LOG("  ! ZombiesGame::PlayMusic symbol not found\n");
    }

    if (hooked > 0) {
        so_flush_caches(mod);
    }
}

static void hook_application_loop_symbols(so_module *mod) {
    int hooked = 0;

    g_Application_Update_once =
        (void (*)(void *))so_symbol(mod, "_ZN11Application6UpdateEv");
    LOGF("  Application::Update one-tick target = %p\n",
         (void *)g_Application_Update_once);

    g_Application_SetupDevice_once =
        (void (*)(void *))so_symbol(mod, "_ZN11Application11SetupDeviceEv");
    LOGF("  Application::SetupDevice target = %p\n",
         (void *)g_Application_SetupDevice_once);

    g_GlfApp_global_slot = (void **)so_symbol(mod, "_ZN3glf4gAppE");
    LOGF("  glf::gApp slot = %p\n", (void *)g_GlfApp_global_slot);

    uintptr_t android_update_sym = so_symbol(mod, "_ZN3glf13AndroidUpdateEv");
    LOGF("  glf::AndroidUpdate probe target left original at 0x%08X\n",
         (unsigned)android_update_sym);

    g_GlfApp_Update_once =
        (void (*)(void *))so_symbol(mod, "_ZN3glf3App6UpdateEv");
    LOGF("  glf::App::Update one-tick target = %p\n",
         (void *)g_GlfApp_Update_once);

    g_android_operator_new =
        (void *(*)(unsigned int))so_symbol(mod, "_Znwj");
    g_CGlfDevice_ctor =
        (void (*)(void *, void *))so_symbol(mod, "_ZN6glitch10CGlfDeviceC1EPN3glf3AppE");
    if (!g_CGlfDevice_ctor) {
        g_CGlfDevice_ctor =
            (void (*)(void *, void *))so_symbol(mod, "_ZN6glitch10CGlfDeviceC2EPN3glf3AppE");
    }
    uintptr_t g_device_sym = so_symbol(mod, "g_device");
    if (g_device_sym) {
        g_device_ptr = (void **)g_device_sym;
        LOGF("  glitch device helpers: g_device global at %p\n", g_device_ptr);
    } else {
        LOG("  glitch device helpers: g_device symbol not found\n");
    }
    uintptr_t zombies_singleton_sym =
        so_symbol(mod, "_ZN13SingletonFastI11ZombiesGameE10s_instanceE");
    if (zombies_singleton_sym) {
        g_ZombiesGame_singleton_ptr = (void **)zombies_singleton_sym;
        LOGF("  ZombiesGame singleton slot at %p initial=%p\n",
             (void *)g_ZombiesGame_singleton_ptr,
             g_ZombiesGame_singleton_ptr ? *g_ZombiesGame_singleton_ptr : NULL);
    } else {
        LOG("  ZombiesGame singleton symbol not found\n");
    }
    LOGF("  glitch device helpers: operator_new=%p CGlfDevice_ctor=%p\n",
         (void *)g_android_operator_new, (void *)g_CGlfDevice_ctor);

    uintptr_t create_device_sym = so_symbol(mod, "_ZN6glitch14createDeviceExEPN3glf3AppE");
    if (create_device_sym) {
        hook_addr(create_device_sym, (uintptr_t)hook_glitch_createDeviceEx_nonblocking);
        hooked++;
        LOGF("  ✓ Hooked glitch::createDeviceEx nonblocking at 0x%08X\n",
             (unsigned)create_device_sym);
    } else {
        LOG("  ! glitch::createDeviceEx symbol not found\n");
    }

    uintptr_t device_run_sym = so_symbol(mod, "_ZN6glitch7IDevice3runEv");
    if (device_run_sym) {
        hook_addr(device_run_sym, (uintptr_t)hook_glitch_IDevice_run_noop);
        hooked++;
        LOGF("  ✓ Hooked glitch::IDevice::run no-op at 0x%08X\n",
             (unsigned)device_run_sym);
    } else {
        LOG("  ! glitch::IDevice::run symbol not found\n");
    }

    uintptr_t create_driver_sym = so_symbol(mod, "_ZN6glitch10CGlfDevice12createDriverEv");
    LOGF("  CGlfDevice::createDriver left original at 0x%08X\n",
         (unsigned)create_driver_sym);

    uintptr_t first_time_init_sym = so_symbol(mod, "_ZN11ZombiesGame13FirstTimeInitEv");
    if (first_time_init_sym) {
        LOGF("  ZombiesGame::FirstTimeInit left original at 0x%08X\n",
             (unsigned)first_time_init_sym);
    } else {
        LOG("  ! ZombiesGame::FirstTimeInit symbol not found\n");
    }

    g_zw_text_base = mod->text_base;
    g_zw_mod = mod;
    g_GSLoading_Update_sym = so_symbol(mod, "_ZN9GSLoading6UpdateEP11Applicationj");
    LOGF("  GSLoading::Update target = 0x%08X\n",
         (unsigned)g_GSLoading_Update_sym);

    /*
     * v4.20: Surgical branch patch in GSLoading::Update.
     *
     * At ELF offset 0x5a153e (+0xD2 from function start), there is:
     *   beq.w 5a194a   (f000 8204)  -- branches only if state+0x11c == 0
     *
     * Despite forcing state+0x11c=0 before every Update call, CMenu::Loading
     * at 0x5a195e was never reached after thousands of frames. The most
     * likely cause: something inside Update resets 0x11c to 1 before the
     * ldrb at 0x5a1538. Changing the conditional branch to an unconditional
     * b.w with the same target (T4 encoding: f000 ba04) guarantees we always
     * reach CMenu::Loading regardless of 0x11c.
     */
    if (g_GSLoading_Update_sym) {
        /*
         * v4.48: v4.20 beq->b.w patch DISABLED. Its premise (0x11c reset to 1
         * before the check) was our own field forcing fighting the game. With
         * the real CMenu::Loading working (v4.44+), the unconditional branch
         * re-ran the load-complete block every frame (CheckForDLC +
         * OnLoadMenuEnd each frame, ~6 fps) and never let Update reach the
         * post-load path at 0x5a17a2 (state+0x11c=1, +0x11d=0).
         */

        /*
         * v4.21: When ZombiesGame+0x1b0 is 0, Update calls
         * FlashManager::SWFLoadSetVisible and then jumps back to 0x5a1500,
         * where ZombiesGame+0x120 can send it through another wait path before
         * our 0x11c branch is reached. Route that return to 0x5a1536 instead:
         * it reloads r4=state and then falls into the patched transition check.
         *
         * Original at ELF 0x5a1948: e5da  b.n 0x5a1500
         * Patched  at ELF 0x5a1948: e5f5  b.n 0x5a1536
         */
        const uint16_t swf_visible_to_transition[] = { 0xE5F5 };
        patch_thumb_halfwords(mod, (g_GSLoading_Update_sym & ~1u) + 0x4DC,
                              swf_visible_to_transition, 1,
                              "GSLoading::Update SWF visible return -> transition gate");
    }

    const struct {
        const char *name;
        uintptr_t fn;
    } firsttime_hooks[] = {
        { "_ZN16CDownloadManager13InitParsePackEv", (uintptr_t)hook_CDownloadManager_InitParsePack },
        { "_ZN20CNotificationManager4InitEv", (uintptr_t)hook_CNotificationManager_Init },
        { "_ZN20CNotificationManager15AddNotificationE11ENotifiTypeiiPKv", (uintptr_t)hook_CNotificationManager_AddNotification },
        { "_ZN20CNotificationManager10CanDisplayEv", (uintptr_t)hook_CNotificationManager_CanDisplay },
        { "_ZN20CNotificationManager21AddLocalNotificationsEv", (uintptr_t)hook_CNotificationManager_AddLocalNotifications },
        /* v5.05: CIapManager::Init runs for real (traced: field setup + QueryProductListFromServer,
           which now lands in the local store; no JNI, no threads). */
        { "_ZN11ZombiesGame16GetCountryIpCodeEv", (uintptr_t)hook_ZombiesGame_GetCountryIpCode },
        { "_ZN13TapJoyManager13UpdateRequestEv", (uintptr_t)hook_TapJoyManager_UpdateRequest },
        { "_ZN9SNManager14SetOfflineNameERKSs", (uintptr_t)hook_SNManager_SetOfflineName },
    };
    for (unsigned int i = 0; i < sizeof(firsttime_hooks) / sizeof(firsttime_hooks[0]); i++) {
        uintptr_t sym = so_symbol(mod, firsttime_hooks[i].name);
        if (sym) {
            hook_addr(sym, firsttime_hooks[i].fn);
            hooked++;
            LOGF("  ✓ Hooked FirstTimeInit service %s at 0x%08X\n",
                 firsttime_hooks[i].name, (unsigned)sym);
        } else {
            LOGF("  ! FirstTimeInit service symbol not found: %s\n",
                 firsttime_hooks[i].name);
        }
    }

    uintptr_t post_init_sym = so_symbol(mod, "_ZN11Application8PostInitEv");
    if (post_init_sym) {
        LOGF("  Application::PostInit left original at 0x%08X\n",
             (unsigned)post_init_sym);
        /*
         * v4.14: dropped the +0x0a94 / +0x0cd2 "skip" patches.
         *
         * They were { 0xf7ff, 0xbffe } which decodes as B.W (T4) with
         * S=1,J1=1,J2=1,imm10=0x3FF,imm11=0x7FE → offset = -4 → branch to
         * self.  That's an infinite spinlock, not a skip:
         *   - +0x0cd2 (v4.12) sat on a dead code path the suppressed
         *     tracking hooks never reached during PostInit, so the game
         *     still booted.
         *   - +0x0a94 (v4.13) IS reached during PostInit (right after the
         *     second getNumberOfCPUCores, before DLC/save), so it stalled
         *     the boot.
         *
         * The real freeze (visible in v4.11/v4.12 logs as silence after
         * "TrackingManager::AddEvent suppressed" + two generic service
         * successes) lives in Application::_Update's first-frame init
         * path — specifically the smart-pointer release chain at
         * _Update+0x16C0..+0x247E that destructs the EventValue
         * temporaries.  Patch belongs in _Update (skip the tracking
         * call before the temps are constructed), not here.
         */
    } else {
        LOG("  ! Application::PostInit symbol not found\n");
    }

    /* Application::Load — left original (MC3 style: real load handles missing saves) */
    uintptr_t app_load_sym = so_symbol(mod, "_ZN11Application4LoadEb");
    if (app_load_sym) {
        LOGF("  Application::Load(bool) at 0x%08X (left original)\n",
             (unsigned)app_load_sym);
    } else {
        LOG("  ! Application::Load symbol not found\n");
    }

    /* FlashManager SWF functions — left original, addresses logged for reference.
     * Offsets relative to libZombieWood.so base (base = post_init_sym - 0x4d2698). */
    if (post_init_sym) {
        uintptr_t so_base = (post_init_sym & ~1u) - 0x4d2698u;
        LOGF("  FlashManager::SWFInit  expected at 0x%08X (left original)\n",
             (unsigned)(so_base + 0x4db978u));
        LOGF("  FlashManager::SWFLoad  expected at 0x%08X (left original)\n",
             (unsigned)(so_base + 0x4dfaacu));
        LOGF("  FlashManager::SWFLoadLoad expected at 0x%08X (left original)\n",
             (unsigned)(so_base + 0x4dbc18u));
        LOGF("  SwfTextureLoader       expected at 0x%08X (left original)\n",
             (unsigned)(so_base + 0x4db864u));
    }

    uintptr_t glf_update_sym = so_symbol(mod, "_ZN3glf3App6UpdateEv");
    LOGF("  glf::App::Update left original at 0x%08X\n",
         (unsigned)glf_update_sym);

    const struct {
        const char *name;
        uintptr_t fn;
    } glf_update_helpers[] = {
        { "_ZN3glf8debugger8Debugger11UpdateFrameEv", (uintptr_t)hook_GlfDebugger_UpdateFrame },
        { "_ZN3glf8debugger12PerfCounters11UpdateValueEPKcj", (uintptr_t)hook_GlfPerfCounters_UpdateValue },
        { "_ZN3glf12InputManager11UpdateFrameEv", (uintptr_t)hook_GlfInputManager_UpdateFrame },
    };
    for (unsigned int i = 0; i < sizeof(glf_update_helpers) / sizeof(glf_update_helpers[0]); i++) {
        uintptr_t sym = so_symbol(mod, glf_update_helpers[i].name);
        if (sym) {
            hook_addr(sym, glf_update_helpers[i].fn);
            hooked++;
            LOGF("  ✓ Hooked GLF update helper %s at 0x%08X\n",
                 glf_update_helpers[i].name, (unsigned)sym);
        } else {
            LOGF("  ! GLF update helper symbol not found: %s\n",
                 glf_update_helpers[i].name);
        }
    }

    /* v4.98: no longer hooked. The no-op left every queued glf event (all touch input)
     * undelivered; the loader now drains the queue itself once per frame. */
    uintptr_t dispatch_events_sym = 0;
    if (dispatch_events_sym) {
        hook_addr(dispatch_events_sym, (uintptr_t)hook_GlfEventManager_DispatchEvents);
        hooked++;
        LOGF("  ✓ Hooked glf::EventManager::DispatchEvents no-op at 0x%08X\n",
             (unsigned)dispatch_events_sym);
    } else {
        LOG("  ! glf::EventManager::DispatchEvents symbol not found\n");
    }

    uintptr_t internal_update_sym = so_symbol(mod, "_ZN11Application7_UpdateEi");
    g_Application_InternalUpdate_orig = (void (*)(void *, int))internal_update_sym;
    LOGF("  Application::_Update direct target at 0x%08X\n",
         (unsigned)internal_update_sym);
    if (internal_update_sym) {
        /*
         * _Update+0x62 dereferences the CHeatUpManager singleton before it is
         * guaranteed to exist on Vita. Skip only that block and keep the rest of
         * the real update loop alive so render state can be populated.
         */
        const uint16_t skip_heatup[] = { 0xe005, 0xbf00 }; /* b.n to _Update+0x70; nop */
        patch_thumb_halfwords(mod, (internal_update_sym & ~1u) + 0x62,
                              skip_heatup, 2, "Application::_Update heatup guard");
        /*
         * _Update+0x162: LDRB.W R3,[R1,#0x1b0] crashes when ZombiesGame singleton
         * (SingletonFast<ZombiesGame>::s_instance) is null — which it is until
         * proper game init runs. Replace with MOVS R3,#0; NOP so the subsequent
         * CMP/BNE treats the flag as 0 (unset) and takes the fall-through path.
         */
        const uint16_t skip_zgame_flag[] = { 0x2300, 0xbf00 }; /* movs r3,#0; nop */
        patch_thumb_halfwords(mod, (internal_update_sym & ~1u) + 0x162,
                              skip_zgame_flag, 2, "Application::_Update ZombiesGame null guard");
        /*
         * _Update+0x36 originally branches based on a ZombiesGame field, but
         * first-frame startup can reach this block while ZombiesGame is still
         * null. Route null through the epilogue and keep the original update
         * path for non-null instances.
         */
        /*
         * v4.52: the old +0x36 "cbnz r1,+0x3A" / +0x38 "b +0x152" patches
         * jumped over +0x38 "mov r0,r1", so the following
         *   ldrd r2,r3,[r0,#0x118]! ; adds/adc dt ; strd
         * added dt to Application+0x118 (corrupting it) instead of
         * ZombiesGame+0x118 — the splash/loading timer that CLevel::Loading
         * step 0xA1 ("splash screen time still too short") and GSLoading wait
         * on (> 3000 ms). Replacement keeps the null guard and the mov:
         *   +0x32 cmp r1,#0 ; +0x34 beq.w +0x152 ; +0x38 mov r0,r1
         * (drops the original ZombiesGame+0x1b0 flag check that skipped the
         * timer while that flag was set).
         */
        const uint16_t zgame_null_guard_timer[] = { 0x2900, 0xF000, 0x808D, 0x4608 };
        patch_thumb_halfwords(mod, (internal_update_sym & ~1u) + 0x32,
                              zgame_null_guard_timer, 4,
                              "Application::_Update ZombiesGame null guard + timer fix");
        /*
         * Restore the original LDRD for the non-null path. Earlier builds
         * replaced it while proving which path was crashing.
         */
        const uint16_t zgame_ldrd_orig[] = { 0xe9f0, 0x2346 }; /* ldrd r2,r3,[r0,#0x118]! */
        patch_thumb_halfwords(mod, (internal_update_sym & ~1u) + 0x3a,
                              zgame_ldrd_orig, 2,
                              "Application::_Update ZombiesGame LDRD restore");
    }

    uintptr_t internal_draw_sym = so_symbol(mod, "_ZN11Application5_DrawEv");
    g_Application_InternalDraw_orig = (void (*)(void *))internal_draw_sym;
    LOGF("  Application::_Draw direct target at 0x%08X\n",
         (unsigned)internal_draw_sym);

    uintptr_t update_app_sym = so_symbol(mod, "_ZN11Application9UpdateAppEi");
    if (update_app_sym) {
        hook_addr(update_app_sym, (uintptr_t)hook_Application_UpdateApp_trace);
        hooked++;
        LOGF("  ✓ Hooked Application::UpdateApp trace at 0x%08X\n",
             (unsigned)update_app_sym);
    } else {
        LOG("  ! Application::UpdateApp symbol not found\n");
    }

    const struct {
        const char *name;
        uintptr_t replacement;
    } optional_update_hooks[] = {
        { "_Z20AResLoader_GetLengthPKc", (uintptr_t)hook_AResLoader_GetLength },
        { "_Z18AResLoader_GetDataPKc", (uintptr_t)hook_AResLoader_GetData },
        { "_ZN9SNManager6UpdateEi", (uintptr_t)hook_SNManager_Update_noop },
        { "_ZN18CGameConfigManager6UpdateEv", (uintptr_t)hook_CGameConfigManager_Update_noop },
        /* v4.46: CMenu::Loading is no longer hooked. Its "block" (v4.28) was
         * the gameswf hash::add spin caused by the softfp/hardfp float ABI
         * mismatch (fixed in v4.44). The emulator only ran stage 1 (Run) and
         * skipped stages 2-9 (SWFPostLoad, SetVisible, LoadPlayerObject...),
         * which left GSLoading stuck. Let the real stage machine run. */
        /* Application::SetupDevice must run to let the app create the device
         * and transition into a drawable state. createDeviceEx/IDevice::run
         * remain hooked below to avoid blocking in Android's device loop. */
        /* gameswf probe hooks removed: root cause was g_device not being set */
        { "_ZN14CPlayerManager6UpdateEi", (uintptr_t)hook_CPlayerManager_Update_noop },
        { "_ZN4glot15TrackingManager6UpdateEv", (uintptr_t)hook_TrackingManager_Update_noop },
        { "_ZN27CComponentLeaderboardReward4LoadEP13CMemoryStream", (uintptr_t)hook_CComponentLeaderboardReward_Load },
        { "_ZN16CDownloadManager6UpdateEv", (uintptr_t)hook_CDownloadManager_Update_noop },
        { "_ZN11GLOTManager6UpdateEi", (uintptr_t)hook_GLOTManager_Update_noop },
    };
    for (unsigned int i = 0; i < sizeof(optional_update_hooks) / sizeof(optional_update_hooks[0]); i++) {
        uintptr_t sym = so_symbol(mod, optional_update_hooks[i].name);
        if (sym) {
            hook_addr(sym, optional_update_hooks[i].replacement);
            hooked++;
            LOGF("  ✓ Hooked optional Android update service %s at 0x%08X\n",
                 optional_update_hooks[i].name, (unsigned)sym);
        } else {
            LOGF("  ! Optional Android update service symbol not found: %s\n",
                 optional_update_hooks[i].name);
        }
    }

    uintptr_t glf_run_sym = so_symbol(mod, "_ZN3glf3App3RunEv");
    if (glf_run_sym) {
        hook_addr(glf_run_sym, (uintptr_t)hook_GlfApp_Run);
        hooked++;
        LOGF("  ✓ Hooked glf::App::Run one-tick at 0x%08X\n",
             (unsigned)glf_run_sym);
    } else {
        LOG("  ! glf::App::Run symbol not found\n");
    }

    uintptr_t run_sym = so_symbol(mod, "_ZN11Application3RunEv");
    if (run_sym) {
        hook_addr(run_sym, (uintptr_t)hook_Application_Run);
        hooked++;
        LOGF("  ✓ Hooked Application::Run one-tick at 0x%08X\n",
             (unsigned)run_sym);
    } else {
        LOG("  ! Application::Run symbol not found\n");
    }

    uintptr_t run_thunk_sym = so_symbol(mod, "_ZThn48_N11Application3RunEv");
    if (run_thunk_sym) {
        hook_addr(run_thunk_sym, (uintptr_t)hook_Application_Run_thunk_48);
        hooked++;
        LOGF("  ✓ Hooked Application::Run thunk one-tick at 0x%08X\n",
             (unsigned)run_thunk_sym);
    } else {
        LOG("  ! Application::Run thunk symbol not found\n");
    }

    uintptr_t update_thunk_sym = so_symbol(mod, "_ZThn48_N11Application6UpdateEv");
    if (update_thunk_sym) {
        hook_addr(update_thunk_sym, (uintptr_t)hook_Application_Update_thunk_48_setup_once);
        hooked++;
        LOGF("  ✓ Hooked Application::Update thunk setup-once at 0x%08X\n",
             (unsigned)update_thunk_sym);
    } else {
        LOG("  ! Application::Update thunk symbol not found\n");
    }

    if (hooked > 0) {
        so_flush_caches(mod);
    }
}

static int symbol_starts_with(const char *name, const char *prefix) {
    return strncmp(name, prefix, strlen(prefix)) == 0;
}

static void hook_gameloft_services_symbols(so_module *mod) {
    static const struct {
        const char *name;
        uintptr_t replacement;
    } hooks[] = {
        { "_ZN16CDownloadManager11CheckForDLCEv", (uintptr_t)hook_DLC_CheckForDLC },
        { "_ZN16CDownloadManager11LoadDLCDataEPKcRi", (uintptr_t)hook_DLC_LoadDLCData },
        { "_ZN16CDownloadManager12CleanDLCDataEv", (uintptr_t)hook_DLC_CleanDLCData },
        { "_ZN11GLOTManager7StartupEv", (uintptr_t)hook_GLOTManager_InitManager },
        { "_ZN11GLOTManager11InitManagerEv", (uintptr_t)hook_GLOTManager_InitManager },
        { "_ZN11GLOTManager12GetDLCStatusEi", (uintptr_t)hook_GLOTManager_GetDLCStatus },
        { "_ZN10glwebtools20ThreadPosixImpl_Data6UpdateEPv", (uintptr_t)hook_glwebtools_thread_update },
        { "_ZN4gaia4Gaia10InitializeERKSsbPFvNS_7OpCodesES2_iPvES4_", (uintptr_t)hook_service_noop_int },
        { "_ZN4gaia4Gaia17StartWorkerThreadENS_11GaiaRequestEPKc", (uintptr_t)hook_service_noop_int },
        { "_ZN4gaia4Gaia34PerformAsyncActionDataContainerPtrEPvS1_", (uintptr_t)hook_service_noop_int },
        { "_ZN4gaia4Gaia6UpdateEv", (uintptr_t)hook_service_noop_int },
        { "_ZN4gaia4Gaia12UpdateStaticEPvS1_", (uintptr_t)hook_service_noop_int },
        { "_ZN4gaia4Gaia9InitGLUIDEv", (uintptr_t)hook_service_noop_int },
        { "nativeGetHttpResponse", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools10GlWebTools10InitializeERKNS0_16CreationSettingsE", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools10GlWebTools6UpdateEv", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools10GlWebTools8ShutdownEv", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools10GlWebTools16CreateUrlRequestERNS_10UrlRequest16CreationSettingsE", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools10GlWebTools16CreateUrlRequestEv", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools10GlWebTools19CreateUrlConnectionERNS_13UrlConnection16CreationSettingsE", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools10GlWebTools19CreateUrlConnectionEv", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools14GlWebToolsCore10InitializeERKNS_10GlWebTools16CreationSettingsE", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools14GlWebToolsCore14CreateInstanceERPS0_", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools14GlWebToolsCore16CreateUrlRequestERNS_10UrlRequest16CreationSettingsE", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools14GlWebToolsCore16CreateUrlRequestEv", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools14GlWebToolsCore19CreateUrlConnectionERKNS_13UrlConnection16CreationSettingsE", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools14GlWebToolsCore19CreateUrlConnectionEv", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools14GlWebToolsCore16UpdateTaskGroupsEy", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools14GlWebToolsCore12UpdateStaticEPvS1_", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools14GlWebToolsCore20CancelAllConnectionsEv", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools14GlWebToolsCore21_CancelAllConnectionsEv", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools14GlWebToolsCore6UpdateEv", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools14GlWebToolsCore8ShutdownEv", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools14GlWebToolsCore9TerminateEv", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools14GlWebToolsCore9TerminateEj", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools10ThreadPool10InitializeERKNS0_16CreationSettingsE", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools10ThreadPool9AddThreadEv", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools10ThreadPool6SignalERKNS_3JobE", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools10ThreadPool6UpdateEyb", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools10ThreadPool9TerminateEv", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools13UrlConnection12StartRequestERNS_10UrlRequestE", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools13UrlConnection13CancelRequestEv", (uintptr_t)hook_service_noop_int },
        { "_ZN10glwebtools13UrlConnection7ReleaseEv", (uintptr_t)hook_service_noop_int },
        { "_ZN17APushNotification19HasPushNotificationEv", (uintptr_t)hook_service_noop_int },
        { "_ZN17APushNotification23ResetNotificationStatusEv", (uintptr_t)hook_service_noop_int },
        { "_ZN17APushNotification26SetOfflineDeviceCredentialEv", (uintptr_t)hook_service_noop_int },
        { "_ZN17APushNotification13GetBundleDataEv", (uintptr_t)hook_service_noop_int },
        { "_ZN17APushNotification8IsEnableEv", (uintptr_t)hook_service_noop_int },
        { "_ZN17APushNotification9GetStatusEb", (uintptr_t)hook_service_noop_int },
        { "_ZN4glot15TrackingManager10InitializeERKSsPcj", (uintptr_t)hook_TrackingManager_Initialize },
        { "_ZN4glot15TrackingManager11GetInstanceEv", (uintptr_t)hook_TrackingManager_GetInstance },
        { "_ZN4glot15TrackingManager8AddEventEiNS_10EventValueES1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_S1_", (uintptr_t)hook_TrackingManager_AddEvent },
        { "_ZN4glot15TrackingManager8AddEventEiPNS_10EventValueEi", (uintptr_t)hook_TrackingManager_AddEvent },
        /*
         * v4.15: Removed EventValue ctor/dtor hooks. They were nooping
         * to memset(self,0,16), which sets the std::string member's
         * _M_dataplus._M_p to NULL. But gcc libstdc++ represents an
         * empty string with a non-NULL sentinel (_S_empty_rep_storage).
         *
         * Inside Application::_Update, gcc inlined the std::string
         * release for the EventValue temporaries that get passed to
         * AddEvent by value. The release code does:
         *
         *     LDR R3, [stackslot]      ; data ptr
         *     SUBS R0, R3, #12         ; control block
         *     CMP R0, sentinel
         *     BNE .Lreal_release       ; -> LDREX/STREX atomic_dec
         *
         * With our nooped ctor: R3 = 0, R0 = -12 = 0xFFFFFFF4, mismatch,
         * atomic decrement on 0xFFFFFFF0 -> heap corruption -> the chain
         * at _Update+0x16C0..+0x247E silently freezes after AddEvent.
         *
         * Letting the real ctors/dtor run initializes the std::string
         * properly so the release path takes the empty-sentinel branch.
         * AddEvent itself is still hooked, so no actual network call.
         */
        { "_ZN4glot15TrackingManager6UpdateEv", (uintptr_t)hook_TrackingManager_Update },
        { "_ZN4glot15TrackingManager7OnPauseEv", (uintptr_t)hook_TrackingManager_OnPause },
        { "_ZN4glot15TrackingManager8OnResumeEv", (uintptr_t)hook_TrackingManager_OnResume },
    };

    unsigned int hooked = 0;
    for (unsigned int i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
        uintptr_t sym = so_symbol(mod, hooks[i].name);
        if (!sym) {
            LOGF("  ! Gameloft service symbol not found: %s\n", hooks[i].name);
            continue;
        }

        hook_addr(sym, hooks[i].replacement);
        hooked++;
        LOGF("  ✓ Hooked Gameloft service %s at 0x%08X\n",
             hooks[i].name, (unsigned)sym);
    }

    static const char *tracking_skip[] = {
        "AddEvent",
        "Initialize",
        "OnPause",
        "OnResume",
        "Update",
        "C1Ev",
        "C2Ev",
        "D0Ev",
        "D1Ev",
        "D2Ev",
        "GetInstance",
        "FreeInstance",
    };
    for (int i = 0; i < mod->num_dynsym; i++) {
        if (mod->dynsym[i].st_shndx == SHN_UNDEF || !mod->dynsym[i].st_name) {
            continue;
        }

        const char *name = mod->dynstr + mod->dynsym[i].st_name;
        if (!symbol_starts_with(name, "_ZN4glot15TrackingManager")) {
            continue;
        }

        int skip = 0;
        for (unsigned int j = 0; j < sizeof(tracking_skip) / sizeof(tracking_skip[0]); j++) {
            if (strstr(name, tracking_skip[j])) {
                skip = 1;
                break;
            }
        }
        if (skip) {
            continue;
        }

        if (mod->dynsym[i].st_size < 8) {
            continue;
        }

        uintptr_t sym = mod->text_base + mod->dynsym[i].st_value;
        hook_addr(sym, (uintptr_t)hook_service_success_int);
        hooked++;
    }

    for (int i = 0; i < mod->num_dynsym; i++) {
        if (mod->dynsym[i].st_shndx == SHN_UNDEF || !mod->dynsym[i].st_name) {
            continue;
        }

        const char *name = mod->dynstr + mod->dynsym[i].st_name;
        if (mod->dynsym[i].st_size < 8) {
            continue;
        }

        if (!symbol_starts_with(name, "_ZN8glevents12EventManager9saveEvent") &&
            !symbol_starts_with(name, "_ZN8glevents12EventManager10saveStatus") &&
            strcmp(name, "_ZN8glevents12EventManager10InitializeENS_20trackingServerConfigEjjRKSsS3_S3_S3_") != 0 &&
            !symbol_starts_with(name, "_ZN8glevents16EventManagerImpl9saveEvent") &&
            !symbol_starts_with(name, "_ZN8glevents16EventManagerImpl10saveStatus") &&
            strcmp(name, "_ZN8glevents16EventManagerImpl10InitializeENS_20trackingServerConfigEjjRKSsS3_S3_S3_") != 0 &&
            strcmp(name, "_ZN8glevents16EventManagerImpl6updateEv") != 0 &&
            strcmp(name, "_ZN8glevents16EventManagerImpl12_readXmlFileEv") != 0 &&
            strcmp(name, "_ZN8glevents16EventManagerImpl12_saveTrackerEjPNS_7TrackerE") != 0) {
            continue;
        }

        uintptr_t sym = mod->text_base + mod->dynsym[i].st_value;
        hook_addr(sym, (uintptr_t)hook_service_success_int);
        hooked++;
    }

    if (hooked > 0) {
        so_flush_caches(mod);
    }
    LOGF("  ✓ Hooked %u Gameloft DLC/tracking service functions\n", hooked);

    static const char *tiny_return0_hooks[] = {
        "APushNotification_HasPushNotification",
        "APushNotification_ResetNotificationStatus",
        "APushNotification_SetOfflineDeviceCredential",
        "APushNotification_GetBundleData",
        "APushNotification_IsEnable",
        "APushNotification_GetStatus",
    };
    const uint16_t movs_r0_0_bx_lr[] = { 0x2000, 0x4770 };
    for (unsigned int i = 0; i < sizeof(tiny_return0_hooks) / sizeof(tiny_return0_hooks[0]); i++) {
        uintptr_t sym = so_symbol(mod, tiny_return0_hooks[i]);
        if (sym) {
            patch_thumb_halfwords(mod, sym & ~1u, movs_r0_0_bx_lr, 2,
                                  tiny_return0_hooks[i]);
        } else {
            LOGF("  ! Tiny return0 symbol not found: %s\n", tiny_return0_hooks[i]);
        }
    }
}

static void hook_language_symbols(so_module *mod) {
    static const struct {
        const char *name;
        uintptr_t replacement;
    } hooks[] = {
        { "DeviceUtils_GetLanguage", (uintptr_t)hook_language_index0 },
        { "_Z12Get_Languagev", (uintptr_t)hook_language_index0 },
        { "_ZN14CStringManager24GetUserPreferredLanguageEv", (uintptr_t)hook_language_index0 },
        { "_ZN14CStringManager16GetLanguageIndexEv", (uintptr_t)hook_language_index0 },
        { "_ZN14CStringManager22GetLanguageIndexGLLiveEv", (uintptr_t)hook_language_index0 },
        { "_ZN11DeviceUtils8LanguageEv", (uintptr_t)hook_language_index0 },
        { "nativeGetCurrentLocale", (uintptr_t)hook_language_en },
        { "nativeGetPhoneLanguageS", (uintptr_t)hook_language_en },
    };

    unsigned int hooked = 0;
    for (unsigned int i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
        uintptr_t sym = so_symbol(mod, hooks[i].name);
        if (!sym) continue;
        hook_addr(sym, hooks[i].replacement);
        hooked++;
        LOGF("  ✓ Hooked language helper %s at 0x%08X\n",
             hooks[i].name, (unsigned)sym);
    }

    if (hooked > 0) {
        so_flush_caches(mod);
    }
    LOGF("  ✓ Hooked %u language helper functions\n", hooked);
}

/* ── v5.06: timing wrapper on glitch::video::CImageLoaderPng::loadImage ──
 * Installed by swapping the vtable slot (vtable so+0xca6110, slot +28), so the original
 * stays callable. The function returns an intrusive_ptr<IImage> through a hidden first
 * argument: r0 = result slot, r1 = this, r2 = IReadFile*. IReadFile::getFileName is
 * vtable +0x28 and returns a char*. */
typedef void (*zw_png_load_fn)(void **out, void *self, void *file);
static zw_png_load_fn g_zw_png_load_orig;
static SceUInt64 g_zw_png_us; static unsigned g_zw_png_n;
/* v5.06: the game's own libpng (old, built for size) needs 350-490 ms for a 1024x1024 RGBA
 * sheet, and the menu loads 13 of them in one frame. Plain 8-bit RGBA, non-interlaced files
 * are decoded here with the SDK's libpng 1.6 (NEON row filters) straight into a CImage built
 * with the game's own allocator and constructor, exactly as the original does:
 * new(0x2c) = so+0x8f8230, CImage(fmt 0xe, dimension2d{w,h}) = so+0x86cff4, pixel pointer at
 * +8, row pitch at +0x18, reference count at +4. Anything else goes to the original. */
#ifndef ZW_PNG_FAST
#define ZW_PNG_FAST 1
#endif
static unsigned g_zw_png_fast_n;
static void zw_png_read_cb(png_structp png, png_bytep dst, png_size_t n) {
    void *file = png_get_io_ptr(png);
    int (*rd)(void *, void *, unsigned) = *(void **)(*(uint8_t **)file + 0xc);
    if ((png_size_t)rd(file, dst, (unsigned)n) != n) png_error(png, "short read");
}
static void zw_png_err_cb(png_structp png, png_const_charp msg) { (void)msg; longjmp(png_jmpbuf(png), 1); }
static void zw_png_warn_cb(png_structp png, png_const_charp msg) { (void)png; (void)msg; }
static int zw_png_fast(void **out, void *file) {
    int (*rd)(void *, void *, unsigned) = *(void **)(*(uint8_t **)file + 0xc);
    int (*seek)(void *, long, int) = *(void **)(*(uint8_t **)file + 0x18);
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    uint8_t hd[29];
    int got = rd(file, hd, sizeof(hd));
    seek(file, 0, 0);
    if (got != (int)sizeof(hd) || memcmp(hd, sig, 8) || memcmp(hd + 12, "IHDR", 4)) return 0;
    uint32_t w = (hd[16] << 24) | (hd[17] << 16) | (hd[18] << 8) | hd[19];
    uint32_t h = (hd[20] << 24) | (hd[21] << 16) | (hd[22] << 8) | hd[23];
    if (hd[24] != 8 || hd[25] != 6 || hd[28] != 0 || !w || !h || w > 2048 || h > 2048) return 0;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, zw_png_err_cb, zw_png_warn_cb);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!info) { if (png) png_destroy_read_struct(&png, NULL, NULL); return 0; }
    uint32_t * volatile img = NULL;
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        if (img) { LOG("  v5.06 png fast path: decode error, image left incomplete\n"); *out = img; return 1; }
        seek(file, 0, 0);
        return 0;
    }
    png_set_read_fn(png, file, zw_png_read_cb);
    png_set_crc_action(png, PNG_CRC_QUIET_USE, PNG_CRC_QUIET_USE);
    png_read_info(png, info);
    if (png_get_valid(png, info, PNG_INFO_tRNS) || png_get_rowbytes(png, info) != w * 4) png_error(png, "unexpected layout");

    void *(*g_new)(unsigned) = (void *)ZW_SO_FN(0x8f8230);
    void (*ctor)(void *, int, const int *) = (void *)ZW_SO_FN(0x86cff4);
    int dim[2] = { (int)w, (int)h };
    uint32_t *im = g_new(0x2c);
    if (!im) png_error(png, "no memory");
    ctor(im, 0xe, dim);
    if (!im[2] || im[6] != w * 4) {   /* not the layout this path was written for */
        LOGF("  v5.06 png fast path: unexpected CImage data=%p pitch=%u\n", (void *)im[2], (unsigned)im[6]);
        void (*del)(void *) = *(void **)(*(uint8_t **)im + 4);
        del(im);
        png_error(png, "layout");
    }
    __sync_fetch_and_add(&im[1], 1);
    img = im;
    uint8_t *row = (uint8_t *)im[2];
    for (uint32_t y = 0; y < h; y++, row += w * 4) png_read_row(png, row, NULL);
    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);
    *out = im;
    g_zw_png_fast_n++;
    return 1;
}
static void **zw_png_load_wrap(void **out, void *self, void *file) {   /* returns the result slot, as the original does */
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    if (!(ZW_PNG_FAST && file && zw_png_fast(out, file)))
        g_zw_png_load_orig(out, self, file);
    SceUInt64 dt = sceKernelGetProcessTimeWide() - t0;
    g_zw_png_us += dt; g_zw_png_n++;
    if (ZW_BOOT_TIMING && g_zw_png_n <= 80 && dt > 20000) {
        const char *name = NULL;
        if (file) { const char *(*gfn)(void *) = *(void **)(*(uint8_t **)file + 0x28); name = gfn(file); }
        uint32_t *img = out ? (uint32_t *)*out : NULL;
        LOGF("  v5.06 png #%u %u ms (total %u ms) pitch=%u %s\n", g_zw_png_n, (unsigned)(dt / 1000),
             (unsigned)(g_zw_png_us / 1000), img ? (unsigned)img[6] : 0, name ? name : "-");
    }
    return out;
}

/* ── v5.06: weapons shop icons ──
 * Measured: scrolling the weapons shop ran at 13-26 fps. GSShop::UpdateIcons (so+0x5a9d9c,
 * 796 bytes) fills 8 icon slots through ZombiesGame::LoadIconTexture. glitch drops a texture
 * from its manager as soon as only the manager still references it
 * (ITexture::removeFromTextureManager, so+0x86b4f4), so every scroll step read and decoded
 * two 425x295 PNGs from the OBB again (about 60 ms each), including ones shown a moment ago.
 *
 * 1. Keep-alive list: when UpdateIcons asks to drop an icon, the loader takes a reference
 *    instead, so the texture stays in the manager and the next request by name finds it.
 *    At most ZW_ICON_KEEP textures; the oldest unused one is really dropped when the list is
 *    full, and everything is dropped when the game leaves the shop state.
 * 2. Shop list icons use the game's own half-size files (name_lowres.png, 212x147), the
 *    same switch the game applies itself when texture-manager flag 0x100 is set. Other
 *    callers of LoadIconTexture are unchanged.
 * LoadIconTexture(result slot, ZombiesGame*, name) is re-implemented here because hook_addr
 * has no trampoline: it is CTextureManager::getTexture(result, mgr, name, 0) (so+0x858834)
 * with the _lowres lookup first. The device pointer comes from the GOT slot so+0xcd2adc. */
#ifndef ZW_SHOP_ICON_CACHE
#define ZW_SHOP_ICON_CACHE 1
#endif
#ifndef ZW_SHOP_LOWRES_ICONS
#define ZW_SHOP_LOWRES_ICONS 1
#endif
#define ZW_ICON_KEEP 24   /* v5.10: was 48 (~12 MB of vram); 24 lowres icons ~ 3 MB */
static void *g_zw_icon_keep[ZW_ICON_KEEP];
static int g_zw_icon_keep_n;
static unsigned g_zw_icon_kept_total, g_zw_icon_evicted, g_zw_icon_low, g_zw_icon_full;
static SceUInt64 g_zw_shop_upd_us, g_zw_shop_rend_us; static unsigned g_zw_shop_upd_n;

static int zw_lr_in_shop_icons(uintptr_t lr) {
    uintptr_t o = (lr & ~(uintptr_t)1) - g_zw_so_base;
    return o >= 0x5a9d9c && o < 0x5a9d9c + 796;
}
static int zw_tex_refs(void *tex) { return *(volatile int *)((uint8_t *)tex + 4); }
/* the game's intrusive_ptr_release(ITexture*): drops one reference and removes the texture
 * from the manager when that leaves only the manager's own */
static void zw_tex_release(void *tex) { void (*rel)(void *) = (void *)ZW_SO_FN(0x3d38dc); rel(tex); }

static void hook_ITexture_removeFromTextureManager(void *tex) {
    uintptr_t lr = (uintptr_t)__builtin_return_address(0);
    if (ZW_SHOP_ICON_CACHE && tex && zw_lr_in_shop_icons(lr)) {
        if (g_zw_icon_keep_n == ZW_ICON_KEEP) {   /* make room: oldest entry nothing else uses */
            for (int i = 0; i < g_zw_icon_keep_n; i++) {
                void *old = g_zw_icon_keep[i];
                if (zw_tex_refs(old) != 2) continue;
                memmove(&g_zw_icon_keep[i], &g_zw_icon_keep[i + 1], (g_zw_icon_keep_n - i - 1) * sizeof(void *));
                g_zw_icon_keep_n--;
                g_zw_icon_evicted++;
                zw_tex_release(old);
                break;
            }
        }
        if (g_zw_icon_keep_n < ZW_ICON_KEEP) {
            __sync_fetch_and_add((int *)((uint8_t *)tex + 4), 1);
            g_zw_icon_keep[g_zw_icon_keep_n++] = tex;
            g_zw_icon_kept_total++;
            return;
        }
        g_zw_icon_full++;
    }
    if (!tex) return;
    void *dev = *(void **)((uint8_t *)tex + 0x2c);
    void (*remove_texture)(void *, void *) = (void *)ZW_SO_FN(0x858e50);   /* CTextureManager::removeTexture(ITexture*) */
    remove_texture(*(void **)((uint8_t *)dev + 0xc8), tex);
}
static void zw_shop_icons_flush(void) {
    int n = g_zw_icon_keep_n;
    g_zw_icon_keep_n = 0;
    for (int i = 0; i < n; i++) zw_tex_release(g_zw_icon_keep[i]);
    if (n && ZW_PERIODIC_LOG) LOGF("  v5.06 shop icons: left the shop, released %d kept textures\n", n);
}

static void **hook_ZombiesGame_LoadIconTexture(void **out, void *game, const char *name) {
    (void)game;
    uintptr_t lr = (uintptr_t)__builtin_return_address(0);
    void **dev_global = *(void ***)(g_zw_so_base + 0xcd2adc);
    void *mgr = *(void **)((uint8_t *)*dev_global + 0xc8);
    volatile uint32_t *flags = (volatile uint32_t *)((uint8_t *)mgr + 0xf0);
    void (*get_texture)(void **, void *, const char *, const char *) = (void *)ZW_SO_FN(0x858834);
    int had = (*flags & 0x100) != 0;
    if (name && (had || (ZW_SHOP_LOWRES_ICONS && zw_lr_in_shop_icons(lr)))) {
        char low[160];
        const char *dot = strrchr(name, '.');
        size_t n = strlen(name);
        if (dot && strlen(dot) <= 7 && n + 8 < sizeof(low)) {
            size_t stem = (size_t)(dot - name);
            memcpy(low, name, stem);
            memcpy(low + stem, "_lowres", 7);
            strcpy(low + stem + 7, dot);
            void *t = NULL;
            *flags &= ~0x100u;
            get_texture(&t, mgr, low, NULL);
            if (had) *flags |= 0x100;
            if (t) { if (!had) g_zw_icon_low++; *out = t; return out; }
        }
    }
    get_texture(out, mgr, name, NULL);
    return out;
}

/* timing wrappers on GSShop::Update / Render (vtable so+0xc9af08, slots +24 and +32) */
static void (*g_zw_shop_update_orig)(void *, void *, unsigned);
static void (*g_zw_shop_render_orig)(void *, void *);
static void zw_shop_update_wrap(void *self, void *app, unsigned dt) {
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    g_zw_shop_update_orig(self, app, dt);
    g_zw_shop_upd_us += sceKernelGetProcessTimeWide() - t0; g_zw_shop_upd_n++;
}
static void zw_shop_render_wrap(void *self, void *app) {
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    g_zw_shop_render_orig(self, app);
    g_zw_shop_rend_us += sceKernelGetProcessTimeWide() - t0;
}
/* v5.07: redirect one Thumb `bl` inside the .so to a loader function. The bl cannot reach the
 * eboot, so it goes to a veneer (ldr.w pc,[pc]; .word fn) in the dead body of the hooked
 * ZombiesGame::LoadIconTexture (so+0x3d7a94, 248 bytes, the hook uses the first 8): slots 0-28. */
static uint32_t zw_thumb_bl(uint32_t from, uint32_t to) {   /* T1 BL encoding, to/from even Thumb addrs */
    int32_t off = (int32_t)to - (int32_t)(from + 4);
    uint32_t S = (off >> 24) & 1, I1 = (off >> 23) & 1, I2 = (off >> 22) & 1;
    uint32_t J1 = (~I1 ^ S) & 1, J2 = (~I2 ^ S) & 1;
    uint16_t h1 = 0xf000 | (S << 10) | ((off >> 12) & 0x3ff);
    uint16_t h2 = 0xd000 | (J1 << 13) | (J2 << 11) | ((off >> 1) & 0x7ff);
    return h1 | ((uint32_t)h2 << 16);
}
static int zw_redirect_bl(uintptr_t base, uint32_t site, uint32_t callee, int slot, void *fn) {
    uint32_t ven = base + 0x3d7a94 + 8 + slot * 8;
    uint16_t *s = (uint16_t *)(base + site);
    uint32_t have = s[0] | ((uint32_t)s[1] << 16);
    if (slot < 0 || slot > 28 || have != zw_thumb_bl(base + site, base + callee)) {
        LOGF("  v5.07 bl at so+0x%x: unexpected 0x%08X, not redirected\n", (unsigned)site, (unsigned)have);
        return 0;
    }
    uint32_t v[2] = { 0xf000f8df, (uint32_t)(uintptr_t)fn };
    kuKernelCpuUnrestrictedMemcpy((void *)ven, v, sizeof(v));
    uint32_t bl = zw_thumb_bl(base + site, ven);
    kuKernelCpuUnrestrictedMemcpy(s, &bl, sizeof(bl));
    return 1;
}

/* v5.07: GSShop::Update calls GSShop::RefreshList (so+0x5a9a2c) on EVERY frame while the
 * category index (+0x148) is 0-3. RefreshList calls UpdateItem -> SetOneItem (Flash SetText /
 * SetVisible by path) for every item of the category: measured 33 ms per frame, which held
 * the weapons shop at 17-20 fps even without touching the screen. The per-frame refresh is
 * there so countdowns and states keep updating, so it is kept, but spread out: this frame
 * refreshes a rotating slice, the whole list every ZW_SHOP_REFRESH_SPREAD frames. Any change
 * of list, length, category, selection (+0x15c) or equipped id (+0x258) refreshes the whole
 * list at once, as before. Only the call in GSShop::Update is redirected; the other callers
 * of RefreshList (popups, tutorial) still get the full refresh. */
#ifndef ZW_SHOP_REFRESH_SPREAD
#define ZW_SHOP_REFRESH_SPREAD 8
#endif
static SceUInt64 g_zw_refresh_us; static unsigned g_zw_refresh_full, g_zw_refresh_items;
static void __attribute__((pcs("aapcs"))) zw_shop_refresh_spread(void *shop) {
    static uintptr_t last_b; static int last_n = -1, pos;
    static uint32_t last_cat, last_sel, last_cur;
    SceUInt64 t0 = ZW_BOOT_TIMING ? sceKernelGetProcessTimeWide() : 0;
    uint8_t *s = (uint8_t *)shop;
    uintptr_t b = *(uintptr_t *)(s + 0x14c), e = *(uintptr_t *)(s + 0x150);
    int n = (int)((e - b) >> 2);
    uint32_t cat = *(uint32_t *)(s + 0x148), sel = *(uint32_t *)(s + 0x15c), cur = *(uint32_t *)(s + 0x258);
    void (*update_item)(void *, int) = (void *)ZW_SO_FN(0x5a99f8);   /* GSShop::UpdateItem(int) */
    if (n <= 0) { last_n = n; return; }
    if (b != last_b || n != last_n || cat != last_cat || sel != last_sel || cur != last_cur) {
        for (int i = 0; i < n; i++) update_item(shop, i);
        last_b = b; last_n = n; last_cat = cat; last_sel = sel; last_cur = cur; pos = 0;
        g_zw_refresh_full++; g_zw_refresh_items += n;
    } else {
        int k = (n + ZW_SHOP_REFRESH_SPREAD - 1) / ZW_SHOP_REFRESH_SPREAD;
        for (int j = 0; j < k; j++) { if (pos >= n) pos = 0; update_item(shop, pos++); }
        g_zw_refresh_items += k;
    }
    if (ZW_BOOT_TIMING) g_zw_refresh_us += sceKernelGetProcessTimeWide() - t0;
}

/* TEMP v5.07 measurement: call-site timers inside GSShop (veneer slots 0-9; 28 = refresh) */
#ifndef ZW_SHOP_SITE_TIMING
#define ZW_SHOP_SITE_TIMING ZW_BOOT_TIMING
#endif
#if ZW_SHOP_SITE_TIMING
typedef uintptr_t (*zw_fn4)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static const struct { uint32_t site, callee; const char *name; } g_zw_sites[] = {
    { 0x5a9a96, 0x449664, "CMenu::Update" },
    { 0x5a9a9e, 0x44d9d8, "NavigateMenu" },
    { 0x5a9bf6, 0x4df370, "FindChar" },
    { 0x5a9c7c, 0x4dbda0, "SWFUpdate" },
    { 0x5aed5e, 0x44e320, "CMenu::OnSetGD" },
    { 0x5aed78, 0x5abd30, "shopTouch" },
    { 0x5abfcc, 0x5a9d9c, "UpdIcons1" },
    { 0x5abfd8, 0x5a9d9c, "UpdIcons2" },
    { 0x5aa7f0, 0x5a9d9c, "UpdIconsCat" },
};
#define ZW_NSITES (int)(sizeof(g_zw_sites) / sizeof(g_zw_sites[0]))
static SceUInt64 g_zw_site_us[16]; static unsigned g_zw_site_n[16];
#define ZW_SITE_WRAP(k) static uintptr_t __attribute__((pcs("aapcs"))) zw_site_w##k(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) { \
    SceUInt64 t0 = sceKernelGetProcessTimeWide(); \
    uintptr_t r = ((zw_fn4)ZW_SO_FN(g_zw_sites[k].callee))(a, b, c, d); \
    g_zw_site_us[k] += sceKernelGetProcessTimeWide() - t0; g_zw_site_n[k]++; return r; }
ZW_SITE_WRAP(0) ZW_SITE_WRAP(1) ZW_SITE_WRAP(2) ZW_SITE_WRAP(3) ZW_SITE_WRAP(4) ZW_SITE_WRAP(5)
ZW_SITE_WRAP(6) ZW_SITE_WRAP(7) ZW_SITE_WRAP(8)
static void *const g_zw_site_wraps[] = { zw_site_w0, zw_site_w1, zw_site_w2, zw_site_w3, zw_site_w4, zw_site_w5,
    zw_site_w6, zw_site_w7, zw_site_w8 };
static void zw_patch_shop_sites(uintptr_t base) {
    int ok = 0;
    for (int k = 0; k < ZW_NSITES; k++) ok += zw_redirect_bl(base, g_zw_sites[k].site, g_zw_sites[k].callee, k, g_zw_site_wraps[k]);
    LOGF("  v5.07 shop call-site timers: %d/%d sites\n", ok, ZW_NSITES);
}
static void zw_shop_sites_log(int frame, unsigned nfr) {
    char buf[600]; int n = 0;
    for (int k = 0; k < ZW_NSITES && n < (int)sizeof(buf) - 60; k++) {
        if (!g_zw_site_n[k]) continue;
        n += snprintf(buf + n, sizeof(buf) - n, " %s %.1fms/%u", g_zw_sites[k].name,
                      g_zw_site_us[k] / 1000.0 / (nfr ? nfr : 1), g_zw_site_n[k]);
        g_zw_site_us[k] = 0; g_zw_site_n[k] = 0;
    }
    LOGF("  v5.07 sites f%d per frame: refresh %.1fms (full %u, items %u)%s\n", frame,
         g_zw_refresh_us / 1000.0 / (nfr ? nfr : 1), g_zw_refresh_full, g_zw_refresh_items, buf);
    g_zw_refresh_us = 0; g_zw_refresh_full = g_zw_refresh_items = 0;
}
#endif
static void zw_patch_shop(so_module *mod) {
    uintptr_t base = mod->text_base;
#if ZW_SHOP_SITE_TIMING
    zw_patch_shop_sites(base);
#endif
    zw_redirect_bl(base, 0x5a9c8a, 0x5a9a2c, 28, (void *)zw_shop_refresh_spread);   /* GSShop::Update -> RefreshList */
    if (ZW_SHOP_ICON_CACHE) hook_addr(base + 0x86b4f4 + 1, (uintptr_t)hook_ITexture_removeFromTextureManager);
    hook_addr(base + 0x3d7a94 + 1, (uintptr_t)hook_ZombiesGame_LoadIconTexture);
    if (ZW_BOOT_TIMING) {
        uintptr_t *upd = (uintptr_t *)(base + 0xc9af08 + 24), *rnd = (uintptr_t *)(base + 0xc9af08 + 32);
        if ((*upd & ~1u) == base + 0x5a9a5c && (*rnd & ~1u) == base + 0x5a7c84) {
            g_zw_shop_update_orig = (void *)*upd; g_zw_shop_render_orig = (void *)*rnd;
            uintptr_t w = (uintptr_t)zw_shop_update_wrap;
            kuKernelCpuUnrestrictedMemcpy(upd, &w, sizeof(w));
            w = (uintptr_t)zw_shop_render_wrap;
            kuKernelCpuUnrestrictedMemcpy(rnd, &w, sizeof(w));
        }
    }
    so_flush_caches(mod);
    LOGF("  v5.06 shop icons: keep-alive %d, lowres list icons %d\n", ZW_SHOP_ICON_CACHE ? ZW_ICON_KEEP : 0, ZW_SHOP_LOWRES_ICONS);
}
/* once per frame from the main loop */
static void zw_shop_frame(int frame) {
    int in_shop = g_zw_so_base && (uintptr_t)g_zw_cur_vtable == g_zw_so_base + 0xc9af10;
    if (!in_shop && g_zw_icon_keep_n) zw_shop_icons_flush();
    {   /* v5.10: free video memory on each shop entry and exit (one line each) */
        static int was_in_shop, visits;
        if (in_shop != was_in_shop) {
            was_in_shop = in_shop;
            if (in_shop) visits++;
            LOGF("  v5.10 shop %s #%d f%d: vram free %uK, kept icons %d\n", in_shop ? "enter" : "exit", visits, frame,
                 (unsigned)(vglMemFree(VGL_MEM_VRAM) >> 10), g_zw_icon_keep_n);
        }
    }
    if (ZW_BOOT_TIMING && ZW_PERIODIC_LOG && (frame % 60) == 59 && g_zw_shop_upd_n) {
        LOGF("  v5.06 shop f%d: GSShop update %.1f ms render %.1f ms per frame | icons kept %d (total %u, evicted %u, full %u) lowres %u | png total %u ms\n",
             frame, g_zw_shop_upd_us / 1000.0 / g_zw_shop_upd_n, g_zw_shop_rend_us / 1000.0 / g_zw_shop_upd_n,
             g_zw_icon_keep_n, g_zw_icon_kept_total, g_zw_icon_evicted, g_zw_icon_full, g_zw_icon_low, (unsigned)(g_zw_png_us / 1000));
#if ZW_SHOP_SITE_TIMING
        zw_shop_sites_log(frame, g_zw_shop_upd_n);
#endif
        g_zw_shop_upd_us = g_zw_shop_rend_us = 0; g_zw_shop_upd_n = 0;
    }
}
static void zw_patch_png_loader(so_module *mod) {
    uintptr_t *slot = (uintptr_t *)(mod->text_base + 0xca6110 + 28);
    if ((*slot & ~1u) != mod->text_base + 0x870a40) {
        LOGF("  v5.06 png loader slot unexpected 0x%08X, not patched\n", (unsigned)*slot);
        return;
    }
    g_zw_png_load_orig = (zw_png_load_fn)*slot;
    uintptr_t w = (uintptr_t)zw_png_load_wrap;
    kuKernelCpuUnrestrictedMemcpy(slot, &w, sizeof(w));
    LOG("  v5.06 png loader wrapped\n");
}

static void hook_iap_symbols(so_module *mod) {
    static const struct {
        const char *name;
        uintptr_t replacement;
    } hooks[] = {
        /* v5.05: GetItemListCB / BuyItemCB run for real (driven by zw_iap_pump).
           The 4-byte C wrappers (InAppBilling_GetState etc. = one b.w to the C++ method)
           are NOT hooked: hook_addr writes more than 4 bytes and corrupted the next
           function (GetItemInfoByID wrapper overwrote getShopAttributeByName). Hooking
           the C++ method covers the wrapper. */
        { "_ZN12InAppBilling11getItemListEPKc", (uintptr_t)hook_InAppBilling_GetItemList_local },
        { "_ZN12InAppBilling16getItemInfoArrayEv", (uintptr_t)hook_InAppBilling_GetItemInfoArray_local },
        { "InAppBilling_FreeItemInfoArray", (uintptr_t)hook_InAppBilling_FreeItemInfoArray_local },
        { "_ZN14IAB_STORE_ITEM11getPropertyEPKc", (uintptr_t)hook_IAB_STORE_ITEM_getProperty },
        { "_ZN12InAppBilling10getAttByIDEPKcS1_", (uintptr_t)hook_InAppBilling_getAttByID_local },
        { "_ZN12InAppBilling22getShopAttributeByNameEPKc", (uintptr_t)hook_InAppBilling_getShopAttributeByName_local },
        { "_ZN12InAppBilling22sendNotifyConfirmationEPKc", (uintptr_t)hook_InAppBilling_sendNotifyConfirmation_local },
        { "_ZN12InAppBilling15getItemInfoByIDEPKcR14IAB_STORE_ITEM", (uintptr_t)hook_InAppBilling_return0 },
        { "_ZN12InAppBilling8getStateEv", (uintptr_t)hook_InAppBilling_GetState_local },
        { "_ZN12InAppBilling7buyItemEPKc", (uintptr_t)hook_InAppBilling_BuyItem_local },
        { "_ZN12InAppBilling19restoreTransactionsEv", (uintptr_t)hook_InAppBilling_void },
        { "_ZN12InAppBilling10showDialogEv", (uintptr_t)hook_InAppBilling_void },
        { "_ZN12InAppBilling13getTotalItemsEv", (uintptr_t)hook_InAppBilling_getTotalItems_local },
    };

    {
        uintptr_t cb = so_symbol(mod, "InAppBilling_GetItemListCB");
        if (cb) g_zw_iap_base = (cb & ~1u) - ZW_IAP_OFF_LIST_CB;
        LOGF("  v5.05 IAP local store base 0x%08X\n", (unsigned)g_zw_iap_base);
    }
    unsigned int hooked = 0;
    for (unsigned int i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
        uintptr_t sym = so_symbol(mod, hooks[i].name);
        if (!sym) {
            continue;
        }

        hook_addr(sym, hooks[i].replacement);
        hooked++;
        LOGF("  ✓ Hooked Android IAP %s at 0x%08X\n",
             hooks[i].name, (unsigned)sym);
    }

    if (hooked > 0) {
        so_flush_caches(mod);
    }
    LOGF("  ✓ Hooked %u Android IAP entry points\n", hooked);
}

/* ─────────────────────────────────────────────
   Hook / symbol redirect table
   All entries here are resolved by so_resolve().
   The game's imported symbol name on the left maps
   to our Vita-side replacement on the right.
   ───────────────────────────────────────────── */
extern so_default_dynlib g_default_dynlib[];
extern const int g_default_dynlib_count;
extern void dynlib_init_android_ctype(void);

static void android_kuser_memory_barrier(void) {
    __sync_synchronize();
}

static int android_kuser_cmpxchg(int oldval, int newval, volatile int *ptr) {
    return __sync_bool_compare_and_swap(ptr, oldval, newval) ? 0 : 1;
}

static void patch_android_kuser_helpers(so_module *mod, const char *name) {
    if (!mod || !mod->text_base || mod->text_size < sizeof(uint32_t)) {
        LOGF("  ! %s kuser patch skipped: no text segment\n", name ? name : "module");
        return;
    }

    uintptr_t membar = (uintptr_t)android_kuser_memory_barrier;
    uintptr_t cmpxchg = (uintptr_t)android_kuser_cmpxchg;
    uint32_t membar_value = (uint32_t)membar;
    uint32_t cmpxchg_value = (uint32_t)cmpxchg;
    unsigned int membar_hits = 0;
    unsigned int cmpxchg_hits = 0;

#ifdef VITA3K_BUILD
    sceKernelOpenVMDomain();
#endif
    for (size_t off = 0; off + sizeof(uint32_t) <= mod->text_size; off += sizeof(uint32_t)) {
        uintptr_t slot = mod->text_base + off;
        uint32_t value = *(uint32_t *)slot;
        if (value == 0xFFFF0FA0u) {
#ifdef VITA3K_BUILD
            memcpy((void *)slot, &membar_value, sizeof(membar_value));
#else
            kuKernelCpuUnrestrictedMemcpy((void *)slot, &membar_value, sizeof(membar_value));
#endif
            membar_hits++;
        } else if (value == 0xFFFF0FC0u) {
#ifdef VITA3K_BUILD
            memcpy((void *)slot, &cmpxchg_value, sizeof(cmpxchg_value));
#else
            kuKernelCpuUnrestrictedMemcpy((void *)slot, &cmpxchg_value, sizeof(cmpxchg_value));
#endif
            cmpxchg_hits++;
        }
    }
#ifdef VITA3K_BUILD
    sceKernelSyncVMDomain(0, (void *)mod->text_base, mod->text_size);
    sceKernelCloseVMDomain();
#endif
    so_flush_caches(mod);
    LOGF("  ✓ %s kuser sweep: membar=%u cmpxchg=%u -> %p / %p\n",
         name ? name : "module", membar_hits, cmpxchg_hits,
         (void *)membar, (void *)cmpxchg);
}

/* ─────────────────────────────────────────────
   Input handling — twin-stick shooter layout
   Left stick  = move character
   Right stick = aim / shoot direction
   Face buttons and triggers mapped below
   ───────────────────────────────────────────── */

/* Declared by the game's .so; we resolve these after so_load.
 * v4.49: real symbol is glf::AndroidAddTouchEvent(int action, int x, int y,
 * int pointerId) — all ints, Android MotionEvent actions (0 down, 1 up,
 * 2 move). The old lookup used the unmangled name "AndroidAddTouchEvent",
 * got NULL, and no input ever reached the game (it also passed floats). */
static void (*AndroidAddTouchEvent)(int action, int x, int y, int id) = NULL;

/* v4.91: glf's own convention, NOT Android MotionEvent: AndroidAddTouchEvent maps
 * 1 -> 0xd6 (press), 2 -> 0xd7 (move), 0 -> 0xd8 (release); FlashManager::OnEvent
 * clears the pressed flag on 0xd8. With 0/1 swapped every touch arrived as
 * release-then-press, so buttons (which fire on release) never triggered. */
#define ZW_TOUCH_DOWN 1
#define ZW_TOUCH_UP   0
#define ZW_TOUCH_MOVE 2
#define ZW_MAX_TOUCH  6
#define ZW_CROSS_TOUCH_ID 9

static SceUInt32 g_buttons_prev = 0;
static int g_touch_inited = 0;
static struct { int active, hw_id, x, y; } g_touch[ZW_MAX_TOUCH];

/* v4.92: why touches are dropped. Path: glf::AndroidAddTouchEvent -> App::GetInstance ->
 * GetInputMgr -> InputManager::GetGamepad(0) -> Gamepad::RaiseTouchEvent, which returns
 * early when gamepad+0x1c (touch count) <= index. FlashManager::OnEvent then requires
 * fm+0x30 != 0, fm+0x24 (RenderFX) != 0, flags(*0xcd2aa8)+0xb and +0x3b != 0. */
static void zw_touch_probe(int frame) {
    if (!g_zw_so_base) return;
    void *(*get_app)(void) = (void *)ZW_SO_FN(0x3a9fe8);
    void *(*get_imgr)(void *) = (void *)ZW_SO_FN(0x3aa0bc);
    void *(*get_pad)(void *, unsigned) = (void *)ZW_SO_FN(0x3c8138);
    void *app = get_app();
    void *im = app ? get_imgr(app) : NULL;
    uint8_t *pad = im ? (uint8_t *)get_pad(im, 0) : NULL;
    uint8_t *flags = *(uint8_t **)(*(void **)(g_zw_so_base + 0xcd2aa8));   /* GOT -> global -> struct */
    uint8_t *fm = *(uint8_t **)(*(void **)(g_zw_so_base + 0xcd2ad8));
    LOGF("  v4.92 touch probe f%d app=%p im=%p pad=%p pad.ntouch=%d pad.touches=%p | flags=%p +b=%d +3b=%d | fm=%p +30=%d rfx=%p\n",
         frame, app, im, pad, pad ? *(int *)(pad + 0x1c) : -9, pad ? *(void **)(pad + 0x20) : NULL,
         flags, flags ? flags[0xb] : -9, flags ? flags[0x3b] : -9,
         fm, fm ? fm[0x30] : -9, fm ? *(void **)(fm + 0x24) : NULL);
    if (app) {   /* v4.93: glf::App::ConvertPosDeviceToScreen = pt * disp->scale(+0xc), then orientation */
        int (*get_orient)(void *) = (void *)ZW_SO_FN(0x3aa1d0);
        uint8_t *(*get_disp)(void *) = (void *)ZW_SO_FN(0x3a9c64);
        uint32_t (*conv)(void *, const float *) = (void *)ZW_SO_FN(0x3aa328);
        uint8_t *disp = get_disp(app);
        float pt[2] = { 812.f, 370.f };
        uint32_t r = conv(app, pt);
        LOGF("  v4.93 touch conv f%d orient=%d disp=%p scale=%f -> (812,370) maps to (%d,%d)\n",
             frame, get_orient(app), disp, disp ? *(float *)(disp + 0xc) : -1.f,
             (int)(int16_t)(r & 0xffff), (int)(int16_t)(r >> 16));
    }
}

static void handle_input(void) {
    if (!AndroidAddTouchEvent) return;

    if (!g_touch_inited) {
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
        memset(g_touch, 0, sizeof(g_touch));
        g_touch_inited = 1;
        LOG("  v4.49 input: front touch + Cross tap enabled\n");
    }

    /* ── Front touchscreen: 1920x1088 panel units -> 960x544 game pixels ── */
    SceTouchData td;
    memset(&td, 0, sizeof(td));
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1) < 0) td.reportNum = 0;

    int seen[ZW_MAX_TOUCH] = {0};
    for (int r = 0; r < (int)td.reportNum && r < ZW_MAX_TOUCH; r++) {
        int hw = td.report[r].id;
        int x = td.report[r].x / 2, y = td.report[r].y / 2;
        int slot = -1, free_slot = -1;
        for (int i = 0; i < ZW_MAX_TOUCH; i++) {
            if (g_touch[i].active && g_touch[i].hw_id == hw) { slot = i; break; }
            if (!g_touch[i].active && free_slot < 0) free_slot = i;
        }
        if (slot < 0) {
            if (free_slot < 0) continue;
            slot = free_slot;
            g_touch[slot].active = 1;
            g_touch[slot].hw_id = hw;
            AndroidAddTouchEvent(ZW_TOUCH_DOWN, x, y, slot);
            static unsigned n90;
            if (n90++ < 30) LOGF("  v4.90 touch DOWN f%d raw=%d,%d -> %d,%d slot=%d hw=%d\n",
                                 g_main_loop_frame, td.report[r].x, td.report[r].y, x, y, slot, hw);
        } else if (x != g_touch[slot].x || y != g_touch[slot].y) {
            AndroidAddTouchEvent(ZW_TOUCH_MOVE, x, y, slot);
        }
        g_touch[slot].x = x;
        g_touch[slot].y = y;
        seen[slot] = 1;
        g_dbg_touch_x = x; g_dbg_touch_y = y; g_dbg_touch_events++;
    }
    g_dbg_touch_down = (int)td.reportNum;
    {   /* v4.95: automatic touch test when ux0:data/zombiewood/autotouch.txt exists:
         * every 60 frames from 600..1200 press at (100,450), release 10 frames later,
         * and log EventManager pending-touch counter (+4) + queue ptrs, and RenderFX slot 0. */
        static int at = -1;
        if (at < 0) { SceIoStat st; at = ZW_DEBUG_BUILD && sceIoGetstat("ux0:data/zombiewood/autotouch.txt", &st) >= 0; if (at) LOG("  v4.95 autotouch armed\n"); }
        int fr = g_main_loop_frame;
        if (at && fr >= 600 && fr < 1200 && g_zw_so_base) {
            int ph = fr % 60;
            if (ph == 0) AndroidAddTouchEvent(ZW_TOUCH_DOWN, 100, 450, 7);
            if (ph == 10) AndroidAddTouchEvent(ZW_TOUCH_UP, 100, 450, 7);
            if (ph == 1 || ph == 5 || ph == 11 || ph == 30) {
                void *(*get_app)(void) = (void *)ZW_SO_FN(0x3a9fe8);
                void *(*get_em)(void *) = (void *)ZW_SO_FN(0x3aa008);
                void *app = get_app();
                uint8_t *em = app ? (uint8_t *)get_em(app) : NULL;
                uint8_t *fm = *(uint8_t **)(*(void **)(g_zw_so_base + 0xcd2ad8));
                uint8_t *rfx = fm ? *(uint8_t **)(fm + 0x24) : NULL;
                LOGF("  v4.95 autotouch f%d ph%d em=%p pending=%d q24=%p q28=%p q2c=%p q30=%p | rfx s0=(%.0f,%.0f p%d id%d)\n",
                     fr, ph, em, em ? *(int *)(em + 4) : -9,
                     em ? *(void **)(em + 0x24) : NULL, em ? *(void **)(em + 0x28) : NULL,
                     em ? *(void **)(em + 0x2c) : NULL, em ? *(void **)(em + 0x30) : NULL,
                     rfx ? *(float *)(rfx + 0x118) : -1.f, rfx ? *(float *)(rfx + 0x11c) : -1.f,
                     rfx ? rfx[0x120] : -9, rfx ? *(int *)(rfx + 0x124) : -9);
            }
        }
    }
    {   /* v4.94: did FlashManager::OnEvent copy the pointer into RenderFX (+0x118 + slot*16)? */
        static unsigned n94; static int prev_down;
        int down = td.reportNum > 0;
        if ((down || prev_down) && n94 < 24 && g_zw_so_base) {
            uint8_t *fm = *(uint8_t **)(*(void **)(g_zw_so_base + 0xcd2ad8));
            uint8_t *rfx = fm ? *(uint8_t **)(fm + 0x24) : NULL;
            if (rfx && (n94 < 4 || (g_main_loop_frame & 7) == 0 || down != prev_down)) {
                n94++;
                LOGF("  v4.94 rfx ptr f%d down=%d s0=(%.0f,%.0f p%d id%d) s1=(%.0f,%.0f p%d id%d) ids=%d,%d,%d,%d\n",
                     g_main_loop_frame, down,
                     *(float *)(rfx + 0x118), *(float *)(rfx + 0x11c), rfx[0x120], *(int *)(rfx + 0x124),
                     *(float *)(rfx + 0x128), *(float *)(rfx + 0x12c), rfx[0x130], *(int *)(rfx + 0x134),
                     *(int *)(rfx + 0x124), *(int *)(rfx + 0x134), *(int *)(rfx + 0x144), *(int *)(rfx + 0x154));
            }
        }
        prev_down = down;
    }
    for (int i = 0; i < ZW_MAX_TOUCH; i++) {
        if (g_touch[i].active && !seen[i]) {
            AndroidAddTouchEvent(ZW_TOUCH_UP, g_touch[i].x, g_touch[i].y, i);
            g_touch[i].active = 0;
        }
    }

    /* ── Cross = tap at screen centre (e.g. "tap to continue") ── */
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    zw_read_sticks(&pad);   /* v4.80 */
    {   /* v4.84 test aid: hold the aim stick for frames 900..2400 when autofire.txt exists */
        static int af = -1;
        if (af < 0) { SceIoStat st; af = ZW_DEBUG_BUILD && sceIoGetstat("ux0:data/zombiewood/autofire.txt", &st) >= 0; if (af) LOG("  v4.84 autofire test armed\n"); }
        if (af && g_main_loop_frame >= 900 && g_main_loop_frame < 2400) {
            g_zw_stick[2] = 0.7f; g_zw_stick[3] = 0.7f;
            /* v4.86: also walk (the open crash needs movement); circle so the player doesn't pin to a wall */
            float t = (float)(g_main_loop_frame - 900) * 0.02f;
            g_zw_stick[0] = 0.8f * cosf(t); g_zw_stick[1] = 0.8f * sinf(t);
        }
    }
    zw_apply_sticks();      /* v4.82 */
    { static int zn; if ((pad.lx < 64 || pad.lx > 192 || pad.ly < 64 || pad.ly > 192 || pad.rx < 64 || pad.rx > 192 || pad.ry < 64 || pad.ry > 192) && zn < 6) { zn++; LOGF("  v4.81 raw pad lx=%d ly=%d rx=%d ry=%d buttons=0x%x\n", pad.lx, pad.ly, pad.rx, pad.ry, (unsigned)pad.buttons); } }
    SceUInt32 pressed  = pad.buttons & ~g_buttons_prev;
    SceUInt32 released = g_buttons_prev & ~pad.buttons;
    zw_hud_buttons(pressed);   /* v5.08 */
    if (pressed & SCE_CTRL_CROSS) {
        AndroidAddTouchEvent(ZW_TOUCH_DOWN, 480, 272, ZW_CROSS_TOUCH_ID);
        LOG("  v4.49 input: Cross -> tap down (480,272)\n");
        g_dbg_cross_count++;
    } else if (released & SCE_CTRL_CROSS) {
        AndroidAddTouchEvent(ZW_TOUCH_UP, 480, 272, ZW_CROSS_TOUCH_ID);
    }
    if (ZW_DEBUG_BUILD && (pressed & SCE_CTRL_SELECT)) g_overlay_on = !g_overlay_on;   /* v4.50; v5.10: debug builds only */
    /* v4.80: debug buttons (START trace, R, L, D-pad) only act while the overlay is shown,
     * so they cannot be hit by accident during play. */
    if (!g_overlay_on) pressed &= SCE_CTRL_SELECT | SCE_CTRL_CROSS;
    if (pressed & SCE_CTRL_START) g_zw_trace_request = 2;          /* v4.76: GL trace + screenshot of the next frame */
    if (pressed & SCE_CTRL_RTRIGGER) {                             /* v4.56 */
        extern volatile int g_zw_force_nodepth;
        g_zw_force_nodepth = !g_zw_force_nodepth;
        if (g_zw_force_nodepth) glDisable(GL_DEPTH_TEST);
        LOGF("  v4.56 R: force depth test off = %d\n", g_zw_force_nodepth);
    }
    if (pressed & SCE_CTRL_UP) {                                   /* v4.56 */
        extern volatile int g_zw_force_nostencil;
        g_zw_force_nostencil = !g_zw_force_nostencil;
        if (g_zw_force_nostencil) glDisable(GL_STENCIL_TEST);
        LOGF("  v4.56 UP: force stencil test off = %d\n", g_zw_force_nostencil);
    }
    if (pressed & SCE_CTRL_DOWN) {                                 /* v4.61 */
        extern volatile GLenum g_zw_depthfunc_override;
        extern void zw_apply_depthfunc_override(void);
        static const GLenum cyc[] = { 0, 1, GL_GEQUAL, GL_GREATER, GL_NOTEQUAL, GL_ALWAYS };
        static int ci = 1;   /* v4.72: starts at GAME */
        ci = (ci + 1) % 6;
        g_zw_depthfunc_override = cyc[ci];
        zw_apply_depthfunc_override();
        LOGF("  v4.61 DOWN: depth func override = 0x%x\n", (unsigned)g_zw_depthfunc_override);
    }
    if (pressed & SCE_CTRL_LTRIGGER) {                             /* v4.55 */
        extern volatile int g_zw_skip_quads;
        g_zw_skip_quads = !g_zw_skip_quads;
        LOGF("  v4.55 L: skip fullscreen quads = %d\n", g_zw_skip_quads);
    }
    g_buttons_prev = pad.buttons;
}

/* ─────────────────────────────────────────────
   v4.50: on-screen debug overlay (SELECT toggles).
   5x7 column font (bit0 = top row), rendered to an RGBA texture and drawn
   with VitaGL fixed-function after the game renders. GL state is saved and
   restored so glitch's cached state stays valid.
   ───────────────────────────────────────────── */
static const uint8_t zw_font5x7[64][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14}, /*  !"# */
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00}, /* $%&' */
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08}, /* ()*+ */
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02}, /* ,-./ */
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31}, /* 0123 */
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03}, /* 4567 */
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00}, /* 89:; */
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06}, /* <=>? */
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22}, /* @ABC */
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A}, /* DEFG */
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41}, /* HIJK */
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E}, /* LMNO */
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31}, /* PQRS */
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F}, /* TUVW */
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00}, /* XYZ[ */
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40}, /* \]^_ */
};

#define ZW_OV_COLS  80
#define ZW_OV_LINES 6
#define ZW_OV_W     (ZW_OV_COLS * 6)   /* 480 px, drawn at 2x = 960 */
#define ZW_OV_H     (ZW_OV_LINES * 9)  /* 54 px,  drawn at 2x = 108 */
#define ZW_OV_TEX_W 512
#define ZW_OV_TEX_H 64
static uint32_t g_ov_pixels[ZW_OV_TEX_W * ZW_OV_TEX_H];
static char g_ov_lines[ZW_OV_LINES][ZW_OV_COLS + 1];

static void zw_ov_text(int line, const char *s) {
    for (int c = 0; c < ZW_OV_COLS && s[c]; c++) {
        int ch = (unsigned char)s[c];
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < 32 || ch > 95) ch = '?';
        const uint8_t *g = zw_font5x7[ch - 32];
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 7; row++)
                if (g[col] & (1u << row))
                    g_ov_pixels[(line * 9 + 1 + row) * ZW_OV_TEX_W + c * 6 + col] = 0xFF7FFF7Fu; /* light green (ABGR) */
    }
}

/* Find the vtable's class name from the .so's dynsym (_ZTVnnName -> Name). */
static const char *zw_vtable_name(const void *vt) {
    static const void *cached_vt = NULL;
    static char cached[48] = "?";
    if (!vt || !g_zw_mod || !g_zw_mod->dynsym) return "?";
    if (vt == cached_vt) return cached;
    cached_vt = vt;
    snprintf(cached, sizeof(cached), "+0x%X", (unsigned)((uintptr_t)vt - g_zw_text_base));
    uintptr_t off = (uintptr_t)vt - g_zw_text_base;
    for (int i = 0; i < g_zw_mod->num_dynsym; i++) {
        const Elf32_Sym *s = &g_zw_mod->dynsym[i];
        if (!s->st_name || !s->st_size) continue;
        if (off < s->st_value || off >= s->st_value + s->st_size) continue;
        const char *n = g_zw_mod->dynstr + s->st_name;
        if (strncmp(n, "_ZTV", 4) == 0) {
            n += 4;
            while (*n >= '0' && *n <= '9') n++;
        }
        snprintf(cached, sizeof(cached), "%s", n);
        break;
    }
    return cached;
}

static int zw_ptr_ok(const void *p) {
    return (uintptr_t)p >= 0x80000000u && (uintptr_t)p < 0xA0000000u;
}

static void zw_overlay_draw(int frame, GLenum gl_err) {
    if (!g_overlay_on) return;

    /* fps over ~1 s */
    static SceUInt64 last_t = 0;
    static int last_frame = 0;
    static float fps = 0.0f;
    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (last_t == 0) { last_t = now; last_frame = frame; }
    if (now - last_t >= 1000000) {
        fps = (frame - last_frame) * 1000000.0f / (float)(now - last_t);
        last_t = now; last_frame = frame;
    }

    /* draws in the last frame */
    static unsigned prev_arr = 0, prev_elem = 0;
    unsigned d_arr = g_gl_draw_arrays_count - prev_arr, d_elem = g_gl_draw_elements_count - prev_elem;
    prev_arr = g_gl_draw_arrays_count; prev_elem = g_gl_draw_elements_count;

    void *st = (void *)g_zw_cur_state;
    const char *sname = zw_vtable_name((const void *)g_zw_cur_vtable);
    int f11c = -1, f11d = -1;
    if (zw_ptr_ok(st)) {
        f11c = *(volatile uint8_t *)((uintptr_t)st + 0x11c);
        f11d = *(volatile uint8_t *)((uintptr_t)st + 0x11d);
    }
    int menu_stage = -1;
    void *zg = (g_ZombiesGame_singleton_ptr && zw_ptr_ok(g_ZombiesGame_singleton_ptr))
               ? *g_ZombiesGame_singleton_ptr : NULL;
    if (zw_ptr_ok(zg)) {
        void *menu = *(void **)((uintptr_t)zg + 0x1c8);
        if (zw_ptr_ok(menu)) menu_stage = *(volatile int *)((uintptr_t)menu + 0x10);
    }

    snprintf(g_ov_lines[0], ZW_OV_COLS + 1, "ZW V4.64  FRAME %d  FPS %.1f  GLERR 0x%04X   (SELECT HIDES)", frame, fps, (unsigned)gl_err);
    extern volatile GLenum g_zw_depthfunc_override;
    {
        GLenum df = g_zw_depthfunc_override;
        const char *dfn = df == 0 ? "AUTO(LEQUAL->NOTEQUAL)" : df == 1 ? "GAME" : df == GL_GEQUAL ? "GEQUAL" : df == GL_GREATER ? "GREATER" :
                          df == GL_NOTEQUAL ? "NOTEQUAL" : df == GL_ALWAYS ? "ALWAYS" : "?";
        snprintf(g_ov_lines[1], ZW_OV_COLS + 1, "STATE %s   [DOWN] DEPTH FUNC: %s", sname, dfn);
    }
    long long zg_timer = zw_ptr_ok(zg) ? *(volatile long long *)((uintptr_t)zg + 0x118) : -1;
    snprintf(g_ov_lines[2], ZW_OV_COLS + 1, "LOADED(11C)=%d  11D=%d  CMENU STAGE=%d  SPLASH TIMER %lld MS (NEEDS >3000)",
             f11c, f11d, menu_stage, zg_timer);
    extern volatile int g_zw_skip_quads;
    extern volatile GLuint g_zw_quad_progs[4];
    snprintf(g_ov_lines[3], ZW_OV_COLS + 1, "DRAWS: ARR %u ELEM %u  QUAD PROGS %u %u %u  [L] SKIP QUADS: %s",
             d_arr, d_elem, g_zw_quad_progs[0], g_zw_quad_progs[1], g_zw_quad_progs[2],
             g_zw_skip_quads ? "ON" : "OFF");
    extern volatile int g_zw_force_nodepth, g_zw_force_nostencil;
    snprintf(g_ov_lines[4], ZW_OV_COLS + 1, "TOUCH %d %d,%d EV %u X %u  [R] NO DEPTH: %s  [UP] NO STENCIL: %s",
             g_dbg_touch_down, g_dbg_touch_x, g_dbg_touch_y, g_dbg_touch_events, g_dbg_cross_count,
             g_zw_force_nodepth ? "ON" : "OFF", g_zw_force_nostencil ? "ON" : "OFF");
    /* v4.51: GSLoading post-load waits on ZombiesGame+0x1c4 (CLevel*) ->
     * vtable+0x10 CLevel::Loading(); its step is CLevel+0x2ec. */
    const char *lvl_name = "-";
    int lvl_stage = -1;
    if (zw_ptr_ok(zg)) {
        void *lvl = *(void **)((uintptr_t)zg + 0x1c4);
        if (zw_ptr_ok(lvl) && zw_ptr_ok(*(void **)lvl)) {
            static char lname[48];
            snprintf(lname, sizeof(lname), "%s", zw_vtable_name(*(void **)lvl));
            lvl_name = lname;
            lvl_stage = *(volatile int *)((uintptr_t)lvl + 0x2ec);
        }
    }
    (void)lvl_name; (void)lvl_stage;
    snprintf(g_ov_lines[5], ZW_OV_COLS + 1, "MS: UPD %llu (%u DRAWS) REND %llu (%u DRAWS) SWAP %llu WORST %llu",
             g_t_step / 1000, g_draws_step, g_t_render / 1000, g_draws_render, g_t_swap / 1000, g_t_frame_max / 1000);
    { static int reset_ctr = 0; if (++reset_ctr >= 60) { reset_ctr = 0; g_t_frame_max = 0; } }

    for (int i = 0; i < ZW_OV_TEX_W * ZW_OV_TEX_H; i++)
        g_ov_pixels[i] = ((i % ZW_OV_TEX_W) < ZW_OV_W && (i / ZW_OV_TEX_W) < ZW_OV_H) ? 0xC0000000u : 0;
    for (int l = 0; l < ZW_OV_LINES; l++) zw_ov_text(l, g_ov_lines[l]);

    /* v4.50b: write straight into the displayed framebuffer (no GL at all:
     * VitaGL immediate mode crashed in glVertex3f). 2x scale, darkened bg. */
    /* v4.50c: right after vglSwapBuffers the display still shows the previous
     * buffer; wait (max 3 vblanks) for the flip so we draw into the new one. */
    static void *last_base = NULL;
    static int fb_logged = 0;
    SceDisplayFrameBuf fb;
    int rc = -1;
    for (int tries = 0; tries < 1; tries++) {   /* v4.58: no vblank waits (cost fps) */
        memset(&fb, 0, sizeof(fb));
        fb.size = sizeof(fb);
        rc = sceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_IMMEDIATE);
        break;   /* v4.75: never wait for vblank here */
    }
    if (fb_logged < 6) {
        fb_logged++;
        LOGF("  v4.50c overlay fb rc=0x%x base=%p prev=%p pf=0x%x %ux%u pitch=%u\n",
             (unsigned)rc, fb.base, last_base, (unsigned)fb.pixelformat,
             (unsigned)fb.width, (unsigned)fb.height, (unsigned)fb.pitch);
    }
    if (rc < 0 || !fb.base) return;
    last_base = fb.base;
    if (fb.pixelformat != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8) return;
    uint32_t *dst = (uint32_t *)fb.base;
    int fw = (int)fb.width, fh = (int)fb.height, pitch = (int)fb.pitch;
    for (int y = 0; y < ZW_OV_H * 2 && y < fh; y++) {
        uint32_t *row = dst + y * pitch;
        const uint32_t *src = g_ov_pixels + (y / 2) * ZW_OV_TEX_W;
        for (int x = 0; x < ZW_OV_W * 2 && x < fw; x++) {
            uint32_t sp = src[x / 2];
            if (sp == 0xFF7FFF7Fu) row[x] = sp;                    /* text */
            else row[x] = 0xFF181818u;   /* v4.75: solid bg - reading the framebuffer back (uncached VRAM) was the 52 ms */
        }
    }
}

/* v4.50: main.c logger exported for dynlib.c (single fd, no offset races). */
void zw_log_raw(const char *s) { log_write(s); }

typedef struct GameThreadArgs_ {
    int argc;
    char **argv;
} GameThreadArgs;

/* ─────────────────────────────────────────────
   Game entry point. Run from a large pthread like MC3.
   ───────────────────────────────────────────── */
/* v4.65: depth/stencil READBACK diagnostic (hardware only).
 * VitaGL's display depth/stencil surface is force-stored at scene end so the
 * real buffer contents can be inspected after vglSwapBuffers. */
#ifndef VITA3K_BUILD
extern SceGxmDepthStencilSurface gxm_depth_stencil_surface;   /* VitaGL internal */
extern SceGxmContext *gxm_context;                            /* VitaGL internal */
#define ZW_DS_DIAG 0        /* 1 = force-store depth/stencil and log histograms on probe frames */
#define ZW_DS_STRIDE 1920   /* 960 * 2 (4x MSAA) */
#define ZW_DS_ROWS   1088   /* 544 * 2 */
/* half-res RGB24 dump of the displayed framebuffer -> ux0:data/zombiewood/shots */
static void zw_ds_shot(const char *tag, int frame) {
    static uint8_t rgb[480 * 272 * 3];
    SceDisplayFrameBuf fb;
    sceGxmFinish(gxm_context);
    sceGxmDisplayQueueFinish();
    memset(&fb, 0, sizeof(fb)); fb.size = sizeof(fb);
    if (sceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_IMMEDIATE) < 0 || !fb.base) return;
    const uint8_t *src = (const uint8_t *)fb.base;
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++) {
            const uint8_t *px = src + ((y * 2) * fb.pitch + x * 2) * 4;
            uint8_t *o = rgb + (y * 480 + x) * 3;
            o[0] = px[0]; o[1] = px[1]; o[2] = px[2];
        }
    char path[128];
    sceIoMkdir("ux0:data/zombiewood/shots", 0777);
    snprintf(path, sizeof(path), "ux0:data/zombiewood/shots/f%04d_%s.raw", frame, tag);
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) { sceIoWrite(fd, rgb, sizeof(rgb)); sceIoClose(fd); }
    LOGF("  v4.66 shot %s fd=0x%x\n", path, (unsigned)fd);
}
static int zw_ds_brief = 0;
static void zw_ds_probe(const char *tag, int frame) {
    const uint32_t *s = (const uint32_t *)&gxm_depth_stencil_surface;
    const uint32_t *d = (const uint32_t *)s[1];
    const uint8_t *st = (const uint8_t *)s[2];
    sceGxmFinish(gxm_context);
    LOGF("  v4.65 DS[%s] frame %d surf=%08x %08x %08x %08x %08x\n", tag, frame,
         (unsigned)s[0], (unsigned)s[1], (unsigned)s[2], (unsigned)s[3], (unsigned)s[4]);
    if (!d) return;
    enum { NV = 12 };
    uint32_t vals[NV]; unsigned cnt[NV]; int nv = 0; unsigned other = 0, total = 0, maskset = 0;
    unsigned b[7] = {0};   /* ==0, (0,.5), [.5,.9), [.9,.99), [.99,1), ==1, >1/nan */
    float mn = 1e30f, mx = -1e30f;
    for (int y = 0; y < ZW_DS_ROWS; y += 4) {
        for (int x = 0; x < ZW_DS_STRIDE; x += 4) {
            uint32_t raw = d[y * ZW_DS_STRIDE + x];
            total++;
            if (raw & 0x80000000u) maskset++;
            union { uint32_t u; float f; } c; c.u = raw & 0x7FFFFFFFu;
            float f = c.f;
            if (f < mn) mn = f;
            if (f > mx) mx = f;
            if (c.u == 0) b[0]++; else if (f < 0.5f) b[1]++; else if (f < 0.9f) b[2]++;
            else if (f < 0.99f) b[3]++; else if (f < 1.0f) b[4]++; else if (f == 1.0f) b[5]++; else b[6]++;
            int i; for (i = 0; i < nv; i++) if (vals[i] == raw) { cnt[i]++; break; }
            if (i == nv) { if (nv < NV) { vals[nv] = raw; cnt[nv] = 1; nv++; } else other++; }
        }
    }
    LOGF("  v4.65 DS[%s] depth n=%u mask=%u min=%f max=%f | ==0:%u <.5:%u <.9:%u <.99:%u <1:%u ==1:%u >1:%u\n",
         tag, total, maskset, mn, mx, b[0], b[1], b[2], b[3], b[4], b[5], b[6]);
    for (int i = 0; i < nv && !zw_ds_brief; i++)
        LOGF("  v4.65 DS[%s]   raw %08x x%u\n", tag, (unsigned)vals[i], cnt[i]);
    LOGF("  v4.65 DS[%s]   other-raw x%u; centre raws %08x %08x %08x %08x\n", tag, other,
         (unsigned)d[544 * ZW_DS_STRIDE + 960], (unsigned)d[544 * ZW_DS_STRIDE + 961],
         (unsigned)d[545 * ZW_DS_STRIDE + 960], (unsigned)d[300 * ZW_DS_STRIDE + 400]);
    if (st) {
        static unsigned h[256];
        memset(h, 0, sizeof(h));
        for (int y = 0; y < ZW_DS_ROWS; y += 4)
            for (int x = 0; x < ZW_DS_STRIDE; x += 4)
                h[st[y * ZW_DS_STRIDE + x]]++;
        char line[400]; int o = 0;
        line[0] = 0;
        for (int i = 0; i < 256 && o < 360; i++)
            if (h[i]) o += snprintf(line + o, sizeof(line) - o, " %d:%u", i, h[i]);
        LOGF("  v4.65 DS[%s] stencil%s\n", tag, line);
    }
}
/* probe schedule: 0 none, 1 clear-only 1.0, 2 clear-only 0.25,
 * 3 game frame GAME func + game stencil, 4 game frame ALWAYS, 5 game frame defaults */
static int zw_ds_probe_kind(int frame) {
    /* v4.76: screenshot on START (v4.79: plus one at frame 900); v5.18: debug builds only */
    return (ZW_DEBUG_BUILD && (g_zw_trace_request || frame == 900)) ? 5 : 0;
}
#endif

static void zw_check_game_files(void);   /* v5.13, defined before main() */
static void zw_check_game_data(void);    /* v5.18, defined before main() */
#define ZW_GAME_DIR "ux0:data/zombiewood/"
static int game_main(int argc, char *argv[]) {
    /* ── 1. Debug log ── */
    log_init();
#if ZW_CALLPROF
    LOG("=== Zombiewood Vita Loader v5.18-callprof (MEASUREMENT BUILD) ===\n");
#else
    LOG("=== Zombiewood Vita Loader v5.18 ===\n");
#endif
    LOG("Starting initialization sequence\n");
    
    /* ── 1b. Init fake JNI env table ── */
    jni_fake_env_init();
    log_jni_table_layout();
    LOG("JNI environment initialized\n");

    /* ── 2. Max out CPU/bus clocks, keep the device awake ── */
    zw_start_power_tick();
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    LOG("Performance: CPU=444MHz  BUS=222MHz  GPU=222MHz\n");

    /* ── 3. Controller / touch init ── */
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    LOG("Controller initialized\n");

    /* ── 4. Load .so files ── */
    static so_module generator_mod, zombiewood_mod;
    
    LOG("=== .SO LOADING STAGE ===\n");
    zw_check_game_files();   /* v5.13: stops at an error screen if a game file is missing or not 1.5.3 */
    zw_check_game_data();    /* v5.18: stops at an error screen if the unpacked data is incomplete */

    LOG("  [1/4] Loading libgenerator.so from ux0:data/zombiewood/\n");
    int gen_result = so_load(&generator_mod, ZW_GAME_DIR "libgenerator.so", LOAD_ADDR_GENERATOR);
    if (gen_result < 0) {
        LOGF("  WARNING: libgenerator.so load failed (0x%08X)\n", gen_result);
        LOG ("  This may not be fatal if it's not required\n");
    } else {
        LOG("  \xE2\x9C\x93 libgenerator.so loaded\n");
    }

    LOG("  [2/4] Loading libZombieWood.so from ux0:data/zombiewood/\n");
    int zw_result = so_load(&zombiewood_mod, ZW_GAME_DIR "libZombieWood.so", LOAD_ADDR_ZOMBIEWOOD);
    if (zw_result < 0) {
        LOGF("  FATAL: libZombieWood.so failed to load (0x%08X)\n", zw_result);
        LOG ("  Cannot continue without main game binary\n");
        zw_log_flush(); if (g_log_fd >= 0) sceIoClose(g_log_fd);
        sceKernelExitProcess(1);
        return 1;
    } else {
        LOG("  libZombieWood.so loaded\n");
    }

    /* ── 5. Register our hook table, then relocate (applies to all loaded modules) ── */
    LOG("=== SYMBOL RESOLUTION STAGE ===\n");
    LOGF("  [3/4] Resolving symbols in libgenerator.so with %d entries (%d bytes)\n",
        (int)(g_default_dynlib_count / sizeof(so_default_dynlib)),
        g_default_dynlib_count);
    
    int gen_resolve_result = so_resolve(&generator_mod, g_default_dynlib, 
                                         g_default_dynlib_count, 0);
    if (gen_resolve_result < 0) {
        LOGF("  ! so_resolve failed for libgenerator (code: %d) - continuing anyway\n", gen_resolve_result);
    } else {
        LOG("  ✓ libgenerator symbols resolved\n");
    }
    patch_import_symbol(&generator_mod, "qsort", (uintptr_t)vita_qsort);
    
    LOG("  [4/4] Relocating libgenerator.so in memory\n");
    if (so_relocate(&generator_mod) < 0) {
        LOG("  ! libgenerator relocation failed - continuing anyway\n");
    } else {
        LOG("  ✓ libgenerator relocation complete\n");
    }

    LOGF("  [5/4] Resolving symbols in libZombieWood.so with %d entries (%d bytes)\n",
        (int)(g_default_dynlib_count / sizeof(so_default_dynlib)),
        g_default_dynlib_count);
    int zw_resolve_result = so_resolve(&zombiewood_mod, g_default_dynlib, 
                                        g_default_dynlib_count, 0);
    if (zw_resolve_result < 0) {
        LOGF("  ✗ FATAL: so_resolve failed for libZombieWood (code: %d)\n", zw_resolve_result);
        LOG("  Game binary cannot be patched. Check debug log for missing symbols\n");
        zw_log_flush(); if (g_log_fd >= 0) sceIoClose(g_log_fd);
        sceKernelExitProcess(1);
        return 1;
    }
    LOG("  ✓ libZombieWood symbols resolved\n");
    patch_import_symbol(&zombiewood_mod, "qsort", (uintptr_t)vita_qsort);

    LOG("  [6/4] Relocating libZombieWood.so in memory\n");
    if (so_relocate(&zombiewood_mod) < 0) {
        LOG("  ✗ FATAL: libZombieWood relocation failed\n");
        LOG("  Apply relative address patches failed. Binary may be corrupted\n");
        zw_log_flush(); if (g_log_fd >= 0) sceIoClose(g_log_fd);
        sceKernelExitProcess(1);
        return 1;
    }
    LOG("  ✓ libZombieWood relocation complete\n");

    patch_android_kuser_helpers(&generator_mod, "libgenerator");
    patch_android_kuser_helpers(&zombiewood_mod, "libZombieWood");

    dynlib_init_android_ctype();
    LOG("  ✓ Android libc _ctype_ compatibility table ready\n");

    /* ── 5a. Run .init_array — every static C++ constructor in the .so.
            Without this, every Singleton<T>, every global object's vtable
            and every global mutex stays zero-initialized — which is what
            caused the prior data-abort at libZombieWood+0x3a52bc inside
            glf::DebuggerMutex::Lock (this == NULL because the Debugger
            singleton was never actually constructed). */
    /* Custom inline init_array runner with per-entry logging — if a
       constructor crashes, the last [N/M] line in the log tells us
       exactly which entry, so we can disassemble and fix the next miss. */
    LOG("=== STATIC INITIALIZATION STAGE ===\n");

    LOGF("  libgenerator: %d static constructors\n", generator_mod.num_init_array);
    for (int i = 0; i < generator_mod.num_init_array; i++) {
        if (generator_mod.init_array[i]) {
#ifndef VITA3K_BUILD
            LOGF("    libgen [%d/%d] = %p (running...)\n",
                 i + 1, generator_mod.num_init_array,
                 (void*)generator_mod.init_array[i]);
#endif
            generator_mod.init_array[i]();
#ifndef VITA3K_BUILD
            LOGF("    libgen [%d/%d] OK\n", i + 1, generator_mod.num_init_array);
#endif
        }
    }
    LOG("  ✓ libgenerator static constructors done\n");

    LOGF("  libZombieWood: %d static constructors\n", zombiewood_mod.num_init_array);
    for (int i = 0; i < zombiewood_mod.num_init_array; i++) {
        if (zombiewood_mod.init_array[i]) {
            uintptr_t init_addr = (uintptr_t)zombiewood_mod.init_array[i];
            uintptr_t init_off = (init_addr & ~(uintptr_t)1) - zombiewood_mod.text_base;
            if (init_off == 0x770540) {
                LOGF("    libzw  [%d/%d] = %p skipped (GlotEvents protobuf descriptor ctor)\n",
                     i + 1, zombiewood_mod.num_init_array,
                     (void*)zombiewood_mod.init_array[i]);
                continue;
            }
#ifndef VITA3K_BUILD
            LOGF("    libzw  [%d/%d] = %p (running...)\n",
                 i + 1, zombiewood_mod.num_init_array,
                 (void*)zombiewood_mod.init_array[i]);
#endif
            zombiewood_mod.init_array[i]();
#ifndef VITA3K_BUILD
            LOGF("    libzw  [%d/%d] OK\n", i + 1, zombiewood_mod.num_init_array);
#endif
        }
    }
    LOG("  ✓ libZombieWood static constructors done\n");

    /* ── 5b. Patch the game's NVThreadGetCurrentJNIEnv so it returns
            our fake env, instead of dereferencing an uninitialized
            JavaVM global (the cause of the prior data-abort at +0x360b80). */
    LOG("=== JNI HOOKUP STAGE ===\n");
    {
        uintptr_t nv_sym = so_symbol(&zombiewood_mod, "NVThreadGetCurrentJNIEnv");
        if (nv_sym) {
            hook_addr(nv_sym, (uintptr_t)hook_NVThreadGetCurrentJNIEnv);
            so_flush_caches(&zombiewood_mod);
            LOGF("  ✓ Hooked NVThreadGetCurrentJNIEnv at 0x%08X\n", (unsigned)nv_sym);
        } else {
            LOG("  ! NVThreadGetCurrentJNIEnv symbol not found — JNI helpers may crash\n");
        }

        uintptr_t android_env_sym = so_symbol(&zombiewood_mod, "AndroidOS_GetEnv");
        if (android_env_sym) {
            hook_addr(android_env_sym, (uintptr_t)hook_AndroidOS_GetEnv);
            so_flush_caches(&zombiewood_mod);
            LOGF("  ✓ Hooked AndroidOS_GetEnv at 0x%08X\n", (unsigned)android_env_sym);
        } else {
            LOG("  ! AndroidOS_GetEnv symbol not found\n");
        }

        uintptr_t view_settings_sym = so_symbol(&zombiewood_mod, "_ZN3glf22AndroidGetViewSettingsEv");
        if (view_settings_sym) {
            LOGF("  Leaving AndroidGetViewSettings original at 0x%08X so it can create GLF/App/ZombiesGame\n",
                 (unsigned)view_settings_sym);
        } else {
            LOG("  ! AndroidGetViewSettings symbol not found — appInit may crash\n");
        }

        uintptr_t set_internet_sym = so_symbol(&zombiewood_mod, "_ZN11ZombiesGame11SetInternetEb");
        if (set_internet_sym) {
            hook_addr(set_internet_sym, (uintptr_t)hook_ZombiesGame_SetInternet);
            so_flush_caches(&zombiewood_mod);
            LOGF("  ✓ Hooked null-safe ZombiesGame::SetInternet at 0x%08X\n",
                 (unsigned)set_internet_sym);
        } else {
            LOG("  ! ZombiesGame::SetInternet symbol not found — network init may crash\n");
        }

        hook_glf_console_symbols(&zombiewood_mod);
        hook_glf_fs_symbols(&zombiewood_mod);
        hook_glf_thread_symbols(&zombiewood_mod);
        {   /* v4.99: vox::DriverAndroid::Init picks OpenSL ES only when the API level is > 8;
             * otherwise it uses the JNI AudioTrack path, which the fake JNI cannot play. */
            int *api = (int *)so_symbol(&zombiewood_mod, "_ZN3vox13DriverAndroid17s_androidAPILevelE");
            if (api) { LOGF("  v4.99 vox s_androidAPILevel %d -> 14\n", *api); *api = 14; }
            else LOG("  ! vox s_androidAPILevel not found\n");
        }
        hook_zombies_music_symbols(&zombiewood_mod);
        hook_trigger_fade(&zombiewood_mod);
        hook_application_loop_symbols(&zombiewood_mod);
        hook_gameloft_services_symbols(&zombiewood_mod);
        hook_language_symbols(&zombiewood_mod);
        hook_iap_symbols(&zombiewood_mod);
        zw_patch_png_loader(&zombiewood_mod);
        zw_patch_shop(&zombiewood_mod);
#if ZW_CALLPROF
        { extern void zw_prof_init(so_module *); zw_prof_init(&zombiewood_mod); }   /* measurement build only, after every other patch */
#endif
    }

    /* ── 5c. Build the fake JavaVM and call JNI_OnLoad so the .so
            populates whatever JavaVM/JNIEnv globals it tracks for
            its own purposes (in addition to the hook above). */
    java_vm_init();
    {
        int (*jni_onload)(void *vm, void *reserved) =
            (void*)so_symbol(&zombiewood_mod, "JNI_OnLoad");
        if (jni_onload) {
            LOG("  Calling JNI_OnLoad(fake_vm, NULL)\n");
            g_android_exit_status = 0;
            g_android_exit_active = 1;
            if (setjmp(g_android_exit_jmp) == 0) {
                int rc = jni_onload(g_jvm, NULL);
                g_android_exit_active = 0;
                LOGF("  ✓ JNI_OnLoad returned %d (expected JNI_VERSION constant >= 0x10004)\n", rc);
            } else {
                g_android_exit_active = 0;
                LOGF("  ! JNI_OnLoad called exit(%d); continuing after controlled escape\n",
                     g_android_exit_status);
            }
        } else {
            LOG("  ! JNI_OnLoad symbol not found\n");
        }
    }

    /* ── 6. Resolve symbols we call directly ── */
    AndroidAddTouchEvent = (void*)so_symbol(&zombiewood_mod, "_ZN3glf20AndroidAddTouchEventEiiii");
    if (!AndroidAddTouchEvent)
        LOG("WARNING: AndroidAddTouchEvent symbol not found — input disabled\n");

    /* ── 7. Initialize vitaGL ── */
    LOG("=== GRAPHICS INITIALIZATION ===\n");
    LOG("  Calling vglInitExtended(960x544, 12MB for GXM)\n");
    vglInitExtended(0, 960, 544, 12 * 1024 * 1024, SCE_GXM_MULTISAMPLE_4X);
#ifndef VITA3K_BUILD
    if (ZW_DS_DIAG) sceGxmDepthStencilSurfaceSetForceStoreMode(&gxm_depth_stencil_surface, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);  /* v4.65 readback diag */
#endif
    LOG("  ✓ vitaGL initialized successfully\n");

    /* ── 8. Call game's native init ── */
    LOG("=== GAME INITIALIZATION ===\n");
#ifdef VITA3K_BUILD
    LOG("  Vita3K file logging kept enabled for game init diagnostics\n");
#endif

    void (*nativeStep)(void *env, void *obj) =
        (void*)so_symbol(&zombiewood_mod,
            "Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_step");
    void (*nativeRender)(void *env, void *obj) =
        (void*)so_symbol(&zombiewood_mod,
            "Java_com_gameloft_android_ANMP_GloftZOHM_GameRenderer_nativeRender");
    LOGF("  Pre-resolved GL2JNILib_step=%p GameRenderer_nativeRender=%p\n",
         (void*)nativeStep, (void*)nativeRender);

    LOG("  [7/7] Looking up Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_init\n");
    void (*nativeInit)(void *env, void *obj) =
        (void*)so_symbol(&zombiewood_mod,
            "Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_init");
    if (nativeInit) {
        LOGF("  ✓ Symbol found at %p\n", (void*)nativeInit);
        LOG("  Calling GL2JNILib_init(env, null)\n");
        nativeInit(g_jni_env, NULL);
        LOG("  ✓ GL2JNILib_init returned successfully\n");
    } else {
        LOG("  ✗ FATAL: GL2JNILib_init symbol not found\n");
        LOG("  The .so file may be corrupted or incompatible\n");
        zw_log_flush(); if (g_log_fd >= 0) sceIoClose(g_log_fd);
        sceKernelExitProcess(1);
        return 1;
    }

    void (*nativeInitGL)(void *env, void *obj) =
        (void*)so_symbol(&zombiewood_mod,
            "Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_initGL");
    if (nativeInitGL) {
        LOGF("  Calling GL2JNILib_initGL at %p\n", (void*)nativeInitGL);
        nativeInitGL(g_jni_env, NULL);
        LOG("  ✓ GL2JNILib_initGL returned successfully\n");
    } else {
        LOG("  ! GL2JNILib_initGL symbol not found\n");
    }

    void (*getViewSettings)(void *env, void *obj) =
        (void*)so_symbol(&zombiewood_mod,
            "Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_getViewSettings");
    if (getViewSettings) {
        LOGF("  GL2JNILib_getViewSettings available at %p; deferring until after Game_nativeInit\n",
             (void*)getViewSettings);
    } else {
        LOG("  ! GL2JNILib_getViewSettings symbol not found\n");
    }

    void (*setPaths)(void *env, void *obj, jstring apk_path, jstring data_path, jstring save_path) =
        (void*)so_symbol(&zombiewood_mod,
            "Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_setPaths");
    if (setPaths) {
        jstring apk_path = jni_NewStringUTF((JNIEnv *)g_jni_env, "app0:/");
        jstring data_path = jni_NewStringUTF((JNIEnv *)g_jni_env, "ux0:/data/zombiewood/");
        jstring save_path = jni_NewStringUTF((JNIEnv *)g_jni_env, "ux0:/data/zombiewood/");
        LOGF("  Calling GL2JNILib_setPaths at %p\n", (void*)setPaths);
        setPaths(g_jni_env, NULL, apk_path, data_path, save_path);
        LOG("  ✓ GL2JNILib_setPaths returned successfully\n");
    } else {
        LOG("  ! GL2JNILib_setPaths symbol not found\n");
    }

    void (*setPhoneModel)(void *env, void *obj, jstring model) =
        (void*)so_symbol(&zombiewood_mod,
            "Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeSetPhoneModel");
    if (setPhoneModel) {
        jstring model = jni_NewStringUTF((JNIEnv *)g_jni_env, "gt-i9100");
        LOGF("  Calling Game_nativeSetPhoneModel at %p\n", (void*)setPhoneModel);
        setPhoneModel(g_jni_env, NULL, model);
        LOG("  ✓ Game_nativeSetPhoneModel returned\n");
    }

    void (*setImei)(void *env, void *obj, jstring imei) =
        (void*)so_symbol(&zombiewood_mod,
            "Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeSetIMEI");
    if (setImei) {
        jstring imei = jni_NewStringUTF((JNIEnv *)g_jni_env, "000000000000000");
        LOGF("  Calling Game_nativeSetIMEI at %p\n", (void*)setImei);
        setImei(g_jni_env, NULL, imei);
        LOG("  ✓ Game_nativeSetIMEI returned\n");
    }

    void (*setGpuInfo)(void *env, void *obj, jstring gpu) =
        (void*)so_symbol(&zombiewood_mod,
            "Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeSetGPUInfo");
    if (setGpuInfo) {
        jstring gpu = jni_NewStringUTF((JNIEnv *)g_jni_env, "PowerVR SGX543MP4+");
        LOGF("  Calling Game_nativeSetGPUInfo at %p\n", (void*)setGpuInfo);
        setGpuInfo(g_jni_env, NULL, gpu);
        LOG("  ✓ Game_nativeSetGPUInfo returned\n");
    }

    void (*setCpuInfo)(void *env, void *obj, uint32_t cpu_mhz_bits) =
        (void*)so_symbol(&zombiewood_mod,
            "Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeSetCPUInfo");
    if (setCpuInfo) {
        union {
            float f;
            uint32_t u;
        } cpu_mhz = { 444.0f };
        LOGF("  Calling Game_nativeSetCPUInfo at %p\n", (void*)setCpuInfo);
        setCpuInfo(g_jni_env, NULL, cpu_mhz.u);
        LOG("  ✓ Game_nativeSetCPUInfo returned\n");
    }

    void (*gameNativeInit)(void *env, void *obj) =
        (void*)so_symbol(&zombiewood_mod,
            "Java_com_gameloft_android_ANMP_GloftZOHM_Game_nativeInit");
    if (gameNativeInit) {
        LOGF("  Calling Game_nativeInit at %p\n", (void*)gameNativeInit);
        gameNativeInit(g_jni_env, NULL);
        LOG("  ✓ Game_nativeInit returned successfully\n");
    } else {
        LOG("  ! Game_nativeInit symbol not found\n");
    }

    if (getViewSettings) {
        LOGF("  Calling deferred GL2JNILib_getViewSettings at %p\n", (void*)getViewSettings);
        getViewSettings(g_jni_env, NULL);
        LOG("  ✓ deferred GL2JNILib_getViewSettings returned successfully\n");
    }

    void (*rendererInit)(void *env, void *obj, int manufacturer, int w, int h, jstring version) =
        (void*)so_symbol(&zombiewood_mod,
            "Java_com_gameloft_android_ANMP_GloftZOHM_GameRenderer_nativeInit");
    if (rendererInit) {
        jstring version = jni_NewStringUTF((JNIEnv *)g_jni_env, "1.1.0");
        LOGF("  Calling GameRenderer_nativeInit at %p\n", (void*)rendererInit);
        rendererInit(g_jni_env, NULL, 4, 960, 544, version);
        LOG("  ✓ GameRenderer_nativeInit returned successfully\n");
    } else {
        LOG("  ! GameRenderer_nativeInit symbol not found\n");
    }

    void (*rendererResize)(void *env, void *obj, int w, int h) =
        (void*)so_symbol(&zombiewood_mod,
            "Java_com_gameloft_android_ANMP_GloftZOHM_GameRenderer_nativeResize");
    if (rendererResize) {
        LOGF("  Calling GameRenderer_nativeResize at %p\n", (void*)rendererResize);
        rendererResize(g_jni_env, NULL, 960, 544);
        LOG("  ✓ GameRenderer_nativeResize returned successfully\n");
    } else {
        LOG("  ! GameRenderer_nativeResize symbol not found\n");
    }

    void (*stateChanged)(void *env, void *obj, int active) =
        (void*)so_symbol(&zombiewood_mod,
            "Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_stateChanged");
    if (stateChanged) {
        LOGF("  Calling GL2JNILib_stateChanged(true) at %p\n", (void*)stateChanged);
        stateChanged(g_jni_env, NULL, 1);
        LOG("  ✓ GL2JNILib_stateChanged returned successfully\n");
    } else {
        LOG("  ! GL2JNILib_stateChanged symbol not found\n");
    }

    LOG("  Skipping GL2JNILib_resize: it crashes until Android thread TLS is emulated\n");
    {
        /*
         * v4.53: GL2JNILib_resize -> glf::AndroidResizeScreen(w,h) is what
         * stores the screen size in the glf app impl (first call:
         * impl+0x112a8/0x112ac, later calls: impl+0x11230/0x11234). Skipping
         * it left the size 0x0, so gameswf's ortho projection divided by
         * zero and every UniformMatrix4fv was NaN (dark screen). Store the
         * size directly; skip only the GetSequentialThreadId() TLS part.
         */
        /* v4.53b: AndroidResizeScreen reads glf::gAppImpl (not App::mInstance):
         *   obj = *gAppImpl; impl = *(obj + 4); impl+0x112a8 = w ... */
        void **gAppImpl = (void **)so_symbol(&zombiewood_mod, "_ZN3glf8gAppImplE");
        void *glf_app = gAppImpl ? *gAppImpl : NULL;
        void *impl = (glf_app && (uintptr_t)glf_app >= 0x80000000u && (uintptr_t)glf_app < 0xA0000000u)
                     ? *(void **)((uintptr_t)glf_app + 4) : NULL;
        LOGF("  v4.53 gAppImpl=%p obj=%p impl=%p\n", (void *)gAppImpl, glf_app, impl);
        g_zw_app_impl = impl;   /* v5.11: Application*, for key events (START = pause) */
        if (impl && (uintptr_t)impl >= 0x80000000u && (uintptr_t)impl < 0xA0000000u) {
            *(int *)((uintptr_t)impl + 0x112a8) = 960;
            *(int *)((uintptr_t)impl + 0x112ac) = 544;
            *(int *)((uintptr_t)impl + 0x11230) = 960;
            *(int *)((uintptr_t)impl + 0x11234) = 544;
            LOGF("  v4.53 glf screen size set 960x544 (app=%p impl=%p)\n", glf_app, impl);
        } else {
            LOGF("  v4.53 ! glf app not available (GetInstance=%p app=%p)\n",
                 (void *)gAppImpl, glf_app);
        }
    }

    void (*gl2jniResume)(void *env, void *obj) =
        (void*)so_symbol(&zombiewood_mod,
            "Java_com_gameloft_android_ANMP_GloftZOHM_GL2JNILib_resume");
    if (gl2jniResume) {
        LOGF("  Calling GL2JNILib_resume at %p\n", (void*)gl2jniResume);
        gl2jniResume(g_jni_env, NULL);
        LOG("  ✓ GL2JNILib_resume returned successfully\n");
    } else {
        LOG("  ! GL2JNILib_resume symbol not found\n");
    }

    void (*surfaceResume)(void *env, void *obj) =
        (void*)so_symbol(&zombiewood_mod,
            "Java_com_gameloft_android_ANMP_GloftZOHM_GameGLSurfaceView_nativeResume");
    if (surfaceResume) {
        LOGF("  Calling GameGLSurfaceView_nativeResume at %p\n", (void*)surfaceResume);
        surfaceResume(g_jni_env, NULL);
        LOG("  ✓ GameGLSurfaceView_nativeResume returned successfully\n");
    } else {
        LOG("  ! GameGLSurfaceView_nativeResume symbol not found\n");
    }

    if (nativeStep) {
        LOGF("  ✓ GL2JNILib_step found at %p\n", (void*)nativeStep);
    } else {
        LOG("  ! GL2JNILib_step symbol not found\n");
    }

    /* ── 9. Resolve render function ── */
    LOG("  Using pre-resolved render function\n");
    if (!nativeRender) {
        LOG("FATAL: GameRenderer_nativeRender not found\n");
        zw_log_flush(); if (g_log_fd >= 0) sceIoClose(g_log_fd);
        return 1;
    }
#ifdef VITA3K_BUILD
    LOGF("  ✓ GameRenderer_nativeRender found at %p (Vita3K file logging disabled)\n",
         (void*)nativeRender);
#else
    LOGF("  ✓ GameRenderer_nativeRender found at %p\n", (void*)nativeRender);
#endif

    int *g_appAlive_ptr = (int *)so_symbol(&zombiewood_mod, "g_appAlive");
    int *g_appInit_ptr = (int *)so_symbol(&zombiewood_mod, "g_appInit");
    int *g_appPaused_ptr = (int *)so_symbol(&zombiewood_mod, "g_appPaused");
    char **g_appPath_ptr = (char **)so_symbol(&zombiewood_mod, "m_gAppPath");
    static char forced_app_path[] = "ux0:/data/zombiewood/";
    LOGF("  app globals: alive=%p init=%p paused=%p path=%p\n",
         (void*)g_appAlive_ptr, (void*)g_appInit_ptr,
         (void*)g_appPaused_ptr, (void*)g_appPath_ptr);
#ifdef VITA3K_BUILD
    sceClibPrintf("[ZW] app globals stage: alive=%p init=%p paused=%p path=%p\n",
                  (void*)g_appAlive_ptr, (void*)g_appInit_ptr,
                  (void*)g_appPaused_ptr, (void*)g_appPath_ptr);
    sceClibPrintf("[ZW] global poke: alive check\n");
#endif
    if (ptr_in_so_module(&zombiewood_mod, g_appAlive_ptr, sizeof(*g_appAlive_ptr))) {
#ifdef VITA3K_BUILD
        sceClibPrintf("[ZW] global poke: alive write %p\n", (void*)g_appAlive_ptr);
#endif
        *g_appAlive_ptr = 1;
#ifdef VITA3K_BUILD
        sceClibPrintf("[ZW] global poke: alive done\n");
#endif
    } else if (g_appAlive_ptr) {
        LOGF("  ! g_appAlive pointer outside module, not writing: %p\n", (void*)g_appAlive_ptr);
    }
#ifdef VITA3K_BUILD
    sceClibPrintf("[ZW] global poke: init check\n");
#endif
    if (ptr_in_so_module(&zombiewood_mod, g_appInit_ptr, sizeof(*g_appInit_ptr))) {
#ifdef VITA3K_BUILD
        sceClibPrintf("[ZW] global poke: init write %p\n", (void*)g_appInit_ptr);
#endif
        *g_appInit_ptr = 0;
#ifdef VITA3K_BUILD
        sceClibPrintf("[ZW] global poke: init done\n");
#endif
    } else if (g_appInit_ptr) {
        LOGF("  ! g_appInit pointer outside module, not writing: %p\n", (void*)g_appInit_ptr);
    }
#ifdef VITA3K_BUILD
    sceClibPrintf("[ZW] global poke: paused check\n");
#endif
    if (ptr_in_so_module(&zombiewood_mod, g_appPaused_ptr, sizeof(*g_appPaused_ptr))) {
#ifdef VITA3K_BUILD
        sceClibPrintf("[ZW] global poke: paused write %p\n", (void*)g_appPaused_ptr);
#endif
        *g_appPaused_ptr = 0;
#ifdef VITA3K_BUILD
        sceClibPrintf("[ZW] global poke: paused done\n");
#endif
    } else if (g_appPaused_ptr) {
        LOGF("  ! g_appPaused pointer outside module, not writing: %p\n", (void*)g_appPaused_ptr);
    }
#ifdef VITA3K_BUILD
    sceClibPrintf("[ZW] global poke: path check\n");
#endif
    if (ptr_in_so_module(&zombiewood_mod, g_appPath_ptr, sizeof(*g_appPath_ptr))) {
        LOGF("  m_gAppPath slot=%p old_value=%p\n",
             (void*)g_appPath_ptr, (void*)*g_appPath_ptr);
#ifdef VITA3K_BUILD
        sceClibPrintf("[ZW] global poke: path write %p\n", (void*)g_appPath_ptr);
#endif
        *g_appPath_ptr = forced_app_path;
#ifdef VITA3K_BUILD
        sceClibPrintf("[ZW] global poke: path done\n");
#endif
        LOGF("  forced m_gAppPath -> %s\n", forced_app_path);
    } else if (g_appPath_ptr) {
        LOGF("  ! m_gAppPath pointer outside module, not writing: %p\n", (void*)g_appPath_ptr);
    }
    LOG("  Forced app state: alive=1 init=0 paused=0\n");

#ifdef VITA3K_BUILD
    LOG("Entering main loop (Vita3K file logging enabled)\n");
    sceClibPrintf("[ZW] entering main loop\n");
#else
    LOG("Entering main loop\n");
#endif

    /* ── 10. Main game loop ── */
    int frame = 0;
    while (1) {
        g_main_loop_frame = frame;
        SceUInt64 zw_t_loop0 = sceKernelGetProcessTimeWide();   /* v4.74 */
        handle_input();
        /* v4.96: glf::EventManager::DispatchEvents normally runs from glf::App::Update,
         * which is not on this loader's frame path. The posted touch events were never
         * delivered: the queue's pending-touch counter climbed to 10, after which
         * PostEvent dropped every new touch, so FlashManager (HUD/menus) never saw one.
         * Drain the queue ourselves once per frame, after feeding input. */
        if (g_zw_so_base) {
            void *(*get_app)(void) = (void *)ZW_SO_FN(0x3a9fe8);
            void *(*get_em)(void *) = (void *)ZW_SO_FN(0x3aa008);
            void (*dispatch)(void *) = (void *)ZW_SO_FN(0x3c3df8);
            void *app = get_app();
            void *em = app ? get_em(app) : NULL;
            if (em) {
                int fr = g_main_loop_frame;
                int log97 = (fr >= 600 && fr < 1200 && (fr % 60 == 1 || fr % 60 == 11));
                void *rd0 = *(void **)((uint8_t *)em + 0x14), *wr0 = *(void **)((uint8_t *)em + 0x24);
                int p0 = *(int *)((uint8_t *)em + 4);
                dispatch(em);
                if (log97)
                    LOGF("  v4.97 dispatch f%d em=%p pending %d->%d rd %p->%p wr %p->%p\n", fr, em,
                         p0, *(int *)((uint8_t *)em + 4), rd0, *(void **)((uint8_t *)em + 0x14),
                         wr0, *(void **)((uint8_t *)em + 0x24));
            }
        }
/* v5.01: each log line costs ~15 ms on the memory card; the routine every-60/300-frame
 * lines caused regular 50-110 ms hitches during play. Off by default. */
#ifndef ZW_PERIODIC_LOG
#define ZW_PERIODIC_LOG 0
#endif
#ifndef VITA3K_BUILD
        if (ZW_PERIODIC_LOG && (frame % 60) == 0) {
            LOGF("  frame %d: begin\n", frame);
        }
#endif
#ifndef VITA3K_BUILD
        extern void zw_frame_clear_gl(uintptr_t); extern void zw_frame_swap_gl(uintptr_t);
        int zw_pk = zw_ds_probe_kind(frame);
        if (zw_pk == 1 || zw_pk == 2) {
            /* clear-only frame: no game rendering, read back what glClear wrote */
            float cd = zw_pk == 1 ? 1.0f : 0.25f;
            glDepthMask(GL_TRUE); glStencilMask(0xFF); glClearDepthf(cd);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            vglSwapBuffers(GL_FALSE);
            zw_ds_probe(zw_pk == 1 ? "clear1.0" : "clear0.25", frame);
            glClearDepthf(1.0f);
            frame++;
            continue;
        }
        {
            extern volatile GLenum g_zw_depthfunc_override;
            extern volatile int g_zw_force_nostencil;
            extern void zw_apply_depthfunc_override(void);
            if (zw_pk == 3) { g_zw_depthfunc_override = 1; g_zw_force_nostencil = 0; zw_apply_depthfunc_override(); }
            if (zw_pk == 4) { g_zw_depthfunc_override = GL_ALWAYS; g_zw_force_nostencil = 0; zw_apply_depthfunc_override(); }
        }
        static GLuint zw_iso_progs[40]; static unsigned zw_iso_draws[40]; static int zw_iso_n = 0;
        const char *zw_tag = zw_pk == 3 ? "gameLEQUAL" : zw_pk == 4 ? "gameALWAYS" : "gameDEFAULT";
        char zw_tagbuf[24];
        {
            extern volatile GLuint g_zw_only_prog; extern volatile int g_zw_frame_nprogs;
            extern volatile GLenum g_zw_depthfunc_override; extern void zw_apply_depthfunc_override(void);
            g_zw_frame_nprogs = 0;
            g_zw_only_prog = 0;
            if (zw_pk == 6) {
                int idx = frame - 236 - 6;
                if (idx < zw_iso_n) {
                    g_zw_only_prog = zw_iso_progs[idx];
                    g_zw_depthfunc_override = 1; zw_apply_depthfunc_override();
                    snprintf(zw_tagbuf, sizeof(zw_tagbuf), "only%03u_n%u", (unsigned)zw_iso_progs[idx], zw_iso_draws[idx]);
                    zw_tag = zw_tagbuf;
                } else zw_pk = 0;
            }
        }
        if (frame == 0) LOG("  frame 0: before glClearColor\n");
        /* v4.70: no loader glClearColor - the game blends its ground additively over the clear colour */
        if (frame == 0) LOG("  frame 0: before glClear\n");
        /* v4.60: clear depth+stencil ourselves every frame with full write masks */
        zw_frame_clear_gl(0);
        if (frame == 0) LOG("  frame 0: after glClear\n");
#endif
        {   /* v4.50: one-frame GL call trace snapshots */
            extern volatile int g_zw_gl_trace;
            g_zw_gl_trace = g_zw_trace_request;   /* v4.76: START requests a one-frame trace (takes ~40 s of log writing) */
            g_zw_trace_request = 0;
            if (g_zw_gl_trace) LOGF("  v4.50 GL trace begin frame %d\n", frame);
        }
        g_t_step = 0; g_draws_step = 0;
        if (nativeStep && !g_zw_single_pass) {
#ifdef VITA3K_BUILD
            if (frame == 0) sceClibPrintf("[ZW] frame 0: before GL2JNILib_step\n");
            if (frame == 0) LOG("  frame 0: before GL2JNILib_step\n");
#else
            if (frame == 0) LOG("  frame 0: before GL2JNILib_step\n");
#endif
            { unsigned _d = g_gl_draw_elements_count + g_gl_draw_arrays_count; SceUInt64 _t = sceKernelGetProcessTimeWide(); nativeStep(g_jni_env, NULL); g_t_step = sceKernelGetProcessTimeWide() - _t; g_draws_step = g_gl_draw_elements_count + g_gl_draw_arrays_count - _d; }
#ifdef VITA3K_BUILD
            if (frame == 0) sceClibPrintf("[ZW] frame 0: after GL2JNILib_step\n");
            if (frame == 0) LOG("  frame 0: after GL2JNILib_step\n");
#else
            if (frame == 0) LOG("  frame 0: after GL2JNILib_step\n");
#endif
        } else if (frame == 0) {
            LOG("  frame 0: GL2JNILib_step unavailable\n");
        }

        zw_iap_pump();   /* v5.05: deliver queued store callbacks on the game thread */
        zw_shop_frame(frame);   /* v5.06: release kept shop icons after leaving the shop */
        if (frame == 0) LOG("  frame 0: before GameRenderer_nativeRender\n");
        { unsigned _d = g_gl_draw_elements_count + g_gl_draw_arrays_count; SceUInt64 _t = sceKernelGetProcessTimeWide(); nativeRender(g_jni_env, NULL); g_t_render = sceKernelGetProcessTimeWide() - _t; g_draws_render = g_gl_draw_elements_count + g_gl_draw_arrays_count - _d; }
        if (zw_badges_frame(frame))   /* v5.09 */
            zw_badges_gl((uintptr_t)((g_zw_badge_vis[0] ? 1 : 0) | (g_zw_badge_vis[1] ? 2 : 0) | (g_zw_badge_vis[2] ? 4 : 0)));
        if (frame == 0) LOG("  frame 0: after GameRenderer_nativeRender\n");
        g_acc_pre += sceKernelGetProcessTimeWide() - zw_t_loop0 - g_t_step - g_t_render;   /* input + loader clear */
        GLenum gl_err = glGetError();
        {
            extern volatile int g_zw_gl_trace;
            if (g_zw_gl_trace) LOGF("  v4.50 GL trace end frame %d\n", frame);
            g_zw_gl_trace = 0;
        }
#ifdef VITA3K_BUILD
        if (frame == 0) LOG("  frame 0: before vglSwapBuffers\n");
        { SceUInt64 _t = sceKernelGetProcessTimeWide(); vglSwapBuffers(GL_FALSE); g_t_swap = sceKernelGetProcessTimeWide() - _t; SceUInt64 _f = g_t_step + g_t_render + g_t_swap; if (_f > g_t_frame_max) g_t_frame_max = _f; }
        if (frame == 0) LOG("  frame 0: after vglSwapBuffers\n");
        zw_overlay_draw(frame, gl_err);   /* v4.50b: after swap, direct fb write */
        if (frame == 0 || (frame % 60) == 0 || gl_err != GL_NO_ERROR) {
            LOGF("  frame %d: after frame glGetError=0x%04X draws=%u/%u\n",
                 frame, (unsigned)gl_err,
                 g_gl_draw_arrays_count, g_gl_draw_elements_count);
        }
#else
        if ((ZW_PERIODIC_LOG && (frame % 60) == 0) || (gl_err != GL_NO_ERROR && frame < 600)) {
            LOGF("  frame %d: after frame glGetError=0x%04X draws=%u/%u\n",
                 frame, (unsigned)gl_err,
                 g_gl_draw_arrays_count, g_gl_draw_elements_count);
        }
        if (frame == 0) LOG("  frame 0: before vglSwapBuffers\n");
        { SceUInt64 _t = sceKernelGetProcessTimeWide(); zw_frame_swap_gl(0); g_t_swap = sceKernelGetProcessTimeWide() - _t; SceUInt64 _f = g_t_step + g_t_render + g_t_swap; if (_f > g_t_frame_max) g_t_frame_max = _f; }
        if (frame == 0) LOG("  frame 0: after vglSwapBuffers\n");
        g_acc_step += g_t_step; g_acc_render += g_t_render; g_acc_swap += g_t_swap;
#if ZW_CALLPROF
        { extern void zw_prof_frame(int, int, uint32_t, uint32_t);
          zw_prof_frame(frame, g_zw_so_base && (uintptr_t)g_zw_cur_vtable == g_zw_so_base + 0xc9c068, (uint32_t)g_t_render, (uint32_t)g_t_swap); }
#endif
        if (frame == 11) LOGF("  boot: menu frame 11 at T+%u ms\n", (unsigned)(sceKernelGetProcessTimeWide() / 1000));
        if (frame == 30) zw_wav_prefetch_start();   /* v5.14: after the boot I/O is over */
        /* v5.16: gameswf::Root::advance marks the whole Flash HUD (SpriteInstance::thisAlive, ~30 ms) each
         * time a countdown expires, then resets it to period * 0.001f; that float is a literal at
         * .so+0x7aa070. A larger factor runs the mark less often (garbage is freed later: watch heap used). */
#ifndef ZW_SWF_GC_MUL
#define ZW_SWF_GC_MUL 8
#endif
#ifndef ZW_SWF_GC_AB
#define ZW_SWF_GC_AB 0   /* 1 = measurement: factor only in odd 600-frame windows */
#endif
        if (g_zw_so_base && frame >= 60 && (frame % 600) == 0) {
            float want = (!ZW_SWF_GC_AB || ((frame / 600) & 1)) ? 0.001f * ZW_SWF_GC_MUL : 0.001f;
            uint32_t *lit = (uint32_t *)(g_zw_so_base + 0x7aa070u), w; memcpy(&w, &want, 4);
            uint32_t cur = *lit; float curf; memcpy(&curf, &cur, 4);
            if (cur != w && (cur == 0x3a83126fu || (curf > 0.0009f && curf < 0.02f))) {
                kuKernelCpuUnrestrictedMemcpy(lit, &w, 4); so_flush_caches(&zombiewood_mod);
                if (ZW_PERIODIC_LOG) LOGF("  v5.16 swf gc factor f%d: %.3f s per period unit\n", frame, (double)want);
            }
        }
        { extern void zw_vcache_frame(void); zw_vcache_frame(); }   /* v5.14: static vertex cache housekeeping (dynlib.c) */
#if ZW_DRAW_STATS
#endif
        if (ZW_PERIODIC_LOG && (frame % 300) == 299) {
            LOGF("  v5.14 wav f%d: hits %u, misses %u (%u ms on the game thread), prefetched %u\n", frame, g_wav_hits, g_wav_miss, g_wav_miss_us / 1000, g_wav_pre);
            g_wav_hits = g_wav_miss = g_wav_miss_us = 0;
            {   extern unsigned g_zw_texsub_n, g_zw_texsub_skip, g_zw_texsub_us, g_zw_texsub_max_us, g_zw_texsub_last[6];
                LOGF("  v5.16 texsub f%d: %u calls, %u copies skipped, %u ms total, worst %u us (tex %u %ux%u fmt %x fast %u)\n",
                     frame, g_zw_texsub_n, g_zw_texsub_skip, g_zw_texsub_us / 1000, g_zw_texsub_max_us,
                     g_zw_texsub_last[0], g_zw_texsub_last[1], g_zw_texsub_last[2], g_zw_texsub_last[3], g_zw_texsub_last[4]);
                g_zw_texsub_n = g_zw_texsub_skip = g_zw_texsub_us = g_zw_texsub_max_us = 0; }
        }
        {   /* v5.01: slow-frame breakdown (only frames > 80 ms after boot, max 60 lines) */
            SceUInt64 ft = g_t_step + g_t_render + g_t_swap;
            static unsigned nslow, nslow_lvl, last_draws, last_wmiss, last_om, last_obn, last_obkb;
            static SceUInt64 last_omus, last_obus;
            unsigned draws_now = g_gl_draw_arrays_count + g_gl_draw_elements_count, dframe = draws_now - last_draws;
            last_draws = draws_now;
            /* v5.16: in a level (>100 draws) log spikes over 50 ms with their split, separately capped,
             * so fight hitches are not crowded out by the level load. v5.17: + OBB cache misses / big reads */
            if (ZW_PERIODIC_LOG && ft > 50000 && dframe > 100 && frame >= 120 && nslow_lvl < 400) {   /* measurement only: each line costs ~15 ms on the card */
                nslow_lvl++;
                LOGF("  v5.16 spike f%d %llu ms (step %llu rend %llu swap %llu) draws %u wavmiss %u | fopen %llu/%u fread %llu/%u fseek %llu/%u tex %llu/%u link %llu/%u | obb miss %u (%llu ms) big %u %u KB (%llu ms)\n",
                     frame, ft / 1000, g_t_step / 1000, g_t_render / 1000, g_t_swap / 1000, dframe, g_wav_miss >= last_wmiss ? g_wav_miss - last_wmiss : g_wav_miss,
                     g_zw_prof_us[0] / 1000, g_zw_prof_n[0], g_zw_prof_us[1] / 1000, g_zw_prof_n[1], g_zw_prof_us[2] / 1000, g_zw_prof_n[2],
                     g_zw_prof_us[5] / 1000, g_zw_prof_n[5], g_zw_prof_us[4] / 1000, g_zw_prof_n[4],
                     g_obb_miss - last_om, (g_obb_miss_us - last_omus) / 1000, g_obb_big_n - last_obn, g_obb_big_kb - last_obkb, (g_obb_big_us - last_obus) / 1000);
                if (g_zw_fread_top_us > 5000) {
                    const char *tp = tracked_file_path(g_zw_fread_top); VirtualFile *tv = virtual_file_from_FILE(g_zw_fread_top);
                    LOGF("    reads: %llu ms on %s%s\n", g_zw_fread_top_us / 1000, tp ? tp : "(untracked)", tv ? (tv->obb ? " [obb cache]" : " [ram]") : " [stdio]");
                }
            }
            g_zw_fread_top = NULL; g_zw_fread_top_us = 0; s_fr_last = NULL;
            last_wmiss = g_wav_miss;
            last_om = g_obb_miss; last_omus = g_obb_miss_us; last_obn = g_obb_big_n; last_obkb = g_obb_big_kb; last_obus = g_obb_big_us;
            if ((frame >= 120 || ZW_BOOT_TIMING) && ft > 80000 && dframe <= 100 && nslow < (ZW_BOOT_TIMING ? 160 : 60)) {
                nslow++;
                if (ZW_BOOT_TIMING) LOGF("  v5.06 T+%u ms at end of f%d\n", (unsigned)(sceKernelGetProcessTimeWide() / 1000), frame);
                if (ZW_BOOT_TIMING && frame < 60) zw_obb_stats("so far");
                LOGF("  v5.01 slow f%d %llu ms | fopen %llu ms/%u fread %llu/%u fseek %llu/%u | compile %llu/%u link %llu/%u tex %llu/%u\n",
                     frame, ft / 1000,
                     g_zw_prof_us[0] / 1000, g_zw_prof_n[0], g_zw_prof_us[1] / 1000, g_zw_prof_n[1],
                     g_zw_prof_us[2] / 1000, g_zw_prof_n[2], g_zw_prof_us[3] / 1000, g_zw_prof_n[3],
                     g_zw_prof_us[4] / 1000, g_zw_prof_n[4], g_zw_prof_us[5] / 1000, g_zw_prof_n[5]);
            }
            for (int i = 0; i < 6; i++) { g_zw_prof_us[i] = 0; g_zw_prof_n[i] = 0; }
        }
        if ((frame % 60) == 59 && !ZW_PERIODIC_LOG) {
            g_acc_step = g_acc_render = g_acc_swap = g_acc_pre = g_acc_ovl = 0;
            g_acc_wall_t0 = sceKernelGetProcessTimeWide(); g_t_frame_max = 0;
        } else if ((frame % 60) == 59) {
            SceUInt64 now = sceKernelGetProcessTimeWide();
            LOGF("  v4.73 perf f%d: avg ms upd=%.1f rend=%.1f swap=%.1f pre=%.1f ovl(prev)=%.1f | wall %.1f ms/frame = %.1f fps | worst %llu ms | draws/frame %u\n",
                 frame, g_acc_step / 60000.0, g_acc_render / 60000.0, g_acc_swap / 60000.0, g_acc_pre / 60000.0, g_acc_ovl / 60000.0,
                 (now - g_acc_wall_t0) / 60000.0, 60000000.0 / (double)(now - g_acc_wall_t0),
                 g_t_frame_max / 1000, g_draws_step + g_draws_render);
            g_acc_step = g_acc_render = g_acc_swap = g_acc_pre = g_acc_ovl = 0; g_acc_wall_t0 = now; g_t_frame_max = 0;
#if ZW_DRAW_STATS
            { extern void zw_draw_stats_log(int); zw_draw_stats_log(frame); }   /* measurement only */
#endif
#if ZW_GXMSTAT
            { extern void zw_gxmstat_log(int); zw_gxmstat_log(frame); }   /* measurement only */
#endif
            if ((frame % 300) == 299) zw_open_report(frame);
            if ((frame % 300) == 299) { extern void zw_audio_stats(int); zw_audio_stats(frame); }   /* v4.99 */
            if (ZW_TRIGGER_PROBES && (frame % 300) == 299) LOGF("  v4.77 collisions begin=%u end=%u | trigger events=%u matched=%u fades=%u\n",
                                           g_zw_begin_n, g_zw_end_n, g_zw_trig_ev_n, g_zw_trig_match_n, g_zw_fade_n);
        }
        if (zw_pk >= 3) zw_ds_shot(zw_tag, frame);
        { SceUInt64 _t = sceKernelGetProcessTimeWide(); zw_overlay_draw(frame, gl_err); g_acc_ovl += sceKernelGetProcessTimeWide() - _t; }   /* v4.50b: after swap, direct fb write */
        if (zw_pk >= 3) {
            extern volatile GLenum g_zw_depthfunc_override;
            extern volatile int g_zw_force_nostencil;
            extern void zw_apply_depthfunc_override(void);
            extern volatile GLuint g_zw_only_prog, g_zw_frame_progs[40]; extern volatile unsigned g_zw_frame_prog_draws[40]; extern volatile int g_zw_frame_nprogs;
            zw_ds_brief = (zw_pk == 6);
            if (ZW_DS_DIAG) zw_ds_probe(zw_tag, frame);
            g_zw_only_prog = 0;
            if (zw_pk == 5) {
                zw_iso_n = g_zw_frame_nprogs;
                for (int i = 0; i < zw_iso_n; i++) { zw_iso_progs[i] = g_zw_frame_progs[i]; zw_iso_draws[i] = g_zw_frame_prog_draws[i];
                    LOGF("  v4.66 frame progs[%d] = %u draws=%u\n", i, (unsigned)zw_iso_progs[i], zw_iso_draws[i]); }
            }
        }
#endif
#ifdef VITA3K_BUILD
        sceKernelDelayThread(16666);
#endif
        frame++;
    }

    /* Unreachable — but clean up anyway */
    if (g_log_fd >= 0) sceIoClose(g_log_fd);
    return 0;
}

/* the loader's own per-frame GL work */
void zw_frame_clear_gl(uintptr_t unused) {
    (void)unused;
    /* v4.60: clear depth+stencil ourselves every frame with full write masks */
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glClearDepthf(1.0f);
    { GLboolean dt = glIsEnabled(GL_DEPTH_TEST), st = glIsEnabled(GL_STENCIL_TEST);
      glEnable(GL_DEPTH_TEST); glEnable(GL_STENCIL_TEST);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
      if (!dt) glDisable(GL_DEPTH_TEST);
      if (!st) glDisable(GL_STENCIL_TEST); }
}
void zw_frame_swap_gl(uintptr_t unused) { (void)unused; vglSwapBuffers(GL_FALSE); }
static void *game_thread_main(void *arg) {
    GameThreadArgs *args = (GameThreadArgs *)arg;
    int argc = args ? args->argc : 0;
    char **argv = args ? args->argv : NULL;
    int res = game_main(argc, argv);
    return (void *)(intptr_t)res;
}

/* ─────────────────────────────────────────────
   Entry point
   ───────────────────────────────────────────── */
/* ── v5.13: loader-only release ──
 * No Gameloft file ships in the VPK. The player copies, from their own Zombiewood 1.5.3 for
 * Android: lib/armeabi-v7a/libZombieWood.so and libgenerator.so from the APK, plus the main
 * and patch OBBs, into ux0:data/zombiewood/. The loader patches fixed code offsets of exactly
 * this build, so the .so files are checked by size + CRC32 and the OBBs by size before
 * anything is loaded; a missing or different file stops at a readable error screen. */
#include <zlib.h>
static const struct { const char *name; uint32_t size, crc; int has_crc; } g_zw_game_files[] = {
    { "libZombieWood.so", 13500416u, 0xa83b9a52u, 1 },
    { "libgenerator.so", 447860u, 0x337cc20du, 1 },
    { "main.1501.com.gameloft.android.ANMP.GloftZOHM.obb", 289401986u, 0, 0 },
    { "patch.15325.com.gameloft.android.ANMP.GloftZOHM.obb", 1479926u, 0, 0 },
};
#define ZW_NGAMEFILES (int)(sizeof(g_zw_game_files) / sizeof(g_zw_game_files[0]))
/* 0 = ok, 1 = missing, 2 = wrong size/checksum */
static int zw_check_game_file(int i) {
    char path[160];
    snprintf(path, sizeof(path), ZW_GAME_DIR "%s", g_zw_game_files[i].name);
    SceIoStat st;
    if (sceIoGetstat(path, &st) < 0) return 1;
    if ((uint64_t)st.st_size != g_zw_game_files[i].size) return 2;
    if (!g_zw_game_files[i].has_crc) return 0;
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return 1;
    static unsigned char buf[256 * 1024];
    uLong crc = crc32(0L, Z_NULL, 0);
    int n;
    while ((n = sceIoRead(fd, buf, sizeof(buf))) > 0) crc = crc32(crc, buf, (uInt)n);
    sceIoClose(fd);
    return (uint32_t)crc == g_zw_game_files[i].crc ? 0 : 2;
}
/* Full-screen text on a plain framebuffer (no GL yet), 5x7 font at 2x; waits for a button. */
static void zw_fatal_screen(const char *const *lines, int n) {
    const int W = 960, H = 544, S = 2;
    SceUID blk = sceKernelAllocMemBlock("zw_err_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 2 * 1024 * 1024, NULL);
    uint32_t *fb = NULL;
    if (blk >= 0) sceKernelGetMemBlockBase(blk, (void **)&fb);
    if (fb) {
        for (int i = 0; i < W * H; i++) fb[i] = 0xFF1A1410u;   /* dark brown (ABGR) */
        for (int l = 0; l < n; l++) {
            uint32_t col = l == 0 ? 0xFF3CC8F0u : 0xFFE8F0F4u;   /* title amber, body off-white */
            int y0 = 40 + l * 9 * S;
            for (int c = 0; lines[l][c] && 30 + (c + 1) * 6 * S < W; c++) {
                int ch = (unsigned char)lines[l][c];
                if (ch >= 'a' && ch <= 'z') ch -= 32;
                if (ch < 32 || ch > 95) ch = '?';
                const uint8_t *g = zw_font5x7[ch - 32];
                for (int gx = 0; gx < 5; gx++)
                    for (int gy = 0; gy < 7; gy++)
                        if (g[gx] & (1u << gy))
                            for (int sy = 0; sy < S; sy++)
                                for (int sx = 0; sx < S; sx++)
                                    fb[(y0 + gy * S + sy) * W + 30 + c * 6 * S + gx * S + sx] = col;
            }
        }
        SceDisplayFrameBuf d;
        memset(&d, 0, sizeof(d));
        d.size = sizeof(d); d.base = fb; d.pitch = W; d.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
        d.width = W; d.height = H;
        sceDisplaySetFrameBuf(&d, SCE_DISPLAY_SETBUF_NEXTFRAME);
    }
    SceCtrlData pad;
    unsigned prev = 0xFFFFFFFFu;   /* ignore buttons already held at launch */
    for (;;) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & ~prev) break;
        prev = pad.buttons;
        sceKernelDelayThread(16000);
    }
    sceKernelExitProcess(0);
}
static void zw_check_game_files(void) {
    static char msg[ZW_NGAMEFILES][64];
    const char *lines[24]; int n = 0, bad = 0;
    lines[n++] = "ZOMBIEWOOD FOR PS VITA";
    lines[n++] = "";
    for (int i = 0; i < ZW_NGAMEFILES; i++) {
        int r = zw_check_game_file(i);
        LOGF("  v5.13 game file %s: %s\n", g_zw_game_files[i].name, r == 0 ? "ok" : r == 1 ? "MISSING" : "WRONG VERSION");
        if (!r) continue;
        if (!bad++) { lines[n++] = "THESE GAME FILES ARE MISSING OR NOT VERSION 1.5.3:"; }
        snprintf(msg[i], sizeof(msg[i]), " %s %s", r == 1 ? "MISSING:" : "WRONG:", g_zw_game_files[i].name);
        lines[n++] = msg[i];
    }
    if (!bad) return;
    lines[n++] = "";
    lines[n++] = "FROM YOUR OWN ZOMBIEWOOD 1.5.3 FOR ANDROID, COPY TO";
    lines[n++] = "UX0:DATA/ZOMBIEWOOD/ :";
    lines[n++] = " LIBZOMBIEWOOD.SO AND LIBGENERATOR.SO";
    lines[n++] = "   (IN THE APK UNDER LIB/ARMEABI-V7A/)";
    lines[n++] = " MAIN.1501...OBB AND PATCH.15325...OBB";
    lines[n++] = "";
    lines[n++] = "PRESS ANY BUTTON TO EXIT.";
    zw_log_flush();
    zw_fatal_screen(lines, n);
}

/* v5.18: the game reads its data unpacked from the OBBs (586 files). tools/prepare_data.py writes
 * zw_data.txt last ("ZWDATA 1", "<size> <name>" lines, "END <count>"). One directory scan checks
 * every listed file by name and size, so a copy to the Vita that stopped partway shows an error
 * screen instead of crashing inside the game. */
#define ZW_MANIFEST "zw_data.txt"
static void zw_data_error(const char *what, const char *const *names, int nn, int nbad) {
    static char head[64], more[64], nb[6][64];
    const char *lines[24]; int n = 0;
    lines[n++] = "ZOMBIEWOOD FOR PS VITA";
    lines[n++] = "";
    snprintf(head, sizeof(head), "%s", what);
    lines[n++] = head;
    for (int i = 0; i < nn && i < 6; i++) { snprintf(nb[i], sizeof(nb[i]), " %.60s", names[i]); lines[n++] = nb[i]; }
    if (nbad > nn) { snprintf(more, sizeof(more), " ... AND %d MORE", nbad - nn); lines[n++] = more; }
    lines[n++] = "";
    lines[n++] = "RUN TOOLS/PREPARE_DATA ON YOUR PC WITH YOUR OWN";
    lines[n++] = "ZOMBIEWOOD 1.5.3 APK AND BOTH OBB FILES, THEN COPY";
    lines[n++] = "THE WHOLE ZOMBIEWOOD FOLDER IT MAKES TO";
    lines[n++] = "UX0:DATA/ZOMBIEWOOD/  (A COPY THAT STOPPED PARTWAY";
    lines[n++] = "LEAVES FILES MISSING: COPY IT AGAIN.)";
    lines[n++] = "";
    lines[n++] = "PRESS ANY BUTTON TO EXIT.";
    zw_log_flush();
    zw_fatal_screen(lines, n);
}
static void zw_check_game_data(void) {
    static char buf[64 * 1024];
    SceUID fd = sceIoOpen(ZW_GAME_DIR ZW_MANIFEST, SCE_O_RDONLY, 0);
    int len = fd >= 0 ? sceIoRead(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0) sceIoClose(fd);
    if (len <= 0) { LOGF("  v5.18 game data: no %s\n", ZW_MANIFEST); zw_data_error("THE GAME DATA HAS NOT BEEN PREPARED (NO ZW_DATA.TXT).", NULL, 0, 0); }
    buf[len] = 0;
    /* directory: name hash -> size */
    static uint32_t dh[1536], ds[1536]; int dn = 0;
    SceUID d = sceIoDopen(ZW_GAME_DIR);
    if (d >= 0) {
        SceIoDirent de; memset(&de, 0, sizeof(de));
        while (dn < 1536 && sceIoDread(d, &de) > 0) {
            if (SCE_S_ISDIR(de.d_stat.st_mode)) continue;
            dh[dn] = zw_strhash(de.d_name); ds[dn] = (uint32_t)de.d_stat.st_size; dn++;
        }
        sceIoDclose(d);
    }
    static const char *bad[6]; static char badn[6][64]; int nbad = 0, nlist = 0, end = -1;
    char *p = buf, *e;
    if (strncmp(p, "ZWDATA 1", 8) != 0) zw_data_error("ZW_DATA.TXT IS DAMAGED.", NULL, 0, 0);
    for (; *p; p = e + 1) {
        e = strchr(p, '\n'); if (!e) break;
        *e = 0; if (e > p && e[-1] == '\r') e[-1] = 0;
        if (!strncmp(p, "ZWDATA", 6)) continue;
        if (!strncmp(p, "END ", 4)) { end = atoi(p + 4); break; }
        char *sp = strchr(p, ' '); if (!sp) continue;
        uint32_t want = (uint32_t)strtoul(p, NULL, 10); const char *name = sp + 1;
        uint32_t h = zw_strhash(name); int ok = 0;
        for (int i = 0; i < dn; i++) if (dh[i] == h) { ok = ds[i] == want ? 1 : -1; break; }
        nlist++;
        if (ok != 1) {
            if (nbad < 6) { snprintf(badn[nbad], sizeof(badn[nbad]), "%s%s", ok ? "INCOMPLETE: " : "MISSING: ", name); bad[nbad] = badn[nbad]; }
            nbad++;
        }
    }
    LOGF("  v5.18 game data: %d listed, %d files in folder, %d missing or incomplete, end %d\n", nlist, dn, nbad, end);
    if (end != nlist) zw_data_error("ZW_DATA.TXT IS INCOMPLETE (THE COPY STOPPED PARTWAY).", NULL, 0, 0);
    if (nbad) {
        static char what[64]; snprintf(what, sizeof(what), "%d GAME DATA FILES ARE MISSING OR INCOMPLETE:", nbad);
        zw_data_error(what, bad, nbad < 6 ? nbad : 6, nbad);
    }
}

int main(int argc, char *argv[]) {
    pthread_t t;
    pthread_attr_t attr;
    GameThreadArgs args;
    args.argc = argc;
    args.argv = argv;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 9 * 1024 * 1024);
    pthread_create(&t, &attr, game_thread_main, &args);
    pthread_attr_destroy(&attr);

    void *ret = NULL;
    pthread_join(t, &ret);
    return (int)(intptr_t)ret;
}
