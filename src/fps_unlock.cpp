/* Frame-rate unlock; see fps_unlock.h. */

#include <string.h>

#include "fps_unlock.h"
#include "platform.h"
#include "port_env.h"
#include "trace.h"

typedef void (*add_icall_fn)(const char *name, void *fn);
typedef void (*set_fps_fn)(int value);

static add_icall_fn g_real_add_icall = NULL;
static set_fps_fn   g_real_set_fps = NULL;
static int          g_forced_fps = -1;      /* -1: not read yet */

static int forced_fps()
{
    if (g_forced_fps < 0)
        g_forced_fps = (int)port_getenv_long("TARGET_FPS", 120);
    return g_forced_fps;
}

/* What the guest calls instead of Application::set_targetFrameRate. */
static ABI_ATTR void set_fps_hook(int value)
{
    int want = forced_fps();
    if (want > 0 && value != want) {
        trace("fps unlock: game asked for targetFrameRate %d, forcing %d", value, want);
        value = want;
    }
    if (g_real_set_fps)
        g_real_set_fps(value);
}

/* What the guest calls instead of il2cpp_add_internal_call. */
static ABI_ATTR void add_icall_hook(const char *name, void *fn)
{
    if (name && !strcmp(name, "UnityEngine.Application::set_targetFrameRate") && forced_fps() > 0) {
        g_real_set_fps = (set_fps_fn)fn;
        fn = (void *)set_fps_hook;
        trace("fps unlock: hooked %s, forcing %d fps", name, forced_fps());
    }
    g_real_add_icall(name, fn);
}

extern "C" void *fps_unlock_hook_symbol(const char *name, void *sym)
{
    if (sym && name && !strcmp(name, "il2cpp_add_internal_call") && !g_real_add_icall) {
        g_real_add_icall = (add_icall_fn)sym;
        return (void *)add_icall_hook;
    }
    return sym;
}

extern "C" void fps_unlock_tick(long frame)
{
    if (frame == 20 && g_real_set_fps && forced_fps() > 0) {
        trace("fps unlock: setting targetFrameRate %d", forced_fps());
        g_real_set_fps(forced_fps());
    }
}
