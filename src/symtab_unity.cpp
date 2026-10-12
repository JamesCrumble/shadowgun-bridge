/*
 * Imports of libmain.so / libunity.so / libil2cpp.so that the inherited tables
 * do not answer: the rest of libandroid (native window, sensors, the motion
 * event accessors libmain forwards), a few bionic-only libc entries, SHA1 (part
 * of bionic's libc, absent from glibc) and some libm functions.
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wchar.h>

#include "so_util.h"
#include "thunk_gen.h"
#include "android_api.h"

extern "C" ANativeWindow *android_platform_window(void);

extern "C" {

/* ---- native window ------------------------------------------------------ */
static ANativeWindow *u_ANativeWindow_fromSurface(void *env, void *surface)
{
    (void)env; (void)surface;
    return android_platform_window();
}
static void u_ANativeWindow_acquire(ANativeWindow *w) { (void)w; }
static void u_ANativeWindow_release(ANativeWindow *w) { (void)w; }
/* Software blits to the window (the splash screen path) are refused; GL owns it. */
static int32_t u_ANativeWindow_lock(ANativeWindow *w, void *buf, void *rect) { (void)w; (void)buf; (void)rect; return -1; }
static int32_t u_ANativeWindow_unlockAndPost(ANativeWindow *w) { (void)w; return -1; }

/* ---- looper ------------------------------------------------------------- */
static ALooper *u_ALooper_forThread(void) { return ALooper_prepare(0); }
static void u_ALooper_wake(ALooper *l) { (void)l; }

/* ---- sensors: none ------------------------------------------------------ */
static void *u_ASensorManager_getInstance(void) { return (void *)0x5E050001; }
static int u_ASensorManager_getSensorList(void *m, void ***list) { (void)m; if (list) *list = NULL; return 0; }
static void *u_ASensorManager_getDefaultSensor(void *m, int type) { (void)m; (void)type; return NULL; }
static void *u_ASensorManager_createEventQueue(void *m, void *looper, int ident, void *cb, void *data)
{
    (void)m; (void)looper; (void)ident; (void)cb; (void)data;
    return (void *)0x5E050002;
}
static int u_ASensorManager_destroyEventQueue(void *m, void *q) { (void)m; (void)q; return 0; }
static int u_ASensorEventQueue_enableSensor(void *q, void *s) { (void)q; (void)s; return -1; }
static int u_ASensorEventQueue_disableSensor(void *q, void *s) { (void)q; (void)s; return 0; }
static int u_ASensorEventQueue_setEventRate(void *q, void *s, int32_t usec) { (void)q; (void)s; (void)usec; return 0; }
static int u_ASensorEventQueue_hasEvents(void *q) { (void)q; return 0; }
static int u_ASensorEventQueue_getEvents(void *q, void *events, size_t count) { (void)q; (void)events; (void)count; return 0; }
static const char *u_ASensor_getName(void *s) { (void)s; return "none"; }
static const char *u_ASensor_getVendor(void *s) { (void)s; return "none"; }
static int u_ASensor_getType(void *s) { (void)s; return 0; }
static float u_ASensor_getResolution(void *s) { (void)s; return 0.0f; }
static int u_ASensor_getMinDelay(void *s) { (void)s; return 0; }

/* ---- motion event accessors only libmain's Dalvik forwarder reads -------- */
static int64_t u_AMotionEvent_getDownTime(const AInputEvent *e) { (void)e; return 0; }
static int64_t u_AMotionEvent_getEventTime(const AInputEvent *e) { (void)e; return 0; }
static int64_t u_AMotionEvent_getHistoricalEventTime(const AInputEvent *e, size_t h) { (void)e; (void)h; return 0; }
static int32_t u_AMotionEvent_getEdgeFlags(const AInputEvent *e) { (void)e; return 0; }
static float u_me_one(const AInputEvent *e, size_t i) { (void)e; (void)i; return 1.0f; }
static float u_me_zero(const AInputEvent *e, size_t i) { (void)e; (void)i; return 0.0f; }
static float u_me_hist(const AInputEvent *e, size_t i, size_t h) { (void)e; (void)i; (void)h; return 1.0f; }
static float u_me_prec(const AInputEvent *e) { (void)e; return 1.0f; }

/* ---- libc --------------------------------------------------------------- */
static int u_gettid(void) { return (int)syscall(SYS_gettid); }
static long u_ptrace(int req, ...) { (void)req; return -1; }
static int u_execl(const char *path, const char *arg, ...) { (void)path; (void)arg; return -1; }
static int u_flock(int fd, int op) { return flock(fd, op); }
static int u_wprintf(const wchar_t *fmt, ...) { (void)fmt; return 0; }
static int u_swscanf(const wchar_t *s, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vswscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- SHA1 (bionic libc API) --------------------------------------------- */
typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t buffer[64];
} U_SHA1_CTX;

#define ROL(v, b) (((v) << (b)) | ((v) >> (32 - (b))))
static void sha1_transform(uint32_t state[5], const uint8_t buffer[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)buffer[i * 4] << 24) | ((uint32_t)buffer[i * 4 + 1] << 16) |
               ((uint32_t)buffer[i * 4 + 2] << 8) | buffer[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}
static void u_SHA1Init(U_SHA1_CTX *c)
{
    c->state[0] = 0x67452301; c->state[1] = 0xEFCDAB89; c->state[2] = 0x98BADCFE;
    c->state[3] = 0x10325476; c->state[4] = 0xC3D2E1F0;
    c->count[0] = c->count[1] = 0;
}
static void u_SHA1Update(U_SHA1_CTX *c, const uint8_t *data, unsigned int len)
{
    unsigned int i, j = (c->count[0] >> 3) & 63;
    if ((c->count[0] += len << 3) < (len << 3))
        c->count[1]++;
    c->count[1] += (len >> 29);
    if (j + len > 63) {
        memcpy(&c->buffer[j], data, (i = 64 - j));
        sha1_transform(c->state, c->buffer);
        for (; i + 63 < len; i += 64)
            sha1_transform(c->state, &data[i]);
        j = 0;
    } else
        i = 0;
    memcpy(&c->buffer[j], &data[i], len - i);
}
static void u_SHA1Final(uint8_t digest[20], U_SHA1_CTX *c)
{
    uint8_t finalcount[8];
    for (int i = 0; i < 8; i++)
        finalcount[i] = (uint8_t)((c->count[(i >= 4 ? 0 : 1)] >> ((3 - (i & 3)) * 8)) & 255);
    u_SHA1Update(c, (const uint8_t *)"\200", 1);
    while ((c->count[0] & 504) != 448)
        u_SHA1Update(c, (const uint8_t *)"\0", 1);
    u_SHA1Update(c, finalcount, 8);
    for (int i = 0; i < 20; i++)
        digest[i] = (uint8_t)((c->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
}

/* ---- libm --------------------------------------------------------------- */
static double u_asin(double x) { return asin(x); }
static float u_atanf(float x) { return atanf(x); }
static float u_fabsf(float x) { return fabsf(x); }
static float u_truncf(float x) { return truncf(x); }
static float u_exp2f(float x) { return exp2f(x); }
static float u_modff(float x, float *ip) { return modff(x, ip); }

} /* extern "C" */

/*
 * The run-time ABI's divmod helpers return two values in registers (quotient in
 * r0[:r1], remainder in r1 / r2:r3), which no C prototype can express: the
 * generated C versions return a struct through a hidden pointer and fault on the
 * first call. libgcc's own are the real thing, and integer-only, so they bind
 * straight through.
 */
extern "C" {
void __aeabi_idivmod(void);
void __aeabi_uidivmod(void);
void __aeabi_ldivmod(void);
void __aeabi_uldivmod(void);
}

DynLibFunction symtable_unity[] = {
    NO_THUNK("__aeabi_idivmod",  (uintptr_t)&__aeabi_idivmod),
    NO_THUNK("__aeabi_uidivmod", (uintptr_t)&__aeabi_uidivmod),
    NO_THUNK("__aeabi_ldivmod",  (uintptr_t)&__aeabi_ldivmod),
    NO_THUNK("__aeabi_uldivmod", (uintptr_t)&__aeabi_uldivmod),

    THUNK_SPECIFIC("ANativeWindow_fromSurface", u_ANativeWindow_fromSurface),
    THUNK_SPECIFIC("ANativeWindow_acquire", u_ANativeWindow_acquire),
    THUNK_SPECIFIC("ANativeWindow_release", u_ANativeWindow_release),
    THUNK_SPECIFIC("ANativeWindow_lock", u_ANativeWindow_lock),
    THUNK_SPECIFIC("ANativeWindow_unlockAndPost", u_ANativeWindow_unlockAndPost),
    THUNK_SPECIFIC("ALooper_forThread", u_ALooper_forThread),
    THUNK_SPECIFIC("ALooper_wake", u_ALooper_wake),

    THUNK_SPECIFIC("ASensorManager_getInstance", u_ASensorManager_getInstance),
    THUNK_SPECIFIC("ASensorManager_getSensorList", u_ASensorManager_getSensorList),
    THUNK_SPECIFIC("ASensorManager_getDefaultSensor", u_ASensorManager_getDefaultSensor),
    THUNK_SPECIFIC("ASensorManager_createEventQueue", u_ASensorManager_createEventQueue),
    THUNK_SPECIFIC("ASensorManager_destroyEventQueue", u_ASensorManager_destroyEventQueue),
    THUNK_SPECIFIC("ASensorEventQueue_enableSensor", u_ASensorEventQueue_enableSensor),
    THUNK_SPECIFIC("ASensorEventQueue_disableSensor", u_ASensorEventQueue_disableSensor),
    THUNK_SPECIFIC("ASensorEventQueue_setEventRate", u_ASensorEventQueue_setEventRate),
    THUNK_SPECIFIC("ASensorEventQueue_hasEvents", u_ASensorEventQueue_hasEvents),
    THUNK_SPECIFIC("ASensorEventQueue_getEvents", u_ASensorEventQueue_getEvents),
    THUNK_SPECIFIC("ASensor_getName", u_ASensor_getName),
    THUNK_SPECIFIC("ASensor_getVendor", u_ASensor_getVendor),
    THUNK_SPECIFIC("ASensor_getType", u_ASensor_getType),
    THUNK_SPECIFIC("ASensor_getResolution", u_ASensor_getResolution),
    THUNK_SPECIFIC("ASensor_getMinDelay", u_ASensor_getMinDelay),

    THUNK_SPECIFIC("AMotionEvent_getDownTime", u_AMotionEvent_getDownTime),
    THUNK_SPECIFIC("AMotionEvent_getEventTime", u_AMotionEvent_getEventTime),
    THUNK_SPECIFIC("AMotionEvent_getHistoricalEventTime", u_AMotionEvent_getHistoricalEventTime),
    THUNK_SPECIFIC("AMotionEvent_getEdgeFlags", u_AMotionEvent_getEdgeFlags),
    THUNK_SPECIFIC("AMotionEvent_getPressure", u_me_one),
    THUNK_SPECIFIC("AMotionEvent_getSize", u_me_one),
    THUNK_SPECIFIC("AMotionEvent_getOrientation", u_me_zero),
    THUNK_SPECIFIC("AMotionEvent_getToolMajor", u_me_one),
    THUNK_SPECIFIC("AMotionEvent_getToolMinor", u_me_one),
    THUNK_SPECIFIC("AMotionEvent_getTouchMajor", u_me_one),
    THUNK_SPECIFIC("AMotionEvent_getTouchMinor", u_me_one),
    THUNK_SPECIFIC("AMotionEvent_getHistoricalPressure", u_me_hist),
    THUNK_SPECIFIC("AMotionEvent_getHistoricalSize", u_me_hist),
    THUNK_SPECIFIC("AMotionEvent_getXPrecision", u_me_prec),
    THUNK_SPECIFIC("AMotionEvent_getYPrecision", u_me_prec),

    THUNK_SPECIFIC("gettid", u_gettid),
    NO_THUNK("ptrace", (uintptr_t)&u_ptrace),
    NO_THUNK("execl", (uintptr_t)&u_execl),
    THUNK_SPECIFIC("flock", u_flock),
    NO_THUNK("wprintf", (uintptr_t)&u_wprintf),
    NO_THUNK("swscanf", (uintptr_t)&u_swscanf),
    THUNK_SPECIFIC("SHA1Init", u_SHA1Init),
    THUNK_SPECIFIC("SHA1Update", u_SHA1Update),
    THUNK_SPECIFIC("SHA1Final", u_SHA1Final),

    THUNK_SPECIFIC("asin", u_asin),
    THUNK_SPECIFIC("atanf", u_atanf),
    THUNK_SPECIFIC("fabsf", u_fabsf),
    THUNK_SPECIFIC("truncf", u_truncf),
    THUNK_SPECIFIC("exp2f", u_exp2f),
    THUNK_SPECIFIC("modff", u_modff),
    { NULL, 0 },
};
