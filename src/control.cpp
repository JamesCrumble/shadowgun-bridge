/* See control.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>
#include <vector>

#include "control.h"
#include "dyn.h"
#include "trace.h"
#include "unity_java.h"

namespace {

bool g_enabled = false;
std::string g_dir;
FILE *g_commands = NULL;
std::string g_partial;

void write_status(long frame, const char *state)
{
    if (!g_enabled)
        return;
    std::string tmp = g_dir + "/status.json.tmp", fin = g_dir + "/status.json";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f)
        return;
    fprintf(f, "{\"state\":\"%s\",\"frame\":%ld}\n", state, frame);
    fclose(f);
    rename(tmp.c_str(), fin.c_str());
}

/* ---- android.view.MotionEvent / KeyEvent, as much as the engine reads ------ */
struct Pointer { int id; float x, y; };
struct MotionData {
    int action;
    long long time_ms, down_ms;
    std::vector<Pointer> ptr;
};
struct KeyData { int action, code; long long time_ms; };

std::vector<Pointer> g_down;       /* pointers currently on the surface */
long long g_down_time = 0;

long long now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

MotionData *md(jobject o)
{
    static MotionData empty{0, 0, 0, {}};
    MotionData *m = o ? (MotionData *)((DynObject *)o)->priv : NULL;
    return m ? m : &empty;
}
KeyData *kd(jobject o) { return (KeyData *)((DynObject *)o)->priv; }

void register_events()
{
    const char *M = "android/view/MotionEvent";
    dyn_method(M, "getAction()I", [](JNIEnv *, jobject s, const jvalue *) { return JI(md(s)->action); });
    dyn_method(M, "getActionMasked()I", [](JNIEnv *, jobject s, const jvalue *) { return JI(md(s)->action & 0xff); });
    dyn_method(M, "getActionIndex()I", [](JNIEnv *, jobject s, const jvalue *) { return JI((md(s)->action >> 8) & 0xff); });
    dyn_method(M, "getPointerCount()I", [](JNIEnv *, jobject s, const jvalue *) { return JI((jint)md(s)->ptr.size()); });
    dyn_method(M, "getPointerId(I)I", [](JNIEnv *, jobject s, const jvalue *a) {
        MotionData *m = md(s);
        return JI(a[0].i >= 0 && (size_t)a[0].i < m->ptr.size() ? m->ptr[a[0].i].id : 0);
    });
    dyn_method(M, "getX(I)F", [](JNIEnv *, jobject s, const jvalue *a) {
        MotionData *m = md(s);
        return JF(a[0].i >= 0 && (size_t)a[0].i < m->ptr.size() ? m->ptr[a[0].i].x : 0.0f);
    });
    dyn_method(M, "getY(I)F", [](JNIEnv *, jobject s, const jvalue *a) {
        MotionData *m = md(s);
        return JF(a[0].i >= 0 && (size_t)a[0].i < m->ptr.size() ? m->ptr[a[0].i].y : 0.0f);
    });
    dyn_method(M, "getX()F", [](JNIEnv *, jobject s, const jvalue *) { return JF(md(s)->ptr.empty() ? 0.0f : md(s)->ptr[0].x); });
    dyn_method(M, "getY()F", [](JNIEnv *, jobject s, const jvalue *) { return JF(md(s)->ptr.empty() ? 0.0f : md(s)->ptr[0].y); });
    dyn_method(M, "getRawX()F", [](JNIEnv *, jobject s, const jvalue *) { return JF(md(s)->ptr.empty() ? 0.0f : md(s)->ptr[0].x); });
    dyn_method(M, "getRawY()F", [](JNIEnv *, jobject s, const jvalue *) { return JF(md(s)->ptr.empty() ? 0.0f : md(s)->ptr[0].y); });
    dyn_method(M, "getEventTime()J", [](JNIEnv *, jobject s, const jvalue *) { return JJ(md(s)->time_ms); });
    dyn_method(M, "getDownTime()J", [](JNIEnv *, jobject s, const jvalue *) { return JJ(md(s)->down_ms); });
    dyn_method(M, "getSource()I", [](JNIEnv *, jobject, const jvalue *) { return JI(0x1002); });   /* SOURCE_TOUCHSCREEN */
    dyn_method(M, "getToolType(I)I", [](JNIEnv *, jobject, const jvalue *) { return JI(1); });     /* TOOL_TYPE_FINGER */
    dyn_method(M, "getPressure(I)F", [](JNIEnv *, jobject, const jvalue *) { return JF(1.0f); });
    dyn_method(M, "getSize(I)F", [](JNIEnv *, jobject, const jvalue *) { return JF(0.1f); });
    for (const char *zero : {"getDeviceId()I", "getHistorySize()I", "getMetaState()I", "getButtonState()I",
                             "getEdgeFlags()I", "getFlags()I"})
        dyn_method(M, zero, [](JNIEnv *, jobject, const jvalue *) { return JI(0); });

    /* MotionEvent.obtain(MotionEvent): the engine copies every event before reading it. */
    dyn_method(M, "obtain(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;", [](JNIEnv *, jobject, const jvalue *a) {
        DynObject *src = (DynObject *)a[0].l;
        DynObject *o = (DynObject *)dyn_new("android/view/MotionEvent");
        o->priv = new MotionData(*md((jobject)src));
        o->fields["copy"] = JI(1);
        return JL((jobject)o);
    });
    dyn_method(M, "recycle()V", [](JNIEnv *, jobject s, const jvalue *) {
        DynObject *o = (DynObject *)s;
        if (o && o->fields.count("copy") && o->priv) {
            delete (MotionData *)o->priv;
            o->priv = NULL;
        }
        return JV();
    });
    dyn_method(M, "findPointerIndex(I)I", [](JNIEnv *, jobject s, const jvalue *a) {
        MotionData *m = md(s);
        for (size_t i = 0; i < m->ptr.size(); i++)
            if (m->ptr[i].id == a[0].i)
                return JI((jint)i);
        return JI(-1);
    });
    /* getAxisValue(axis, pointerIndex): AXIS_X = 0, AXIS_Y = 1; everything else is 0. */
    dyn_method(M, "getAxisValue(II)F", [](JNIEnv *, jobject s, const jvalue *a) {
        MotionData *m = md(s);
        if (a[1].i < 0 || (size_t)a[1].i >= m->ptr.size())
            return JF(0.0f);
        return JF(a[0].i == 0 ? m->ptr[a[1].i].x : a[0].i == 1 ? m->ptr[a[1].i].y : 0.0f);
    });
    dyn_method(M, "getAxisValue(I)F", [](JNIEnv *, jobject s, const jvalue *a) {
        MotionData *m = md(s);
        if (m->ptr.empty())
            return JF(0.0f);
        return JF(a[0].i == 0 ? m->ptr[0].x : a[0].i == 1 ? m->ptr[0].y : 0.0f);
    });
    dyn_method(M, "getHistoricalX(II)F", [](JNIEnv *, jobject, const jvalue *) { return JF(0.0f); });
    dyn_method(M, "getHistoricalY(II)F", [](JNIEnv *, jobject, const jvalue *) { return JF(0.0f); });
    dyn_method(M, "getHistoricalEventTime(I)J", [](JNIEnv *, jobject s, const jvalue *) { return JJ(md(s)->time_ms); });

    /* The engine looks these up on the base class, whatever the event really is. */
    const char *I = "android/view/InputEvent";
    dyn_method(I, "getSource()I", [](JNIEnv *, jobject s, const jvalue *) {
        return JI(dyn_is(s, "android/view/KeyEvent") ? 0x101 : 0x1002);   /* KEYBOARD : TOUCHSCREEN */
    });
    dyn_method(I, "getDeviceId()I", [](JNIEnv *, jobject, const jvalue *) { return JI(0); });
    dyn_method(I, "getEventTime()J", [](JNIEnv *, jobject s, const jvalue *) {
        return JJ(dyn_is(s, "android/view/KeyEvent") ? kd(s)->time_ms : md(s)->time_ms);
    });
    dyn_method(I, "getDevice()Landroid/view/InputDevice;", [](JNIEnv *, jobject, const jvalue *) { return JL(NULL); });

    const char *K = "android/view/KeyEvent";
    dyn_method(K, "getAction()I", [](JNIEnv *, jobject s, const jvalue *) { return JI(kd(s)->action); });
    dyn_method(K, "getKeyCode()I", [](JNIEnv *, jobject s, const jvalue *) { return JI(kd(s)->code); });
    dyn_method(K, "getEventTime()J", [](JNIEnv *, jobject s, const jvalue *) { return JJ(kd(s)->time_ms); });
    dyn_method(K, "getDownTime()J", [](JNIEnv *, jobject s, const jvalue *) { return JJ(kd(s)->time_ms); });
    dyn_method(K, "getSource()I", [](JNIEnv *, jobject, const jvalue *) { return JI(0x101); });      /* SOURCE_KEYBOARD */
    dyn_method(K, "getUnicodeChar()I", [](JNIEnv *, jobject, const jvalue *) { return JI(0); });
    dyn_method(K, "getUnicodeChar(I)I", [](JNIEnv *, jobject, const jvalue *) { return JI(0); });
    for (const char *zero : {"getDeviceId()I", "getMetaState()I", "getRepeatCount()I", "getFlags()I", "getScanCode()I"})
        dyn_method(K, zero, [](JNIEnv *, jobject, const jvalue *) { return JI(0); });
}

typedef jboolean (*InjectFn)(JNIEnv *, jobject, jobject);
InjectFn inject_fn()
{
    static InjectFn fn = (InjectFn)dyn_native("com/unity3d/player/UnityPlayer", "nativeInjectEvent");
    return fn;
}

void inject_motion(JNIEnv *env, int action, const std::vector<Pointer> &ptrs)
{
    InjectFn fn = inject_fn();
    if (!fn)
        return;
    /*
     * Events are not freed when the call returns: the engine may keep the
     * reference and read it on a later frame. A ring of recent events bounds the
     * memory instead.
     */
    static DynObject *ring[256];
    static unsigned ring_pos = 0;
    DynObject *&slot = ring[ring_pos++ % 256];
    if (slot) {
        delete (MotionData *)slot->priv;
        delete slot;
    }
    DynObject *o = (DynObject *)dyn_new("android/view/MotionEvent");
    o->priv = new MotionData{action, now_ms(), g_down_time, ptrs};
    slot = o;
    fn(env, unity_java_player(), (jobject)o);
}

/* touch <action> <x> <y> <id> from the wrapper: 1 down, 2 move, 0 up. */
void on_touch(JNIEnv *env, int act, float x, float y, int id)
{
    int idx = -1;
    for (size_t i = 0; i < g_down.size(); i++)
        if (g_down[i].id == id)
            idx = (int)i;

    if (act == 1) {
        if (idx >= 0) {
            g_down[idx].x = x;
            g_down[idx].y = y;
            inject_motion(env, 2, g_down);
            return;
        }
        g_down.push_back({id, x, y});
        idx = (int)g_down.size() - 1;
        if (g_down.size() == 1) {
            g_down_time = now_ms();
            inject_motion(env, 0, g_down);                    /* ACTION_DOWN */
        } else {
            inject_motion(env, 5 | (idx << 8), g_down);       /* ACTION_POINTER_DOWN */
        }
    } else if (act == 2) {
        if (idx < 0)
            return;
        g_down[idx].x = x;
        g_down[idx].y = y;
        inject_motion(env, 2, g_down);                        /* ACTION_MOVE */
    } else {
        if (idx < 0)
            return;
        g_down[idx].x = x;
        g_down[idx].y = y;
        inject_motion(env, g_down.size() == 1 ? 1 : (6 | (idx << 8)), g_down);   /* UP / POINTER_UP */
        g_down.erase(g_down.begin() + idx);
    }
}

void on_key(JNIEnv *env, int code, bool down)
{
    InjectFn fn = inject_fn();
    if (!fn)
        return;
    static DynObject *ring[64];
    static unsigned ring_pos = 0;
    DynObject *&slot = ring[ring_pos++ % 64];
    if (slot) {
        delete (KeyData *)slot->priv;
        delete slot;
    }
    DynObject *o = (DynObject *)dyn_new("android/view/KeyEvent");
    o->priv = new KeyData{down ? 0 : 1, code, now_ms()};
    slot = o;
    fn(env, unity_java_player(), (jobject)o);
}

/* Returns false on quit. */
bool run_line(JNIEnv *env, const char *line)
{
    int act, id, code;
    float x, y;
    char word[16];
    if (sscanf(line, "touch %d %f %f %d", &act, &x, &y, &id) == 4) {
        static int seen = 0;
        if (act != 2 && seen < 6) {
            seen++;
            trace("control: touch %d at %.0f,%.0f id %d (inject %s)", act, x, y, id, inject_fn() ? "ok" : "MISSING");
        }
        on_touch(env, act, x, y, id);
    } else if (sscanf(line, "key %d %15s", &code, word) == 2) {
        on_key(env, code, strcmp(word, "down") == 0);
    } else if (strncmp(line, "pause", 5) == 0 || strncmp(line, "resume", 6) == 0) {
        bool pause = line[0] == 'p';
        if (pause) {
            auto fn = (jboolean (*)(JNIEnv *, jobject))dyn_native("com/unity3d/player/UnityPlayer", "nativePause");
            if (fn)
                fn(env, unity_java_player());
        } else {
            auto fn = (void (*)(JNIEnv *, jobject))dyn_native("com/unity3d/player/UnityPlayer", "nativeResume");
            auto focus = (void (*)(JNIEnv *, jobject, jboolean))dyn_native("com/unity3d/player/UnityPlayer", "nativeFocusChanged");
            if (fn)
                fn(env, unity_java_player());
            if (focus)
                focus(env, unity_java_player(), 1);
        }
    } else if (strncmp(line, "quit", 4) == 0) {
        return false;
    }
    return true;
}

} // namespace

void control_init(void)
{
    register_events();
    const char *dir = getenv("SHADOWGUN_CONTROL_DIR");
    if (!dir || !*dir)
        return;
    g_dir = dir;
    std::string path = g_dir + "/commands";
    FILE *create = fopen(path.c_str(), "a");
    if (create)
        fclose(create);
    g_commands = fopen(path.c_str(), "r");
    if (!g_commands) {
        trace("control: cannot open %s", path.c_str());
        return;
    }
    g_enabled = true;
    write_status(0, "starting");
    trace("control: reading %s", path.c_str());
}

bool control_tick(JNIEnv *env, long frame)
{
    if (!g_enabled)
        return true;
    char buf[512];
    while (fgets(buf, sizeof(buf), g_commands)) {
        g_partial += buf;
        if (g_partial.empty() || g_partial.back() != '\n')
            continue;                       /* the writer is mid-line */
        std::string line;
        line.swap(g_partial);
        if (!run_line(env, line.c_str()))
            return false;
    }
    clearerr(g_commands);
    if (frame % 30 == 0)
        write_status(frame, "running");
    return true;
}

void control_shutdown(long frame)
{
    write_status(frame, "stopped");
}
