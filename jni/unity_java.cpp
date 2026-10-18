/* See unity_java.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <map>
#include <mutex>
#include <string>

#include "dyn.h"
#include "unity_java.h"
#include "so_util.h"
#include "trace.h"
#include <vector>

void fmod_audio_register(JNIEnv *env);
void unity_www_register(JNIEnv *env);

static UnityJavaConfig g_cfg;
static std::string g_game_dir, g_apk, g_obb, g_files_dir, g_cache_dir;
static jobject g_player, g_activity, g_surface;

static const char *kPackage = "com.madfingergames.shadowgun";

jobject unity_java_player(void) { return g_player; }
jobject unity_java_activity(void) { return g_activity; }
jobject unity_java_surface(void) { return g_surface; }
/*
 * UnityPlayer.UnitySendMessage from the Java side: queued by the plugin fakes,
 * delivered on the engine's thread at the top of the next frame.
 */
struct UnityMessage { std::string object, method, arg; };
static std::mutex g_msg_lock;
static std::vector<UnityMessage> g_messages;

static void send_message(const std::string &object, const char *method, const char *arg)
{
    std::lock_guard<std::mutex> g(g_msg_lock);
    g_messages.push_back({object, method, arg});
}

void unity_java_frame(JNIEnv *env, long frame)
{
    (void)frame;
    std::vector<UnityMessage> batch;
    {
        std::lock_guard<std::mutex> g(g_msg_lock);
        batch.swap(g_messages);
    }
    if (batch.empty())
        return;
    auto fn = (void (*)(JNIEnv *, jclass, jstring, jstring, jstring))
        dyn_native("com/unity3d/player/UnityPlayer", "nativeUnitySendMessage");
    if (!fn)
        return;
    jclass cls = (jclass)dyn_class("com/unity3d/player/UnityPlayer");
    for (const UnityMessage &m : batch) {
        trace("UnitySendMessage(%s, %s, %s)", m.object.c_str(), m.method.c_str(), m.arg.c_str());
        fn(env, cls, env->NewStringUTF(m.object.c_str()), env->NewStringUTF(m.method.c_str()),
           env->NewStringUTF(m.arg.c_str()));
    }
}

/* ---- java.io.File --------------------------------------------------------- */
static jobject new_file(const std::string &path)
{
    DynObject *o = (DynObject *)dyn_new("java/io/File");
    o->priv = strdup(path.c_str());
    return (jobject)o;
}
static const char *file_path(jobject f)
{
    return (f && ((DynObject *)f)->priv) ? (const char *)((DynObject *)f)->priv : "";
}

/* ---- boxed values / proxies ----------------------------------------------- */
static jobject new_boxed(const char *cls, jvalue v)
{
    DynObject *o = (DynObject *)dyn_new(cls);
    o->fields["value"] = v;
    return (jobject)o;
}
static jvalue boxed(jobject o)
{
    if (!o)
        return JV();
    return ((DynObject *)o)->fields["value"];
}

/* ---- SharedPreferences (PlayerPrefs), kept in <files>/playerprefs.txt ------ */
static std::mutex g_prefs_lock;
static std::map<std::string, std::string> g_prefs;   /* key -> "i:123" / "f:1.5" / "s:text" */
static std::string g_prefs_path;

static void prefs_load()
{
    FILE *f = fopen(g_prefs_path.c_str(), "r");
    if (!f)
        return;
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) > 0) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = 0;
        g_prefs[line] = tab + 1;
    }
    free(line);
    fclose(f);
}
static void prefs_save()
{
    std::string tmp = g_prefs_path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f)
        return;
    for (auto &kv : g_prefs)
        fprintf(f, "%s\t%s\n", kv.first.c_str(), kv.second.c_str());
    fclose(f);
    rename(tmp.c_str(), g_prefs_path.c_str());
}
static bool prefs_get(const char *key, char type, std::string *out)
{
    std::lock_guard<std::mutex> g(g_prefs_lock);
    auto it = g_prefs.find(key);
    if (it == g_prefs.end() || it->second.size() < 2 || it->second[0] != type)
        return false;
    *out = it->second.substr(2);
    return true;
}
static void prefs_put(const char *key, char type, const std::string &v)
{
    std::lock_guard<std::mutex> g(g_prefs_lock);
    g_prefs[key] = std::string(1, type) + ":" + v;
}

static void register_context()
{
    const char *C = "*";
    dyn_method(C, "getPackageName()Ljava/lang/String;", [](JNIEnv *e, jobject, const jvalue *) { return JS(e, kPackage); });
    dyn_method(C, "getPackageCodePath()Ljava/lang/String;", [](JNIEnv *e, jobject, const jvalue *) { return JS(e, g_apk.c_str()); });
    dyn_method(C, "getPackageResourcePath()Ljava/lang/String;", [](JNIEnv *e, jobject, const jvalue *) { return JS(e, g_apk.c_str()); });
    dyn_method(C, "getFilesDir()Ljava/io/File;", [](JNIEnv *, jobject, const jvalue *) { return JL(new_file(g_files_dir)); });
    dyn_method(C, "getCacheDir()Ljava/io/File;", [](JNIEnv *, jobject, const jvalue *) { return JL(new_file(g_cache_dir)); });
    dyn_method(C, "getExternalFilesDir(Ljava/lang/String;)Ljava/io/File;", [](JNIEnv *, jobject, const jvalue *) { return JL(new_file(g_files_dir)); });
    dyn_method(C, "getExternalCacheDir()Ljava/io/File;", [](JNIEnv *, jobject, const jvalue *) { return JL(new_file(g_cache_dir)); });
    dyn_method(C, "getObbDir()Ljava/io/File;", [](JNIEnv *, jobject, const jvalue *) { return JL(new_file(g_game_dir)); });
    dyn_method(C, "getApplicationContext()Landroid/content/Context;", [](JNIEnv *, jobject, const jvalue *) { return JL(g_activity); });
    dyn_method(C, "getSharedPreferences(Ljava/lang/String;I)Landroid/content/SharedPreferences;",
               [](JNIEnv *, jobject, const jvalue *) { return JL(dyn_new("android/content/SharedPreferences")); });
    dyn_field("android/content/Context", "MODE_PRIVATE", [](JNIEnv *, jobject) { return JI(0); });

    const char *F = "java/io/File";
    for (const char *m : {"getPath()Ljava/lang/String;", "getAbsolutePath()Ljava/lang/String;",
                          "getCanonicalPath()Ljava/lang/String;", "toString()Ljava/lang/String;"})
        dyn_method(F, m, [](JNIEnv *e, jobject self, const jvalue *) { return JS(e, file_path(self)); });
    dyn_method(F, "getParent()Ljava/lang/String;", [](JNIEnv *e, jobject self, const jvalue *) {
        std::string p = file_path(self);
        size_t cut = p.find_last_of('/');
        return JS(e, cut == std::string::npos || cut == 0 ? "/" : p.substr(0, cut).c_str());
    });
    dyn_method(F, "getName()Ljava/lang/String;", [](JNIEnv *e, jobject self, const jvalue *) {
        std::string p = file_path(self);
        size_t cut = p.find_last_of('/');
        return JS(e, cut == std::string::npos ? p.c_str() : p.c_str() + cut + 1);
    });
    dyn_method(F, "<init>(Ljava/lang/String;)V", [](JNIEnv *, jobject self, const jvalue *a) {
        ((DynObject *)self)->priv = strdup(dyn_str(a[0].l));
        return JV();
    });
    dyn_method(F, "canWrite()Z", [](JNIEnv *, jobject, const jvalue *) { return JZ(true); });
    dyn_method(F, "canRead()Z", [](JNIEnv *, jobject, const jvalue *) { return JZ(true); });
    dyn_method(F, "exists()Z", [](JNIEnv *, jobject self, const jvalue *) {
        struct stat st;
        return JZ(stat(file_path(self), &st) == 0);
    });
    dyn_method(F, "mkdirs()Z", [](JNIEnv *, jobject self, const jvalue *) { return JZ(mkdir(file_path(self), 0755) == 0); });

    const char *S = "android/content/SharedPreferences";
    dyn_method(S, "contains(Ljava/lang/String;)Z", [](JNIEnv *, jobject, const jvalue *a) {
        std::lock_guard<std::mutex> g(g_prefs_lock);
        return JZ(g_prefs.count(dyn_str(a[0].l)) != 0);
    });
    dyn_method(S, "getInt(Ljava/lang/String;I)I", [](JNIEnv *, jobject, const jvalue *a) {
        std::string v;
        return JI(prefs_get(dyn_str(a[0].l), 'i', &v) ? atoi(v.c_str()) : a[1].i);
    });
    dyn_method(S, "getFloat(Ljava/lang/String;F)F", [](JNIEnv *, jobject, const jvalue *a) {
        std::string v;
        return JF(prefs_get(dyn_str(a[0].l), 'f', &v) ? strtof(v.c_str(), NULL) : a[1].f);
    });
    dyn_method(S, "getString(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", [](JNIEnv *e, jobject, const jvalue *a) {
        std::string v;
        if (prefs_get(dyn_str(a[0].l), 's', &v))
            return JS(e, v.c_str());
        return JL(a[1].l);
    });
    dyn_method(S, "edit()Landroid/content/SharedPreferences$Editor;",
               [](JNIEnv *, jobject, const jvalue *) { return JL(dyn_new("android/content/SharedPreferences$Editor")); });

    const char *E = "android/content/SharedPreferences$Editor";
    dyn_method(E, "putInt(Ljava/lang/String;I)Landroid/content/SharedPreferences$Editor;", [](JNIEnv *, jobject self, const jvalue *a) {
        prefs_put(dyn_str(a[0].l), 'i', std::to_string(a[1].i));
        return JL(self);
    });
    dyn_method(E, "putFloat(Ljava/lang/String;F)Landroid/content/SharedPreferences$Editor;", [](JNIEnv *, jobject self, const jvalue *a) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.9g", a[1].f);
        prefs_put(dyn_str(a[0].l), 'f', buf);
        return JL(self);
    });
    dyn_method(E, "putString(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;", [](JNIEnv *, jobject self, const jvalue *a) {
        std::string v = dyn_str(a[1].l);
        for (char &c : v)
            if (c == '\n' || c == '\t')
                c = ' ';
        prefs_put(dyn_str(a[0].l), 's', v);
        return JL(self);
    });
    dyn_method(E, "remove(Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;", [](JNIEnv *, jobject self, const jvalue *a) {
        std::lock_guard<std::mutex> g(g_prefs_lock);
        g_prefs.erase(dyn_str(a[0].l));
        return JL(self);
    });
    dyn_method(E, "clear()Landroid/content/SharedPreferences$Editor;", [](JNIEnv *, jobject self, const jvalue *) {
        std::lock_guard<std::mutex> g(g_prefs_lock);
        g_prefs.clear();
        return JL(self);
    });
    auto save = [](JNIEnv *, jobject, const jvalue *) {
        std::lock_guard<std::mutex> g(g_prefs_lock);
        prefs_save();
        return JZ(true);
    };
    dyn_method(E, "commit()Z", save);
    dyn_method(E, "apply()V", save);
}

/* ---- java.util: just enough Map/Set/Iterator for the engine to walk one ---- */
struct JList { std::vector<jobject> items; size_t pos = 0; };
static jobject new_list_obj(const char *cls, const std::vector<jobject> &items)
{
    DynObject *o = (DynObject *)dyn_new(cls);
    JList *l = new JList();
    l->items = items;
    o->priv = l;
    return (jobject)o;
}
static jobject new_entry(jobject k, jobject v)
{
    DynObject *o = (DynObject *)dyn_new("java/util/Map$Entry");
    o->fields["key"] = JL(k);
    o->fields["value"] = JL(v);
    return (jobject)o;
}

static void register_util()
{
    auto same = [](JNIEnv *, jobject self, const jvalue *) { return JL(self); };
    dyn_method("java/util/Map", "entrySet()Ljava/util/Set;", same);
    dyn_method("java/util/Map", "iterator()Ljava/util/Iterator;", same);
    dyn_method("java/util/Map", "size()I", [](JNIEnv *, jobject self, const jvalue *) {
        return JI((jint)((JList *)((DynObject *)self)->priv)->items.size());
    });
    dyn_method("java/util/Map", "hasNext()Z", [](JNIEnv *, jobject self, const jvalue *) {
        JList *l = (JList *)((DynObject *)self)->priv;
        return JZ(l->pos < l->items.size());
    });
    dyn_method("java/util/Map", "next()Ljava/lang/Object;", [](JNIEnv *, jobject self, const jvalue *) {
        JList *l = (JList *)((DynObject *)self)->priv;
        return JL(l->pos < l->items.size() ? l->items[l->pos++] : NULL);
    });
    dyn_method("java/util/Map$Entry", "getKey()Ljava/lang/Object;", [](JNIEnv *, jobject self, const jvalue *) { return ((DynObject *)self)->fields["key"]; });
    dyn_method("java/util/Map$Entry", "getValue()Ljava/lang/Object;", [](JNIEnv *, jobject self, const jvalue *) { return ((DynObject *)self)->fields["value"]; });

    dyn_method("android/content/SharedPreferences", "getAll()Ljava/util/Map;", [](JNIEnv *e, jobject, const jvalue *) {
        std::lock_guard<std::mutex> g(g_prefs_lock);
        std::vector<jobject> entries;
        for (auto &kv : g_prefs) {
            if (kv.second.size() < 2)
                continue;
            const char *v = kv.second.c_str() + 2;
            jobject val;
            switch (kv.second[0]) {
            case 'i': val = new_boxed("java/lang/Integer", JI(atoi(v))); break;
            case 'f': val = new_boxed("java/lang/Float", JF(strtof(v, NULL))); break;
            default:  val = (jobject)e->NewStringUTF(v); break;
            }
            entries.push_back(new_entry((jobject)e->NewStringUTF(kv.first.c_str()), val));
        }
        return JL(new_list_obj("java/util/Map", entries));
    });
}

/* What System.loadLibrary does for a library that ships in the APK. */
static jvalue load_library(JNIEnv *env, jobject, const jvalue *a)
{
    std::string name = std::string("lib") + dyn_str(a[0].l) + ".so";
    so_module *mod = so_find_module(name.c_str());
    if (!mod) {
        trace("loadLibrary(%s)", name.c_str());
        mod = so_load_module(name.c_str(), NULL, NULL);
        if (mod) {
            auto onload = (int (*)(JavaVM *, void *))so_symbol(mod, "JNI_OnLoad");
            JavaVM *vm = NULL;
            env->GetJavaVM(&vm);
            if (onload)
                onload(vm, NULL);
        }
    }
    if (!mod)
        trace("loadLibrary(%s) failed", name.c_str());
    return JZ(mod != NULL);
}

/*
 * com.unity3d.player.ReflectionHelper: how C# scripts (AndroidJavaObject /
 * AndroidJavaClass) look members up. On Android it goes through java.lang.reflect;
 * here the "reflected" object simply wraps the JNI id, and FromReflectedMethod /
 * FromReflectedField in jni.cpp unwrap it.
 */
static void register_reflection()
{
    const char *R = "com/unity3d/player/ReflectionHelper";
    dyn_method(R, "getMethodID(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Method;",
               [](JNIEnv *e, jobject, const jvalue *a) {
        jclass c = (jclass)a[0].l;
        if (!c)
            return JL(NULL);
        jmethodID id = a[3].z ? e->GetStaticMethodID(c, dyn_str(a[1].l), dyn_str(a[2].l))
                              : e->GetMethodID(c, dyn_str(a[1].l), dyn_str(a[2].l));
        DynObject *m = (DynObject *)dyn_new("java/lang/reflect/Method");
        m->priv = (void *)id;
        return JL((jobject)m);
    });
    dyn_method(R, "getConstructorID(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/reflect/Constructor;",
               [](JNIEnv *e, jobject, const jvalue *a) {
        jclass c = (jclass)a[0].l;
        if (!c)
            return JL(NULL);
        DynObject *m = (DynObject *)dyn_new("java/lang/reflect/Constructor");
        m->priv = (void *)e->GetMethodID(c, "<init>", dyn_str(a[1].l));
        return JL((jobject)m);
    });
    dyn_method(R, "getFieldID(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Field;",
               [](JNIEnv *e, jobject, const jvalue *a) {
        jclass c = (jclass)a[0].l;
        if (!c)
            return JL(NULL);
        jfieldID id = a[3].z ? e->GetStaticFieldID(c, dyn_str(a[1].l), dyn_str(a[2].l))
                             : e->GetFieldID(c, dyn_str(a[1].l), dyn_str(a[2].l));
        DynObject *m = (DynObject *)dyn_new("java/lang/reflect/Field");
        m->priv = (void *)id;
        return JL((jobject)m);
    });
    auto proxy = [](JNIEnv *, jobject, const jvalue *) { return JL(dyn_new("com/unity3d/player/ReflectionHelper$proxy")); };
    dyn_method(R, "newProxyInstance(ILjava/lang/Class;)Ljava/lang/Object;", proxy);
    dyn_method(R, "newProxyInstance(I[Ljava/lang/Class;)Ljava/lang/Object;", proxy);
}

static void register_lang()
{
    register_reflection();
    dyn_method("*", "equals(Ljava/lang/Object;)Z", [](JNIEnv *, jobject self, const jvalue *a) {
        if (self == a[0].l)
            return JZ(true);
        if (dyn_is(self, "java/lang/String") && dyn_is(a[0].l, "java/lang/String"))
            return JZ(strcmp(dyn_str(self), dyn_str(a[0].l)) == 0);
        return JZ(false);
    });
    dyn_method("android/os/Environment", "getExternalStorageState()Ljava/lang/String;", [](JNIEnv *e, jobject, const jvalue *) { return JS(e, "mounted"); });
    dyn_field("android/os/Environment", "MEDIA_MOUNTED", [](JNIEnv *e, jobject) { return JS(e, "mounted"); });
    dyn_method("android/os/Environment", "getExternalStorageDirectory()Ljava/io/File;", [](JNIEnv *, jobject, const jvalue *) { return JL(new_file(g_game_dir)); });
    register_util();
    dyn_method("com/unity3d/player/UnityPlayer", "loadLibrary(Ljava/lang/String;)Z", load_library);
    dyn_method("*", "getClass()Ljava/lang/Class;", [](JNIEnv *, jobject self, const jvalue *) {
        return JL((jobject)dyn_class_of(self));
    });
    dyn_method("*", "getName()Ljava/lang/String;", [](JNIEnv *e, jobject self, const jvalue *) {
        /* Class.getName(): dotted form of the class path. */
        std::string n = dyn_class_name((jclass)self);
        for (char &c : n)
            if (c == '/')
                c = '.';
        return JS(e, n.c_str());
    });
    dyn_method("*", "<init>()V", [](JNIEnv *, jobject, const jvalue *) { return JV(); });
    /* Listener proxies (audio focus, runnables...): an object nobody ever calls back. */
    dyn_method("bitter/jnibridge/JNIBridge", "newInterfaceProxy(J[Ljava/lang/Class;)Ljava/lang/Object;",
               [](JNIEnv *, jobject, const jvalue *) { return JL(dyn_new("bitter/jnibridge/JNIBridge$a")); });
    dyn_method("bitter/jnibridge/JNIBridge", "disableInterfaceProxy(Ljava/lang/Object;)V",
               [](JNIEnv *, jobject, const jvalue *) { return JV(); });

    dyn_method("java/lang/Integer", "<init>(I)V", [](JNIEnv *, jobject self, const jvalue *a) { ((DynObject *)self)->fields["value"] = JI(a[0].i); return JV(); });
    dyn_method("java/lang/Integer", "valueOf(I)Ljava/lang/Integer;", [](JNIEnv *, jobject, const jvalue *a) { return JL(new_boxed("java/lang/Integer", JI(a[0].i))); });
    dyn_method("*", "intValue()I", [](JNIEnv *, jobject self, const jvalue *) { return JI(boxed(self).i); });
    dyn_method("*", "booleanValue()Z", [](JNIEnv *, jobject self, const jvalue *) { return JZ(boxed(self).z); });
    dyn_method("*", "floatValue()F", [](JNIEnv *, jobject self, const jvalue *) { return JF(boxed(self).f); });
    dyn_method("*", "longValue()J", [](JNIEnv *, jobject self, const jvalue *) { return JJ(boxed(self).j); });
}

static void register_system()
{
    auto str_field = [](const char *cls, const char *name, const char *val) {
        std::string v = val;
        dyn_field(cls, name, [v](JNIEnv *e, jobject) { return JS(e, v.c_str()); });
    };
    auto int_field = [](const char *cls, const char *name, int val) {
        dyn_field(cls, name, [val](JNIEnv *, jobject) { return JI(val); });
    };
    /* A plain 2013-era Adreno phone, which is what this build was tuned for. */
    const char *B = "android/os/Build";
    str_field(B, "ID", "KOT49H");
    str_field(B, "MANUFACTURER", "LGE");
    str_field(B, "MODEL", "Nexus 5");
    str_field(B, "BRAND", "google");
    str_field(B, "DEVICE", "hammerhead");
    str_field(B, "PRODUCT", "hammerhead");
    str_field(B, "HARDWARE", "hammerhead");
    str_field(B, "BOARD", "hammerhead");
    str_field(B, "DISPLAY", "KOT49H");
    str_field(B, "FINGERPRINT", "google/hammerhead/hammerhead:4.4.4/KTU84P/1227136:user/release-keys");
    str_field(B, "CPU_ABI", "armeabi-v7a");
    str_field(B, "CPU_ABI2", "armeabi");
    str_field(B, "SERIAL", "unknown");
    str_field(B, "TAGS", "release-keys");
    str_field(B, "TYPE", "user");
    const char *V = "android/os/Build$VERSION";
    int_field(V, "SDK_INT", 19);
    str_field(V, "RELEASE", "4.4.4");
    str_field(V, "INCREMENTAL", "1227136");
    str_field(V, "CODENAME", "REL");

    const char *C = "android/content/Context";
    for (const char *svc : {"WINDOW_SERVICE:window", "AUDIO_SERVICE:audio", "CONNECTIVITY_SERVICE:connectivity",
                            "VIBRATOR_SERVICE:vibrator", "TELEPHONY_SERVICE:phone", "DISPLAY_SERVICE:display",
                            "ACTIVITY_SERVICE:activity", "INPUT_METHOD_SERVICE:input_method",
                            "MEDIA_ROUTER_SERVICE:media_router", "WIFI_SERVICE:wifi", "SENSOR_SERVICE:sensor"}) {
        std::string t = svc;
        size_t c = t.find(':');
        str_field(C, t.substr(0, c).c_str(), t.substr(c + 1).c_str());
    }
    int_field(C, "BIND_AUTO_CREATE", 1);
    int_field("android/content/pm/PackageManager", "GET_META_DATA", 128);
    int_field("android/content/pm/PackageManager", "PERMISSION_GRANTED", 0);

    dyn_method("*", "getSystemService(Ljava/lang/String;)Ljava/lang/Object;", [](JNIEnv *, jobject, const jvalue *a) {
        std::string n = dyn_str(a[0].l);
        const char *cls = NULL;
        if (n == "window") cls = "android/view/WindowManager";
        else if (n == "audio") cls = "android/media/AudioManager";
        else if (n == "connectivity") cls = "android/net/ConnectivityManager";
        else if (n == "display") cls = "android/hardware/display/DisplayManager";
        else if (n == "vibrator") cls = "android/os/Vibrator";
        else if (n == "phone") cls = "android/telephony/TelephonyManager";
        static std::map<std::string, jobject> services;
        if (!cls)
            return JL(NULL);
        jobject &o = services[n];
        if (!o)
            o = dyn_new(cls);
        return JL(o);
    });
    dyn_method("*", "getWindowManager()Landroid/view/WindowManager;", [](JNIEnv *, jobject, const jvalue *) {
        static jobject wm = dyn_new("android/view/WindowManager");
        return JL(wm);
    });
    dyn_method("*", "getDefaultDisplay()Landroid/view/Display;", [](JNIEnv *, jobject, const jvalue *) {
        static jobject d = dyn_new("android/view/Display");
        return JL(d);
    });
    const char *D = "android/view/Display";
    auto fill = [](JNIEnv *, jobject, const jvalue *a) {
        DynObject *m = (DynObject *)a[0].l;
        if (m) {
            m->fields["widthPixels"] = JI(g_cfg.width);
            m->fields["heightPixels"] = JI(g_cfg.height);
            m->fields["densityDpi"] = JI(320);
            m->fields["density"] = JF(2.0f);
            m->fields["scaledDensity"] = JF(2.0f);
            m->fields["xdpi"] = JF(320.0f);
            m->fields["ydpi"] = JF(320.0f);
        }
        return JV();
    };
    dyn_method(D, "getMetrics(Landroid/util/DisplayMetrics;)V", fill);
    dyn_method(D, "getRealMetrics(Landroid/util/DisplayMetrics;)V", fill);
    dyn_method(D, "getWidth()I", [](JNIEnv *, jobject, const jvalue *) { return JI(g_cfg.width); });
    dyn_method(D, "getHeight()I", [](JNIEnv *, jobject, const jvalue *) { return JI(g_cfg.height); });
    dyn_method(D, "getRotation()I", [](JNIEnv *, jobject, const jvalue *) { return JI(1); });       /* ROTATION_90: landscape */
    dyn_method(D, "getOrientation()I", [](JNIEnv *, jobject, const jvalue *) { return JI(1); });
    dyn_method(D, "getRefreshRate()F", [](JNIEnv *, jobject, const jvalue *) { return JF(60.0f); });
    dyn_method(D, "getDisplayId()I", [](JNIEnv *, jobject, const jvalue *) { return JI(0); });

    dyn_method("android/hardware/display/DisplayManager", "getDisplay(I)Landroid/view/Display;", [](JNIEnv *, jobject, const jvalue *) {
        static jobject d = dyn_new("android/view/Display");
        return JL(d);
    });
    dyn_method("android/content/pm/PackageManager", "getApplicationInfo(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;",
               [](JNIEnv *e, jobject, const jvalue *) {
        static DynObject *ai = NULL;
        if (!ai) {
            ai = (DynObject *)dyn_new("android/content/pm/ApplicationInfo");
            std::string libdir = g_game_dir + "/lib/armeabi-v7a";
            ai->fields["nativeLibraryDir"] = JS(e, libdir.c_str());
            ai->fields["sourceDir"] = JS(e, g_apk.c_str());
            ai->fields["publicSourceDir"] = JS(e, g_apk.c_str());
            ai->fields["dataDir"] = JS(e, g_game_dir.c_str());
            ai->fields["packageName"] = JS(e, kPackage);
            ai->fields["metaData"] = JL(dyn_new("android/os/Bundle"));
            ai->fields["flags"] = JI(0);
            ai->fields["targetSdkVersion"] = JI(27);
        }
        return JL((jobject)ai);
    });
    dyn_method("*", "getApplicationInfo()Landroid/content/pm/ApplicationInfo;", [](JNIEnv *e, jobject self, const jvalue *a) {
        static jobject pm = dyn_new("android/content/pm/PackageManager");
        jclass c = e->GetObjectClass(pm);
        jmethodID m = e->GetMethodID(c, "getApplicationInfo", "(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;");
        return JL(e->CallObjectMethod(pm, m, (jobject)NULL, 0));
    });
    dyn_method("java/lang/System", "load(Ljava/lang/String;)V", [](JNIEnv *, jobject, const jvalue *) { return JV(); });
    dyn_method("android/os/Bundle", "getString(Ljava/lang/String;)Ljava/lang/String;", [](JNIEnv *, jobject, const jvalue *) { return JL(NULL); });
    dyn_method("android/os/Bundle", "getBoolean(Ljava/lang/String;)Z", [](JNIEnv *, jobject, const jvalue *) { return JZ(false); });
    dyn_method("android/os/Bundle", "getInt(Ljava/lang/String;)I", [](JNIEnv *, jobject, const jvalue *) { return JI(0); });
    dyn_method("android/os/Bundle", "containsKey(Ljava/lang/String;)Z", [](JNIEnv *, jobject, const jvalue *) { return JZ(false); });
    dyn_method("java/lang/ClassLoader", "findLibrary(Ljava/lang/String;)Ljava/lang/String;", [](JNIEnv *e, jobject, const jvalue *a) {
        std::string p = g_game_dir + "/lib/armeabi-v7a/lib" + dyn_str(a[0].l) + ".so";
        struct stat st;
        if (stat(p.c_str(), &st) != 0)
            return JL(NULL);
        return JS(e, p.c_str());
    });
    dyn_method("*", "getPackageManager()Landroid/content/pm/PackageManager;", [](JNIEnv *, jobject, const jvalue *) {
        static jobject pm = dyn_new("android/content/pm/PackageManager");
        return JL(pm);
    });
    const char *P = "android/content/pm/PackageManager";
    dyn_method(P, "hasSystemFeature(Ljava/lang/String;)Z", [](JNIEnv *, jobject, const jvalue *a) {
        return JZ(strstr(dyn_str(a[0].l), "touchscreen") != NULL);
    });
    dyn_method(P, "checkPermission(Ljava/lang/String;Ljava/lang/String;)I", [](JNIEnv *, jobject, const jvalue *) { return JI(0); });
    dyn_method("*", "checkCallingOrSelfPermission(Ljava/lang/String;)I", [](JNIEnv *, jobject, const jvalue *) { return JI(0); });

    /* No services, no network: binds fail, intents go nowhere. */
    dyn_method("*", "bindService(Landroid/content/Intent;Landroid/content/ServiceConnection;I)Z", [](JNIEnv *, jobject, const jvalue *) { return JZ(false); });
    dyn_method("*", "unbindService(Landroid/content/ServiceConnection;)V", [](JNIEnv *, jobject, const jvalue *) { return JV(); });
    dyn_method("android/content/Intent", "<init>(Ljava/lang/String;)V", [](JNIEnv *, jobject, const jvalue *) { return JV(); });
    dyn_method("android/content/Intent", "setPackage(Ljava/lang/String;)Landroid/content/Intent;", [](JNIEnv *, jobject self, const jvalue *) { return JL(self); });
    dyn_method("android/net/Uri", "decode(Ljava/lang/String;)Ljava/lang/String;", [](JNIEnv *, jobject, const jvalue *a) { return JL(a[0].l); });
    dyn_method("android/net/Uri", "encode(Ljava/lang/String;)Ljava/lang/String;", [](JNIEnv *, jobject, const jvalue *a) { return JL(a[0].l); });
    dyn_method("*", "getContentResolver()Landroid/content/ContentResolver;", [](JNIEnv *, jobject, const jvalue *) {
        static jobject cr = dyn_new("android/content/ContentResolver");
        return JL(cr);
    });
    dyn_field("android/provider/Settings$System", "SCREEN_OFF_TIMEOUT", [](JNIEnv *e, jobject) { return JS(e, "screen_off_timeout"); });
    dyn_method("android/provider/Settings$System", "getInt(Landroid/content/ContentResolver;Ljava/lang/String;I)I",
               [](JNIEnv *, jobject, const jvalue *a) { return JI(a[2].i); });
    dyn_method("android/provider/Settings$Secure", "getString(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",
               [](JNIEnv *e, jobject, const jvalue *) { return JS(e, "5ad0a6ca11b0a4d1"); });
    dyn_field("android/provider/Settings$Secure", "ANDROID_ID", [](JNIEnv *e, jobject) { return JS(e, "android_id"); });
    dyn_method("android/telephony/TelephonyManager", "getDeviceId()Ljava/lang/String;", [](JNIEnv *, jobject, const jvalue *) { return JL(NULL); });
    dyn_field("android/Manifest$permission", "READ_PHONE_STATE", [](JNIEnv *e, jobject) { return JS(e, "android.permission.READ_PHONE_STATE"); });
    dyn_field("android/content/pm/PackageManager", "FEATURE_AUDIO_LOW_LATENCY", [](JNIEnv *e, jobject) { return JS(e, "android.hardware.audio.low_latency"); });
    dyn_field("android/media/MediaRouter", "ROUTE_TYPE_LIVE_VIDEO", [](JNIEnv *, jobject) { return JI(2); });
    /* 44100 keeps FMOD's mixer at the rate the Android wrapper's AudioTrack plays. */
    dyn_method("android/media/AudioTrack", "getNativeOutputSampleRate(I)I", [](JNIEnv *, jobject, const jvalue *) { return JI(44100); });
    dyn_method("android/media/AudioManager", "getProperty(Ljava/lang/String;)Ljava/lang/String;", [](JNIEnv *e, jobject, const jvalue *a) {
        return JS(e, strstr(dyn_str(a[0].l), "SAMPLE_RATE") ? "44100" : "256");
    });
    dyn_method("android/media/AudioManager", "requestAudioFocus", [](JNIEnv *, jobject, const jvalue *) { return JI(1); });
    dyn_method("android/media/AudioManager", "abandonAudioFocus", [](JNIEnv *, jobject, const jvalue *) { return JI(1); });
    dyn_method("android/media/AudioManager", "isMusicActive()Z", [](JNIEnv *, jobject, const jvalue *) { return JZ(false); });
    dyn_method("android/view/InputDevice", "getDeviceIds()[I", [](JNIEnv *e, jobject, const jvalue *) { return JL((jobject)e->NewIntArray(0)); });
    dyn_method("java/util/HashMap", "<init>(I)V", [](JNIEnv *, jobject, const jvalue *) { return JV(); });
    dyn_method("java/util/HashMap", "put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", [](JNIEnv *, jobject, const jvalue *) { return JL(NULL); });
    dyn_method("android/app/ApplicationErrorReport", "getErrorReportReceiver", [](JNIEnv *, jobject, const jvalue *) { return JL(NULL); });
    dyn_method("java/lang/Class", "getClassLoader()Ljava/lang/ClassLoader;", [](JNIEnv *, jobject, const jvalue *) {
        static jobject cl = dyn_new("java/lang/ClassLoader");
        return JL(cl);
    });

    {
        /* android.content.pm.ActivityInfo orientation constants (the engine compares them). */
        const char *AI = "android/content/pm/ActivityInfo";
        struct { const char *n; int v; } orient[] = {
            {"SCREEN_ORIENTATION_LANDSCAPE", 0}, {"SCREEN_ORIENTATION_PORTRAIT", 1},
            {"SCREEN_ORIENTATION_UNSPECIFIED", -1}, {"SCREEN_ORIENTATION_SENSOR", 4},
            {"SCREEN_ORIENTATION_SENSOR_LANDSCAPE", 6}, {"SCREEN_ORIENTATION_SENSOR_PORTRAIT", 7},
            {"SCREEN_ORIENTATION_REVERSE_LANDSCAPE", 8}, {"SCREEN_ORIENTATION_REVERSE_PORTRAIT", 9},
            {"SCREEN_ORIENTATION_FULL_SENSOR", 10},
        };
        for (auto &o : orient) {
            int v = o.v;
            dyn_field(AI, o.n, [v](JNIEnv *, jobject) { return JI(v); });
        }
        dyn_method("android/app/Activity", "getRequestedOrientation()I", [](JNIEnv *, jobject, const jvalue *) { return JI(6); });
        dyn_method("android/app/Activity", "setRequestedOrientation(I)V", [](JNIEnv *, jobject, const jvalue *) { return JV(); });
    }

    const char *U = "com/unity3d/player/UnityPlayer";
    for (const char *m : {"setFullscreen(Z)V", "executeGLThreadJobs()V", "disableLogger()V", "startActivityIndicator()V",
                          "stopActivityIndicator()V", "hideSoftInput()V", "addPhoneCallListener()V",
                          "Location_StopUpdatingLocation()V"})
        dyn_method(U, m, [](JNIEnv *, jobject, const jvalue *) { return JV(); });
    dyn_method(U, "getSplashMode()I", [](JNIEnv *, jobject, const jvalue *) { return JI(1); });
    dyn_method(U, "getNumCameras()I", [](JNIEnv *, jobject, const jvalue *) { return JI(0); });
    dyn_method(U, "Location_IsServiceEnabledByUser()Z", [](JNIEnv *, jobject, const jvalue *) { return JZ(false); });
    dyn_method(U, "installPresentationDisplay(I)Z", [](JNIEnv *, jobject, const jvalue *) { return JZ(false); });
}

/*
 * The game's own Java plugins, as its C# scripts see them.
 *
 * Madfinger.Plugins.AndroidPermissions gates the start-up on the runtime
 * permission dialog; here every permission is simply granted. The Google Play
 * and crash-reporter plugins are left to the "unhandled call returns 0/null"
 * default, which reads as "not connected / not available".
 */
static int g_perm_count = 4;
static jintArray granted_array(JNIEnv *e, int n)
{
    jintArray arr = e->NewIntArray(n);   /* zero-filled: PERMISSION_GRANTED == 0 */
    return arr;
}
static void register_game_plugins()
{
    dyn_method("java/lang/Class", "forName(Ljava/lang/String;)Ljava/lang/Object;", [](JNIEnv *, jobject, const jvalue *a) {
        std::string n = dyn_str(a[0].l);
        for (char &c : n)
            if (c == '.')
                c = '/';
        return JL((jobject)dyn_class(n.c_str()));
    });
    dyn_method("java/lang/Class", "forName(Ljava/lang/String;)Ljava/lang/Class;", [](JNIEnv *, jobject, const jvalue *a) {
        std::string n = dyn_str(a[0].l);
        for (char &c : n)
            if (c == '.')
                c = '/';
        return JL((jobject)dyn_class(n.c_str()));
    });
    dyn_field("com/unity3d/player/UnityPlayer", "currentActivity", [](JNIEnv *, jobject) { return JL(g_activity); });

    dyn_method("android/content/pm/PackageManager", "getPackageInfo(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;",
               [](JNIEnv *e, jobject, const jvalue *) {
        static DynObject *pi = NULL;
        if (!pi) {
            int ver = 170200014;
            const char *base = strrchr(g_obb.c_str(), '/');
            base = base ? base + 1 : g_obb.c_str();
            if (strncmp(base, "main.", 5) == 0 && atoi(base + 5) > 0)
                ver = atoi(base + 5);
            pi = (DynObject *)dyn_new("android/content/pm/PackageInfo");
            pi->fields["versionCode"] = JI(ver);
            pi->fields["versionName"] = JS(e, "1.7.0");
            pi->fields["packageName"] = JS(e, kPackage);
        }
        return JL((jobject)pi);
    });
    dyn_method("android/content/pm/PackageManager", "getInstallerPackageName(Ljava/lang/String;)Ljava/lang/String;",
               [](JNIEnv *e, jobject, const jvalue *) { return JS(e, "com.android.vending"); });

    /*
     * Google Play Games: not available. The plugin answers asynchronously with
     * UnitySendMessage to the object the game named in UnityCallbacks.init(); the
     * game's start-up waits for those answers, so "no" has to actually be said.
     */
    static std::string gpg_object;
    const char *G = "com/madfingergames/googleplaygames/";
    dyn_method((std::string(G) + "UnityCallbacks").c_str(), "init(Ljava/lang/String;)V", [](JNIEnv *, jobject, const jvalue *a) {
        gpg_object = dyn_str(a[0].l);
        return JV();
    });
    dyn_method((std::string(G) + "API").c_str(), "isConnected", [](JNIEnv *, jobject, const jvalue *) { return JZ(false); });
    dyn_method((std::string(G) + "API").c_str(), "isConnecting", [](JNIEnv *, jobject, const jvalue *) { return JZ(false); });
    dyn_method((std::string(G) + "API").c_str(), "isPlayServicesAvailable", [](JNIEnv *, jobject, const jvalue *) { return JZ(false); });
    dyn_method((std::string(G) + "API").c_str(), "resolvePlayServicesAvailabilityError", [](JNIEnv *, jobject, const jvalue *) {
        send_message(gpg_object, "_OnResolvePlayServicesAvailability", "false");
        return JV();
    });
    dyn_method((std::string(G) + "API").c_str(), "connect", [](JNIEnv *, jobject, const jvalue *) {
        send_message(gpg_object, "_OnConnect", "false");
        return JV();
    });
    dyn_method((std::string(G) + "API").c_str(), "disconnect", [](JNIEnv *, jobject, const jvalue *) { return JV(); });

    const char *P = "com/madfingergames/androidpermissions/PermissionsActivity";
    dyn_method(P, "init(Ljava/lang/String;)V", [](JNIEnv *, jobject, const jvalue *) { return JV(); });
    dyn_method(P, "checkPermissions(I[Ljava/lang/String;)[I", [](JNIEnv *e, jobject, const jvalue *a) {
        int n = a[1].l ? e->GetArrayLength((jarray)a[1].l) : 0;
        return JL((jobject)granted_array(e, n));
    });
    dyn_method(P, "requestPermissions(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;)I", [](JNIEnv *e, jobject, const jvalue *a) {
        g_perm_count = a[2].l ? e->GetArrayLength((jarray)a[2].l) : 0;
        return JI(1);
    });
    dyn_method(P, "isRequestReady(I)Z", [](JNIEnv *, jobject, const jvalue *) { return JZ(true); });
    dyn_method(P, "getGrantedResults(I)[I", [](JNIEnv *e, jobject, const jvalue *) { return JL((jobject)granted_array(e, g_perm_count)); });
    dyn_method(P, "showSettingsDialog", [](JNIEnv *, jobject, const jvalue *) { return JI(1); });
    dyn_method(P, "showRetryDialog", [](JNIEnv *, jobject, const jvalue *) { return JI(1); });
}

void unity_java_init(JNIEnv *env, const UnityJavaConfig &cfg)
{
    g_cfg = cfg;
    g_game_dir = cfg.game_dir;
    g_apk = cfg.apk_path;
    g_obb = cfg.obb_path ? cfg.obb_path : "";
    g_files_dir = g_game_dir + "/files";
    g_cache_dir = g_game_dir + "/cache";
    mkdir(g_files_dir.c_str(), 0755);
    mkdir(g_cache_dir.c_str(), 0755);
    g_prefs_path = g_files_dir + "/playerprefs.txt";
    prefs_load();

    g_player   = dyn_new("com/unity3d/player/UnityPlayer");
    g_activity = dyn_new("com/madfingergames/unityplayer/MFUnityPlayerActivity");
    g_surface  = dyn_new("android/view/Surface");

    register_context();
    register_lang();
    register_system();
    register_game_plugins();
    fmod_audio_register(env);
    unity_www_register(env);
}
