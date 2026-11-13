/* v4.99: minimal OpenSL ES emulation for the Vox sound engine, on top of sceAudioOut.
 *
 * Vox (vox::DriverAndroid::_InitOSL) does its own mixing and only needs:
 *   slCreateEngine -> Object.Realize/GetState/GetInterface(SL_IID_ENGINE)
 *   Engine.CreateOutputMix (+0x1c) -> Object.Realize/GetState
 *   Engine.CreateAudioPlayer (+0x08), PCM 16-bit -> Object.Realize,
 *     GetInterface(SL_IID_PLAY), GetInterface(SL_IID_BUFFERQUEUE)
 *   BufferQueue.RegisterCallback (+0x0c), GetState (+0x08), Enqueue (+0x00), Clear (+0x04)
 *   Play.SetPlayState (+0x00)
 * It mixes into ONE buffer, enqueues it, and keeps two enqueued (re-filling the same
 * memory), so Enqueue copies the data. A thread drains the queue into the audio port in
 * fixed grains and calls the registered callback once per consumed buffer.
 */
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

extern void zw_log(const char *fmt, ...);
#define log_printf zw_log

typedef uint32_t SLresult;
typedef uint32_t SLuint32;
typedef const void *SLInterfaceID;
#define SL_RESULT_PARAMETER_INVALID    2
#define SL_RESULT_FEATURE_UNSUPPORTED 12
#define SL_OBJECT_STATE_REALIZED       2
#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PLAYING 3

/* Interface IDs: only their addresses matter. The game imports the *variables*
 * (const SLInterfaceID SL_IID_x), so dynlib maps the symbol to &zw_SL_IID_x. */
static const uint32_t iid_engine[4] = { 0x8d97c260, 1, 0, 0 };
static const uint32_t iid_play[4]   = { 0xef0bd9c0, 2, 0, 0 };
static const uint32_t iid_bq[4]     = { 0x2bc99cc0, 3, 0, 0 };
const void *zw_SL_IID_ENGINE      = iid_engine;
const void *zw_SL_IID_PLAY        = iid_play;
const void *zw_SL_IID_BUFFERQUEUE = iid_bq;

/* ---------------- objects ---------------- */
typedef struct ZwObj ZwObj;
typedef struct {
    SLresult (*Realize)(void *self, uint32_t async);
    SLresult (*Resume)(void *self, uint32_t async);
    SLresult (*GetState)(void *self, SLuint32 *state);
    SLresult (*GetInterface)(void *self, SLInterfaceID iid, void *pInterface);
    SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
    void     (*AbortAsyncOperation)(void *self);
    void     (*Destroy)(void *self);
    SLresult (*SetPriority)(void *self, int32_t prio, uint32_t preemptable);
    SLresult (*GetPriority)(void *self, int32_t *prio, uint32_t *preemptable);
    SLresult (*SetLossOfControlInterfaces)(void *self, int16_t n, SLInterfaceID *ids, uint32_t enabled);
} ObjVtbl;

typedef struct {
    SLresult (*CreateLEDDevice)(void *, void *, SLuint32, SLuint32, const void *, const void *);
    SLresult (*CreateVibraDevice)(void *, void *, SLuint32, SLuint32, const void *, const void *);
    SLresult (*CreateAudioPlayer)(void *self, void *pPlayer, void *src, void *sink,
                                  SLuint32 n, const void *ids, const void *req);
    SLresult (*CreateAudioRecorder)(void *, void *, void *, void *, SLuint32, const void *, const void *);
    SLresult (*CreateMidiPlayer)(void);
    SLresult (*CreateListener)(void);
    SLresult (*Create3DGroup)(void);
    SLresult (*CreateOutputMix)(void *self, void *pMix, SLuint32 n, const void *ids, const void *req);
    SLresult (*CreateMetadataExtractor)(void);
    SLresult (*CreateExtensionObject)(void);
    SLresult (*QueryNumSupportedInterfaces)(void);
    SLresult (*QuerySupportedInterfaces)(void);
    SLresult (*QueryNumSupportedExtensions)(void);
    SLresult (*QuerySupportedExtension)(void);
    SLresult (*IsExtensionSupported)(void);
} EngineVtbl;

typedef struct {
    SLresult (*SetPlayState)(void *self, SLuint32 state);
    SLresult (*GetPlayState)(void *self, SLuint32 *state);
    SLresult (*GetDuration)(void *self, SLuint32 *ms);
    SLresult (*GetPosition)(void *self, SLuint32 *ms);
    SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
    SLresult (*SetCallbackEventsMask)(void *self, SLuint32 m);
    SLresult (*GetCallbackEventsMask)(void *self, SLuint32 *m);
    SLresult (*SetMarkerPosition)(void *self, SLuint32 ms);
    SLresult (*ClearMarkerPosition)(void *self);
    SLresult (*GetMarkerPosition)(void *self, SLuint32 *ms);
    SLresult (*SetPositionUpdatePeriod)(void *self, SLuint32 ms);
    SLresult (*GetPositionUpdatePeriod)(void *self, SLuint32 *ms);
} PlayVtbl;

typedef struct { SLuint32 count, playIndex; } BQState;
typedef void (*BQCallback)(void *bq, void *ctx);
typedef struct {
    SLresult (*Enqueue)(void *self, const void *buf, SLuint32 size);
    SLresult (*Clear)(void *self);
    SLresult (*GetState)(void *self, BQState *st);
    SLresult (*RegisterCallback)(void *self, BQCallback cb, void *ctx);
} BQVtbl;

enum { OBJ_ENGINE, OBJ_MIX, OBJ_PLAYER };

/* Each "interface" is a pointer to a vtable pointer; the game does (*itf)->Fn(itf, ...). */
struct ZwObj {
    const ObjVtbl *obj_vt;        /* SLObjectItf points here */
    const void    *sub_vt;        /* engine / play itf points here */
    const BQVtbl  *bq_vt;         /* buffer-queue itf points here */
    int kind;
    SLuint32 state;
};

static ZwObj *obj_of_obj(void *self) { return (ZwObj *)self; }
static ZwObj *obj_of_sub(void *self) { return (ZwObj *)((uint8_t *)self - offsetof(ZwObj, sub_vt)); }

/* ---------------- player / output thread ---------------- */
#define ZW_GRAIN    1024                    /* frames per sceAudioOutOutput */
#define ZW_QSLOTS   4
#define ZW_MAXBUF   (16 * 1024)

static struct {
    pthread_mutex_t mtx;
    int inited;
    int channels, rate;
    SLuint32 play_state;
    BQCallback cb; void *cb_ctx; void *bq_itf;
    uint8_t data[ZW_QSLOTS][ZW_MAXBUF];
    SLuint32 size[ZW_QSLOTS];
    int head, count;                       /* ring of queued buffers */
    SLuint32 consumed;                     /* bytes of slot[head] already played */
    SLuint32 play_index;
    int port;
    volatile int run;
    unsigned underruns, played;
} P;

static void *audio_thread(void *arg) {
    (void)arg;
    int16_t *out = memalign(64, ZW_GRAIN * 2 * sizeof(int16_t));
    while (P.run) {
        int nfill = 0, finished = 0;
        pthread_mutex_lock(&P.mtx);
        int playing = P.play_state == SL_PLAYSTATE_PLAYING;
        uint8_t *dst = (uint8_t *)out;
        SLuint32 want = ZW_GRAIN * 2 * sizeof(int16_t);
        while (playing && want > 0 && P.count > 0) {
            SLuint32 avail = P.size[P.head] - P.consumed;
            SLuint32 n = avail < want ? avail : want;
            if (P.channels == 2) {
                memcpy(dst, P.data[P.head] + P.consumed, n);
                dst += n; want -= n; P.consumed += n;
            } else {                       /* mono -> stereo */
                SLuint32 frames = n / 2; if (frames > want / 4) frames = want / 4;
                const int16_t *s = (const int16_t *)(P.data[P.head] + P.consumed);
                int16_t *d = (int16_t *)dst;
                for (SLuint32 i = 0; i < frames; i++) { d[2 * i] = d[2 * i + 1] = s[i]; }
                dst += frames * 4; want -= frames * 4; P.consumed += frames * 2;
                if (frames == 0) want = 0;
            }
            if (P.consumed >= P.size[P.head]) {
                P.head = (P.head + 1) % ZW_QSLOTS; P.count--; P.consumed = 0;
                P.play_index++; finished++;
            }
            nfill = 1;
        }
        if (want > 0) { memset(dst, 0, want); if (playing) P.underruns++; }
        BQCallback cb = P.cb; void *ctx = P.cb_ctx, *bq = P.bq_itf;
        pthread_mutex_unlock(&P.mtx);
        (void)nfill;
        /* Let Vox refill before blocking on the hardware, so the queue never runs dry. */
        for (int i = 0; i < finished && cb; i++) cb(bq, ctx);
        sceAudioOutOutput(P.port, out);
        P.played++;
    }
    free(out);
    return NULL;
}

static void player_start_thread(void) {
    if (P.run) return;
    int rate = P.rate;
    if (rate != 8000 && rate != 11025 && rate != 12000 && rate != 16000 && rate != 22050 &&
        rate != 24000 && rate != 32000 && rate != 44100 && rate != 48000) rate = 44100;
    P.port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, ZW_GRAIN, rate, SCE_AUDIO_OUT_MODE_STEREO);
    log_printf("  v4.99 audio: port=0x%08X rate=%d ch=%d grain=%d\n", P.port, rate, P.channels, ZW_GRAIN);
    if (P.port < 0) return;
    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(P.port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    P.run = 1;
    pthread_t th; pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 512 * 1024);   /* Vox mixes (and decodes) inside the callback */
    pthread_create(&th, &a, audio_thread, NULL);
    pthread_attr_destroy(&a);
}

/* ---------------- vtable functions ---------------- */
static SLresult o_Realize(void *self, uint32_t async) { (void)async; obj_of_obj(self)->state = SL_OBJECT_STATE_REALIZED; return 0; }
static SLresult o_Resume(void *self, uint32_t async) { (void)self; (void)async; return 0; }
static SLresult o_GetState(void *self, SLuint32 *st) { if (st) *st = obj_of_obj(self)->state; return 0; }
static SLresult o_GetInterface(void *self, SLInterfaceID iid, void *pItf) {
    ZwObj *o = obj_of_obj(self);
    void **out = (void **)pItf;
    if (!out) return SL_RESULT_PARAMETER_INVALID;
    if ((iid == iid_engine && o->kind == OBJ_ENGINE) || (iid == iid_play && o->kind == OBJ_PLAYER)) {
        *out = &o->sub_vt; return 0;
    }
    if (iid == iid_bq && o->kind == OBJ_PLAYER) { *out = &o->bq_vt; return 0; }
    log_printf("  v4.99 audio: GetInterface kind=%d iid=%p unsupported\n", o->kind, iid);
    *out = NULL;
    return SL_RESULT_FEATURE_UNSUPPORTED;
}
static SLresult o_RegisterCallback(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return 0; }
static void o_Abort(void *self) { (void)self; }
static void o_Destroy(void *self) {
    ZwObj *o = obj_of_obj(self);
    if (o->kind == OBJ_PLAYER) {
        pthread_mutex_lock(&P.mtx);
        P.play_state = SL_PLAYSTATE_STOPPED; P.cb = NULL; P.count = 0; P.consumed = 0;
        pthread_mutex_unlock(&P.mtx);
    }
}
static SLresult o_SetPriority(void *s, int32_t p, uint32_t e) { (void)s; (void)p; (void)e; return 0; }
static SLresult o_GetPriority(void *s, int32_t *p, uint32_t *e) { (void)s; if (p) *p = 0; if (e) *e = 0; return 0; }
static SLresult o_SetLoss(void *s, int16_t n, SLInterfaceID *i, uint32_t e) { (void)s; (void)n; (void)i; (void)e; return 0; }
static const ObjVtbl g_obj_vt = { o_Realize, o_Resume, o_GetState, o_GetInterface, o_RegisterCallback,
                                  o_Abort, o_Destroy, o_SetPriority, o_GetPriority, o_SetLoss };

static ZwObj *new_obj(int kind);

typedef struct { SLuint32 locatorType, numBuffers; } LocBQ;
typedef struct { SLuint32 formatType, numChannels, samplesPerSec, bitsPerSample, containerSize, channelMask, endianness; } FmtPCM;
typedef struct { void *pLocator, *pFormat; } DataSrc;

static SLresult e_CreateAudioPlayer(void *self, void *pPlayer, void *src, void *sink,
                                    SLuint32 n, const void *ids, const void *req) {
    (void)self; (void)sink; (void)n; (void)ids; (void)req;
    DataSrc *ds = (DataSrc *)src;
    FmtPCM *f = ds ? (FmtPCM *)ds->pFormat : NULL;
    LocBQ *l = ds ? (LocBQ *)ds->pLocator : NULL;
    P.channels = f ? (int)f->numChannels : 2;
    P.rate = f ? (int)(f->samplesPerSec / 1000) : 44100;
    log_printf("  v4.99 audio: CreateAudioPlayer loc=0x%x nbuf=%u fmt=%u ch=%u rate=%u bits=%u\n",
               l ? l->locatorType : 0, l ? l->numBuffers : 0, f ? f->formatType : 0,
               f ? f->numChannels : 0, f ? f->samplesPerSec : 0, f ? f->bitsPerSample : 0);
    if (P.channels < 1 || P.channels > 2) P.channels = 2;
    *(void **)pPlayer = &new_obj(OBJ_PLAYER)->obj_vt;
    return 0;
}
static SLresult e_CreateOutputMix(void *self, void *pMix, SLuint32 n, const void *ids, const void *req) {
    (void)self; (void)n; (void)ids; (void)req;
    *(void **)pMix = &new_obj(OBJ_MIX)->obj_vt;
    return 0;
}
static SLresult e_unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }
static const EngineVtbl g_engine_vt = {
    (void *)e_unsupported, (void *)e_unsupported, e_CreateAudioPlayer, (void *)e_unsupported,
    e_unsupported, e_unsupported, e_unsupported, e_CreateOutputMix, e_unsupported, e_unsupported,
    e_unsupported, e_unsupported, e_unsupported, e_unsupported, e_unsupported };

static SLresult p_SetPlayState(void *self, SLuint32 st) {
    (void)self;
    pthread_mutex_lock(&P.mtx);
    P.play_state = st;
    if (st == SL_PLAYSTATE_STOPPED) { P.count = 0; P.consumed = 0; }
    pthread_mutex_unlock(&P.mtx);
    log_printf("  v4.99 audio: SetPlayState(%u)\n", st);
    if (st == SL_PLAYSTATE_PLAYING) player_start_thread();
    return 0;
}
static SLresult p_GetPlayState(void *self, SLuint32 *st) { (void)self; if (st) *st = P.play_state; return 0; }
static SLresult p_zero(void *self, SLuint32 *v) { (void)self; if (v) *v = 0; return 0; }
static SLresult p_ok1(void *self, SLuint32 v) { (void)self; (void)v; return 0; }
static SLresult p_ok0(void *self) { (void)self; return 0; }
static SLresult p_cb(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return 0; }
static const PlayVtbl g_play_vt = { p_SetPlayState, p_GetPlayState, p_zero, p_zero, p_cb, p_ok1, p_zero,
                                    p_ok1, p_ok0, p_zero, p_ok1, p_zero };

static SLresult b_Enqueue(void *self, const void *buf, SLuint32 size) {
    (void)self;
    if (!buf || size == 0 || size > ZW_MAXBUF) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&P.mtx);
    if (P.count >= ZW_QSLOTS) { pthread_mutex_unlock(&P.mtx); return 14; /* SL_RESULT_BUFFER_INSUFFICIENT */ }
    int slot = (P.head + P.count) % ZW_QSLOTS;
    memcpy(P.data[slot], buf, size);
    P.size[slot] = size;
    P.count++;
    pthread_mutex_unlock(&P.mtx);
    return 0;
}
static SLresult b_Clear(void *self) {
    (void)self;
    pthread_mutex_lock(&P.mtx); P.count = 0; P.consumed = 0; pthread_mutex_unlock(&P.mtx);
    return 0;
}
static SLresult b_GetState(void *self, BQState *st) {
    (void)self;
    pthread_mutex_lock(&P.mtx);
    if (st) { st->count = P.count; st->playIndex = P.play_index; }
    pthread_mutex_unlock(&P.mtx);
    return 0;
}
static SLresult b_RegisterCallback(void *self, BQCallback cb, void *ctx) {
    pthread_mutex_lock(&P.mtx); P.cb = cb; P.cb_ctx = ctx; P.bq_itf = self; pthread_mutex_unlock(&P.mtx);
    log_printf("  v4.99 audio: bq RegisterCallback cb=%p ctx=%p\n", (void *)cb, ctx);
    return 0;
}
static const BQVtbl g_bq_vt = { b_Enqueue, b_Clear, b_GetState, b_RegisterCallback };

static ZwObj *new_obj(int kind) {
    ZwObj *o = calloc(1, sizeof(*o));
    o->obj_vt = &g_obj_vt;
    o->kind = kind;
    o->state = 1; /* SL_OBJECT_STATE_UNREALIZED */
    o->sub_vt = kind == OBJ_ENGINE ? (const void *)&g_engine_vt :
                kind == OBJ_PLAYER ? (const void *)&g_play_vt : NULL;
    o->bq_vt = kind == OBJ_PLAYER ? &g_bq_vt : NULL;
    return o;
}

SLresult slCreateEngine(void **pEngine, SLuint32 numOptions, const void *opts,
                        SLuint32 numItf, const void *ids, const void *req) {
    (void)numOptions; (void)opts; (void)numItf; (void)ids; (void)req;
    if (!P.inited) { pthread_mutex_init(&P.mtx, NULL); P.inited = 1; P.channels = 2; P.rate = 44100; }
    if (!pEngine) return SL_RESULT_PARAMETER_INVALID;
    *pEngine = &new_obj(OBJ_ENGINE)->obj_vt;
    log_printf("  v4.99 audio: slCreateEngine -> %p\n", *pEngine);
    return 0;
}

void zw_audio_stats(int frame) {
    if (!P.run) return;
    log_printf("  v4.99 audio f%d: played=%u underruns=%u queued=%d state=%u\n",
               frame, P.played, P.underruns, P.count, P.play_state);
}
