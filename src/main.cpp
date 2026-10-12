/*
 * Shadowgun (Unity 5.3.8f1, IL2CPP, armeabi-v7a) loader - entry point.
 *
 * On Android the Java class com.unity3d.player.UnityPlayer drives the engine:
 * it loads libmain.so, which loads libunity.so (which in turn dlopens
 * libil2cpp.so), and then calls the natives libunity registered from its
 * JNI_OnLoad. There is no Java here, so this file plays UnityPlayer. The call
 * order is transcribed from the game's own classes.dex (UnityPlayer.<init> and
 * the UnityMain thread):
 *
 *     initJni(context)
 *     nativeFile(apk path)            the engine reads assets/bin/Data out of the zip
 *     nativeFile(obb path)            "useObb" in assets/bin/Data/settings.xml
 *     nativeInitWWW / nativeInitWebRequest
 *     nativeRecreateGfxState(0, surface)
 *     nativeResume, nativeFocusChanged(true)
 *     nativeRender()                  once per frame; presents through eglSwapBuffers
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>

#include <pthread.h>

#include <SDL2/SDL.h>

#include "so_util.h"
#include "khronos/gles2.h"

#include "jni.h"
#include "dyn.h"

#include "app_exit.h"
#include "crash.h"
#include "fps_unlock.h"
#include "fix_path.h"
#include "gl_bridge.h"
#include "port_env.h"
#include "pc_prof.h"
#include "port_version.h"
#include "syscall_prof.h"
#include "trace.h"
#include "unity_java.h"
#include "control.h"

extern "C" void android_egl_init(SDL_Window *window, SDL_GLContext gl);
extern "C" void android_platform_init(SDL_Window *win, int w, int h);
extern "C" void viewport_scale_init(int phys_w, int phys_h);
extern "C" long android_egl_frames(void);
extern "C" void crash_report_thread_init(void);
extern "C" void android_egl_set_swap_hook(void (*hook)(long frame));

static const char *kNativeLibDir = "lib/armeabi-v7a";

static int kWidth  = 1280;
static int kHeight = 720;

static so_module *g_unity = NULL;

/* Kept for the inherited pthread/crash helpers, which name one module. */
so_module *shadowgun_module(void) { return g_unity; }

static int report_unresolved_symbols(so_module *mod)
{
    int missing = 0;

    for (int i = 0; i < mod->num_dynsym; i++) {
        Elf_Sym *sym = &mod->dynsym[i];
        if (sym->st_shndx != SHN_UNDEF)
            continue;

        const char *name = mod->dynstr + sym->st_name;
        if (!name || !*name)
            continue;

        if (so_resolve_link(mod, name))
            continue;

        if (ELF32_ST_BIND(sym->st_info) == STB_WEAK)
            continue;

        fprintf(stderr, "unresolved symbol: %s (%s)\n", name, mod->soname ? mod->soname : "?");
        missing++;
    }

    fflush(stderr);
    return missing;
}

/*
 * The loader's post-relocation hook, once per module (libmain, libunity, and
 * later libil2cpp from inside dlopen). Missing imports are listed here, before
 * any of the module's code runs; they are fatal unless SHADOWGUN_ALLOW_UNRESOLVED
 * is set, in which case the first call to one aborts with its PLT stub.
 */
extern "C" int so_after_relocate(so_module *mod)
{
    const char *name = mod->soname ? mod->soname : "module";
    trace("module loaded: %s text=%p size=%zu", name, (void *)mod->text_base, (size_t)mod->text_size);

    if (mod->soname && strcmp(mod->soname, "libunity.so") == 0) {
        g_unity = mod;
        crash_report_init(mod, "libunity.so");
    }

    int missing = report_unresolved_symbols(mod);
    if (missing == 0)
        return 0;

    if (port_getenv_long("ALLOW_UNRESOLVED", 0)) {
        trace("%d import(s) of %s have no implementation; continuing as asked", missing, name);
        return 0;
    }
    fatal("%d import(s) of %s have no implementation (listed above).", missing, name);
    return 1;
}

static bool find_obb(const char *dir, char *out, size_t outsz)
{
    DIR *d = opendir(dir);
    if (!d)
        return false;
    bool found = false;
    /* readdir64: under a 32-bit qemu-user the plain call fails with EOVERFLOW on
     * filesystems that hand out 64-bit directory offsets. */
    struct dirent64 *e;
    while ((e = readdir64(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n > 4 && strcmp(e->d_name + n - 4, ".obb") == 0 && strncmp(e->d_name, "main.", 5) == 0) {
            snprintf(out, outsz, "%s/%s", dir, e->d_name);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

/*
 * Bring-up aid for runs on the PC: SHADOWGUN_SHOT_EVERY=N writes every Nth
 * presented frame to ctl/shot_<frame>.bmp, read back just before the swap.
 */
static long g_shot_every = 0;
static void swap_hook(long frame)
{
    if (!g_shot_every || frame % g_shot_every != 0 || gl_bridge_enabled())
        return;
    void (*read_pixels)(int, int, int, int, unsigned, unsigned, void *) =
        (decltype(read_pixels))SDL_GL_GetProcAddress("glReadPixels");
    if (!read_pixels)
        return;
    const int w = kWidth, h = kHeight;
    unsigned char *px = (unsigned char *)malloc((size_t)w * h * 4);
    if (!px)
        return;
    read_pixels(0, 0, w, h, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, px);
    char path[64];
    mkdir("ctl", 0755);
    snprintf(path, sizeof(path), "ctl/shot_%06ld.bmp", frame);
    FILE *f = fopen(path, "wb");
    if (f) {
        unsigned size = 54 + (unsigned)w * h * 3;
        unsigned char hdr[54] = {'B', 'M'};
        memcpy(hdr + 2, &size, 4);
        hdr[10] = 54; hdr[14] = 40;
        memcpy(hdr + 18, &w, 4);
        memcpy(hdr + 22, &h, 4);   /* positive height: bottom-up, as GL returns it */
        hdr[26] = 1; hdr[28] = 24;
        fwrite(hdr, 1, 54, f);
        unsigned char *row = (unsigned char *)malloc((size_t)w * 3);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                row[x * 3 + 0] = px[(y * w + x) * 4 + 2];
                row[x * 3 + 1] = px[(y * w + x) * 4 + 1];
                row[x * 3 + 2] = px[(y * w + x) * 4 + 0];
            }
            fwrite(row, 1, (size_t)w * 3, f);
        }
        free(row);
        fclose(f);
        trace("screenshot %s", path);
    }
    free(px);
}

template <typename F> static F native(const char *name)
{
    void *p = dyn_native("com/unity3d/player/UnityPlayer", name);
    if (!p)
        trace("UnityPlayer.%s was not registered by libunity", name);
    return (F)p;
}

static int port_main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        printf("%s\n", SHADOWGUN_PORT_VERSION);
        return 0;
    }

    trace("Shadowgun port v%s", SHADOWGUN_PORT_VERSION);

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <game-directory>\n"
                "\n"
                "The directory holds your own copy of the game:\n"
                "  lib/armeabi-v7a/{libmain,libunity,libil2cpp}.so\n"
                "  base.apk   (the original APK; the engine reads its assets from it)\n"
                "  main.*.obb (the expansion file)\n",
                argv[0]);
        return 2;
    }

    char game_dir[PATH_MAX];
    if (!realpath(argv[1], game_dir)) {
        fatal("'%s': %s", argv[1], strerror(errno));
        return 1;
    }
    io_set_game_dir(game_dir);

    {
        long w = port_getenv_long("WIDTH", kWidth), h = port_getenv_long("HEIGHT", kHeight);
        if (w >= 320 && w <= 4096 && h >= 240 && h <= 4096) {
            kWidth = (int)w;
            kHeight = (int)h;
        }
    }

    char lib_dir[PATH_MAX], apk_path[PATH_MAX], obb_path[PATH_MAX] = "";
    snprintf(lib_dir, sizeof(lib_dir), "%s/%s", game_dir, kNativeLibDir);
    snprintf(apk_path, sizeof(apk_path), "%s/base.apk", game_dir);

    struct stat st;
    if (stat(apk_path, &st) != 0) {
        fatal("'%s' does not exist.", apk_path);
        return 1;
    }
    /* The game's own script looks for the expansion file where Android keeps it,
     * relative to "external storage" - which is the game directory here. */
    char obb_dir[PATH_MAX];
    snprintf(obb_dir, sizeof(obb_dir), "%s/Android/obb/com.madfingergames.shadowgun", game_dir);
    if (!find_obb(obb_dir, obb_path, sizeof(obb_path)) && !find_obb(game_dir, obb_path, sizeof(obb_path)))
        trace("no main.*.obb in %s - the engine will only see the APK", game_dir);

    /* GL before the modules: the import tables are filled from the live context. */
    SDL_Window   *window = NULL;
    SDL_GLContext gl     = NULL;

    if (gl_bridge_enabled()) {
        /* GL runs in the Android app; the window and the context are only tokens here. */
        if (SDL_Init(SDL_INIT_VIDEO) != 0)
            trace("SDL_Init(video) failed: %s", SDL_GetError());
        window = SDL_CreateWindow("Shadowgun", 0, 0, kWidth, kHeight, 0);
        if (!gl_bridge_init()) {
            fatal("could not connect to the GL host.");
            return 1;
        }
        gl = (SDL_GLContext)(uintptr_t)1;
    } else if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        trace("SDL_Init(video) failed: %s", SDL_GetError());
    } else {
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        window = SDL_CreateWindow("Shadowgun", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  kWidth, kHeight, SDL_WINDOW_OPENGL);
        if (!window)
            trace("SDL_CreateWindow failed: %s", SDL_GetError());
        else if (!(gl = SDL_GL_CreateContext(window)))
            trace("no GLES 2.0 context: %s", SDL_GetError());
    }

    {
        /* The inherited viewport remap is for a fixed-size engine; Unity renders at
         * whatever the surface reports, so logical == physical. */
        char dim[16];
        snprintf(dim, sizeof(dim), "%d", kWidth);
        setenv("SHADOWGUN_SCREEN_W", dim, 1);
        snprintf(dim, sizeof(dim), "%d", kHeight);
        setenv("SHADOWGUN_SCREEN_H", dim, 1);
    }
    if (gl) {
        load_gles1_funcs();
        load_gles2_funcs();
        viewport_scale_init(kWidth, kHeight);
        if (!gl_bridge_enabled()) {
            const GLubyte *(*get_string)(GLenum) =
                (const GLubyte *(*)(GLenum))SDL_GL_GetProcAddress("glGetString");
            if (get_string)
                trace("GL_VERSION=%s | GL_RENDERER=%s", (const char *)get_string(0x1F02),
                      (const char *)get_string(0x1F01));
            /* The engine makes the context current itself, possibly on another thread. */
            SDL_GL_MakeCurrent(window, NULL);
        }
    }
    g_shot_every = port_getenv_long("SHOT_EVERY", 0);
    android_egl_set_swap_hook(swap_hook);
    android_egl_init(window, gl);
    android_platform_init(window, kWidth, kHeight);

    JavaVM *vm  = NULL;
    JNIEnv *env = NULL;
    if (JNI_CreateJavaVM(&vm, &env, NULL) != JNI_OK || !vm || !env) {
        fatal("could not create the JNI environment.");
        return 1;
    }

    UnityJavaConfig cfg;
    cfg.game_dir = game_dir;
    cfg.apk_path = apk_path;
    cfg.obb_path = obb_path;
    cfg.width = kWidth;
    cfg.height = kHeight;
    unity_java_init(env, cfg);
    control_init();

    so_set_options(NULL, lib_dir);

    /* libunity.so names libmain.so in DT_NEEDED, so this maps both. */
    so_module *unity = so_load_module("libunity.so", NULL, NULL);
    if (!unity) {
        fatal("could not load libunity.so from '%s'.", lib_dir);
        fflush(NULL);
        _exit(1);
    }

    auto JNI_OnLoad = (int (*)(JavaVM *, void *))so_symbol(unity, "JNI_OnLoad");
    if (!JNI_OnLoad) {
        fatal("libunity.so exports no JNI_OnLoad.");
        _exit(1);
    }
    trace("-> libunity JNI_OnLoad");
    int ver = JNI_OnLoad(vm, NULL);
    trace("<- JNI_OnLoad = 0x%x", ver);

    typedef jobject self_t;
    auto initJni         = native<void (*)(JNIEnv *, self_t, jobject)>("initJni");
    auto nativeFile      = native<void (*)(JNIEnv *, self_t, jstring)>("nativeFile");
    auto nativeInitWWW   = native<void (*)(JNIEnv *, self_t, jclass)>("nativeInitWWW");
    auto nativeInitWebRq = native<void (*)(JNIEnv *, self_t, jclass)>("nativeInitWebRequest");
    auto nativeGfx       = native<void (*)(JNIEnv *, self_t, jint, jobject)>("nativeRecreateGfxState");
    auto nativeResume    = native<void (*)(JNIEnv *, self_t)>("nativeResume");
    auto nativeFocus     = native<void (*)(JNIEnv *, self_t, jboolean)>("nativeFocusChanged");
    auto nativeRender    = native<jboolean (*)(JNIEnv *, self_t)>("nativeRender");

    if (!initJni || !nativeFile || !nativeGfx || !nativeResume || !nativeRender) {
        fatal("libunity.so did not register the UnityPlayer natives the port drives.");
        fflush(NULL);
        _exit(1);
    }

    jobject player  = unity_java_player();
    jobject context = unity_java_activity();

    trace("-> initJni");
    initJni(env, player, context);
    trace("-> nativeFile %s", apk_path);
    nativeFile(env, player, env->NewStringUTF(apk_path));
    if (obb_path[0]) {
        trace("-> nativeFile %s", obb_path);
        nativeFile(env, player, env->NewStringUTF(obb_path));
    }
    if (nativeInitWWW)
        nativeInitWWW(env, player, (jclass)dyn_class("com/unity3d/player/WWW"));
    if (nativeInitWebRq)
        nativeInitWebRq(env, player, (jclass)dyn_class("com/unity3d/player/UnityWebRequest"));

    trace("-> nativeRecreateGfxState");
    nativeGfx(env, player, 0, unity_java_surface());
    trace("-> nativeResume");
    nativeResume(env, player);
    if (nativeFocus)
        nativeFocus(env, player, 1);

    const long frame_limit = port_getenv_long("FRAME_LIMIT", 0);
    long frames = 0;
    while (frame_limit == 0 || frames < frame_limit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                goto done;
        }
        if (android_app_exit_requested())
            goto done;

        if (!control_tick(env, frames))
            goto done;
        unity_java_frame(env, frames);
        fps_unlock_tick(frames);
        sysprof_tick(frames);
        pc_prof_tick(frames);

        if (frames < 5)
            trace("-> nativeRender #%ld", frames + 1);
        gl_bridge_profile_step(1);
        jboolean alive = nativeRender(env, player);
        gl_bridge_profile_step(0);
        frames++;
        if (frames <= 5 || frames % 100 == 0)
            trace("frames=%ld presented=%ld", frames, android_egl_frames());
        if (!alive) {
            trace("nativeRender returned false - the engine asked to quit");
            break;
        }
    }

done:
    control_shutdown(frames);
    trace("run finished: %ld frames, %ld presented", frames, android_egl_frames());
    fflush(NULL);
    _exit(0);
}

/*
 * The engine does not run on the process's initial thread. On Android it lives
 * on the "UnityMain" Java thread; here it gets a thread of its own with a stack
 * sized for the engine plus the host code underneath every libc and JNI call.
 * On the initial thread under qemu-user the same start-up ran out of stack about
 * one launch in two.
 */
struct MainArgs { int argc; char **argv; int rc; };
static void *main_thread(void *p)
{
    MainArgs *a = (MainArgs *)p;
    crash_report_thread_init();
    {
        pthread_attr_t at;
        void *base = NULL;
        size_t size = 0;
        if (pthread_getattr_np(pthread_self(), &at) == 0) {
            pthread_attr_getstack(&at, &base, &size);
            pthread_attr_destroy(&at);
        }
        int marker = 0;
        trace("engine thread stack %p..%p (%zu KiB), sp near %p", base, (char *)base + size, size / 1024, (void *)&marker);
    }
    a->rc = port_main(a->argc, a->argv);
    return NULL;
}

int main(int argc, char **argv)
{
    MainArgs args = {argc, argv, 1};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u * 1024 * 1024);
    pthread_t t;
    if (pthread_create(&t, &attr, main_thread, &args) != 0)
        return port_main(argc, argv);
    pthread_join(t, NULL);
    return args.rc;
}
