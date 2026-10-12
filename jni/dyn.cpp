/* See dyn.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <set>

#include "platform.h"
#include "dyn.h"

namespace {

std::recursive_mutex g_lock;

struct ClassInfo {
    Class clazz;
    std::string path;
    std::map<std::string, ManagedMethod *> methods;   /* "s|name|sig" */
    std::map<std::string, FieldId *> fields;
    std::map<std::string, void *> natives;            /* name -> fnPtr (last registered wins) */
};

std::map<std::string, ClassInfo *> &classes()
{
    static std::map<std::string, ClassInfo *> m;
    return m;
}
std::map<const Class *, ClassInfo *> &by_class()
{
    static std::map<const Class *, ClassInfo *> m;
    return m;
}
std::map<std::string, DynFn> &handlers()
{
    static std::map<std::string, DynFn> m;
    return m;
}
std::map<std::string, DynGet> &getters()
{
    static std::map<std::string, DynGet> m;
    return m;
}

/* Info for any class: dynamic ones own it, static ones get a side record. */
ClassInfo *info_for(Class *clz)
{
    auto it = by_class().find(clz);
    if (it != by_class().end())
        return it->second;
    ClassInfo *ci = new ClassInfo();
    ci->path = clz->classpath ? clz->classpath : "?";
    /* ci->clazz unused: the static Class stays the identity. */
    by_class()[clz] = ci;
    classes()[ci->path] = ci;
    return ci;
}

int count_args(const char *sig)
{
    int n = 0;
    const char *p = sig;
    if (*p == '(')
        p++;
    while (*p && *p != ')') {
        while (*p == '[')
            p++;
        if (*p == 'L') {
            while (*p && *p != ';')
                p++;
        }
        if (*p)
            p++;
        n++;
    }
    return n;
}

const DynFn *find_handler(const DynMethod *d)
{
    auto &h = handlers();
    const std::string keys[4] = {
        d->cls + "." + d->name + d->sig,
        d->cls + "." + d->name,
        "*." + d->name + d->sig,
        "*." + d->name,
    };
    for (const auto &k : keys) {
        auto it = h.find(k);
        if (it != h.end())
            return &it->second;
    }
    /*
     * Same name and arguments, different return type. C# scripts look methods up
     * through ReflectionHelper with a descriptor they compose themselves, and a
     * call typed as AndroidJavaObject asks for "...)Ljava/lang/Object;" whatever
     * the method really returns; Java's reflection matches it anyway.
     */
    size_t close = d->sig.find(')');
    if (close != std::string::npos) {
        const std::string loose[2] = {
            d->cls + "." + d->name + d->sig.substr(0, close + 1),
            "*." + d->name + d->sig.substr(0, close + 1),
        };
        for (const auto &k : loose) {
            auto it = h.lower_bound(k);
            if (it != h.end() && it->first.compare(0, k.size(), k) == 0)
                return &it->second;
        }
    }
    return nullptr;
}

bool trace_jni()
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SHADOWGUN_JNI_TRACE");
        on = (e && *e && *e != '0') ? 1 : 0;
    }
    return on == 1;
}

jvalue invoke(JNIEnv *env, jobject self, const ManagedMethod *m, const jvalue *args)
{
    DynMethod *d = (DynMethod *)m->dyn;
    const DynFn *fn = find_handler(d);
    d->calls++;
    if (!fn) {
        if (d->calls == 1)
            fprintf(stderr, "JNI unhandled call: %s.%s%s\n", d->cls.c_str(), d->name.c_str(), d->sig.c_str());
        return JV();
    }
    if (trace_jni() && d->calls <= 3)
        fprintf(stderr, "JNI call: %s.%s%s\n", d->cls.c_str(), d->name.c_str(), d->sig.c_str());
    return (*fn)(env, self, args);
}

} // namespace

bool dyn_trace_jni() { return trace_jni(); }

/*
 * The slice of the Java class hierarchy the engine asks about. Unity checks
 * IsInstanceOf(event, InputEvent) before it looks at a MotionEvent at all, so an
 * exact-class comparison silently drops every touch.
 */
static const char *parent_of(const char *cls)
{
    static const std::map<std::string, const char *> parents = {
        {"android/view/MotionEvent", "android/view/InputEvent"},
        {"android/view/KeyEvent", "android/view/InputEvent"},
        {"com/madfingergames/unityplayer/MFUnityPlayerActivity", "com/unity3d/player/UnityPlayerActivity"},
        {"com/unity3d/player/UnityPlayerActivity", "android/app/Activity"},
        {"android/app/Activity", "android/view/ContextThemeWrapper"},
        {"android/view/ContextThemeWrapper", "android/content/ContextWrapper"},
        {"android/content/ContextWrapper", "android/content/Context"},
        {"com/unity3d/player/UnityPlayer", "android/widget/FrameLayout"},
        {"android/widget/FrameLayout", "android/view/ViewGroup"},
        {"android/view/ViewGroup", "android/view/View"},
        {"java/lang/Integer", "java/lang/Number"},
        {"java/lang/Float", "java/lang/Number"},
        {"java/lang/Long", "java/lang/Number"},
        {"java/lang/Double", "java/lang/Number"},
        {"android/content/SharedPreferences$Editor", "java/lang/Object"},
    };
    auto it = parents.find(cls);
    if (it != parents.end())
        return it->second;
    if (strcmp(cls, "java/lang/Object") == 0)
        return NULL;
    return "java/lang/Object";
}

const char *dyn_superclass_path(const char *cls) { return parent_of(cls); }

bool dyn_is_subclass(const char *cls, const char *ancestor)
{
    for (int depth = 0; cls && depth < 16; depth++) {
        if (strcmp(cls, ancestor) == 0)
            return true;
        cls = parent_of(cls);
    }
    return false;
}

jvalue JS(JNIEnv *env, const char *utf)
{
    return JL((jobject)env->NewStringUTF(utf ? utf : ""));
}

const char *dyn_str(jobject jstr)
{
    if (!jstr)
        return "";
    String *s = (String *)jstr;
    return s->str ? s->str : "";
}

void dyn_method(const char *cls, const char *key, DynFn fn)
{
    std::lock_guard<std::recursive_mutex> g(g_lock);
    handlers()[std::string(cls) + "." + key] = std::move(fn);
}

void dyn_field(const char *cls, const char *name, DynGet fn)
{
    std::lock_guard<std::recursive_mutex> g(g_lock);
    getters()[std::string(cls) + "." + name] = std::move(fn);
}

Class *dyn_class(const char *classpath)
{
    std::lock_guard<std::recursive_mutex> g(g_lock);
    auto it = classes().find(classpath);
    if (it != classes().end()) {
        /* A side record of a static class has no Class of its own. */
        for (auto &kv : by_class())
            if (kv.second == it->second)
                return (Class *)kv.first;
    }
    for (const Class *c : ClassRegistry::get_class_registry())
        if (strcmp(c->classpath, classpath) == 0)
            return (Class *)c;

    ClassInfo *ci = new ClassInfo();
    ci->path = classpath;
    ci->clazz.classpath = strdup(classpath);
    ci->clazz.classname = ci->clazz.classpath;
    ci->clazz.managed_methods = NULL;
    ci->clazz.native_methods = NULL;
    ci->clazz.fields = NULL;
    ci->clazz.instance_size = 0;
    classes()[classpath] = ci;
    by_class()[&ci->clazz] = ci;
    if (trace_jni())
        fprintf(stderr, "JNI class: %s\n", classpath);
    return &ci->clazz;
}

bool dyn_is_dynamic_class(const Class *clz)
{
    std::lock_guard<std::recursive_mutex> g(g_lock);
    auto it = by_class().find(clz);
    return it != by_class().end() && &it->second->clazz == clz;
}

jobject dyn_new(const char *classpath)
{
    DynObject *o = new DynObject();
    o->cls = dyn_class(classpath);
    return (jobject)o;
}

/*
 * The class of any reference, including a jclass. A Class struct is not an
 * Object - it has no vtable, its first word is the class-path string - so
 * calling _getClass() on one jumps into text. C# scripts do pass classes where
 * objects are expected (AndroidJavaClass is an AndroidJavaObject).
 */
Class *dyn_class_of(jobject obj)
{
    if (!obj)
        return NULL;
    {
        std::lock_guard<std::recursive_mutex> g(g_lock);
        bool is_class = by_class().count((const Class *)obj) != 0;
        if (!is_class)
            for (const Class *c : ClassRegistry::get_class_registry())
                if ((const void *)c == (const void *)obj) {
                    is_class = true;
                    break;
                }
        if (is_class)
            return dyn_class("java/lang/Class");
    }
    return ((Object *)obj)->_getClass();
}

bool dyn_is(jobject obj, const char *classpath)
{
    if (!obj)
        return false;
    Class *c = dyn_class_of(obj);
    return c && c->classpath && strcmp(c->classpath, classpath) == 0;
}

const char *dyn_class_name(jclass cls)
{
    Class *c = (Class *)cls;
    return (c && c->classpath) ? c->classpath : "?";
}

ManagedMethod *dyn_get_method(Class *clz, const char *name, const char *sig, bool is_static)
{
    std::lock_guard<std::recursive_mutex> g(g_lock);
    ClassInfo *ci = info_for(clz);
    std::string key = std::string(is_static ? "s|" : "i|") + name + "|" + sig;
    auto it = ci->methods.find(key);
    if (it != ci->methods.end())
        return it->second;

    DynMethod *d = new DynMethod();
    d->cls = ci->path;
    d->name = name;
    d->sig = sig;
    d->is_static = is_static;
    d->nargs = count_args(sig);

    ManagedMethod *m = new ManagedMethod{
        .clazz = clz,
        .name = strdup(name),
        .signature = strdup(sig),
        .addr_variadic = NULL,
        .addr_array = NULL,
        .is_static_method = is_static,
        .takes_class = false,
        .dyn = d,
    };
    ci->methods[key] = m;
    if (!find_handler(d) && trace_jni())
        fprintf(stderr, "JNI lookup without handler: %s.%s%s%s\n", d->cls.c_str(), name, sig,
                is_static ? " (static)" : "");
    else if (trace_jni())
        fprintf(stderr, "JNI lookup: %s.%s%s\n", d->cls.c_str(), name, sig);
    return m;
}

FieldId *dyn_get_field(Class *clz, const char *name, const char *sig, bool is_static)
{
    std::lock_guard<std::recursive_mutex> g(g_lock);
    ClassInfo *ci = info_for(clz);
    std::string key = std::string(is_static ? "s|" : "i|") + name;
    auto it = ci->fields.find(key);
    if (it != ci->fields.end())
        return it->second;

    DynField *d = new DynField{ci->path, name, sig ? sig : "", is_static};
    FieldId *f = new FieldId{};
    f->clazz = clz;
    f->name = strdup(name);
    f->signature = strdup(sig ? sig : "");
    f->offset = 0;
    f->is_static = is_static ? 1 : 0;
    f->dyn = d;
    ci->fields[key] = f;
    return f;
}

jvalue dyn_call_a(JNIEnv *env, jobject self, const ManagedMethod *m, const jvalue *args)
{
    return invoke(env, self, m, args);
}

/*
 * Arguments are read off the caller's va_list by hand, by descriptor. The list
 * was built by the game's compiler; on ARM EABI it is a bare pointer into the
 * argument area, 4-byte slots with 64-bit values aligned to 8.
 */
jvalue dyn_call_v(JNIEnv *env, jobject self, const ManagedMethod *m, va_list va)
{
    DynMethod *d = (DynMethod *)m->dyn;
    jvalue args[32];
    memset(args, 0, sizeof(args));
    uintptr_t ap = *(uintptr_t *)&va;
    const char *p = d->sig.c_str();
    int n = 0;
    if (*p == '(')
        p++;
    while (*p && *p != ')' && n < 32) {
        char c = *p;
        if (c == '[') {
            while (*p == '[')
                p++;
            if (*p == 'L')
                while (*p && *p != ';')
                    p++;
            c = 'L';
        } else if (c == 'L') {
            while (*p && *p != ';')
                p++;
        }
        if (*p)
            p++;
        switch (c) {
        case 'J':
            ap = (ap + 7) & ~(uintptr_t)7;
            memcpy(&args[n].j, (void *)ap, 8);
            ap += 8;
            break;
        case 'D':
            ap = (ap + 7) & ~(uintptr_t)7;
            memcpy(&args[n].d, (void *)ap, 8);
            ap += 8;
            break;
        case 'F': {
            double dv;
            ap = (ap + 7) & ~(uintptr_t)7;
            memcpy(&dv, (void *)ap, 8);
            ap += 8;
            args[n].f = (float)dv;
            break;
        }
        case 'Z':
        case 'B':
            args[n].i = *(int32_t *)ap;
            args[n].b = (jbyte)args[n].i;
            ap += 4;
            break;
        default:
            args[n].i = *(int32_t *)ap;
            ap += 4;
            break;
        }
        n++;
    }
    return invoke(env, self, m, args);
}

jvalue dyn_field_get(JNIEnv *env, jobject self, const FieldId *f)
{
    DynField *d = (DynField *)f->dyn;
    {
        std::lock_guard<std::recursive_mutex> g(g_lock);
        auto &gt = getters();
        auto it = gt.find(d->cls + "." + d->name);
        if (it == gt.end())
            it = gt.find("*." + d->name);
        if (it != gt.end()) {
            DynGet fn = it->second;
            return fn(env, self);
        }
    }
    if (!d->is_static && self && dyn_is_dynamic_class(dyn_class_of(self)) && strcmp(dyn_class_of(self)->classpath, "java/lang/Class") != 0) {
        DynObject *o = (DynObject *)self;
        auto it = o->fields.find(d->name);
        if (it != o->fields.end())
            return it->second;
    }
    static std::set<std::string> said;
    std::lock_guard<std::recursive_mutex> g(g_lock);
    if (said.insert(d->cls + "." + d->name).second)
        fprintf(stderr, "JNI unhandled field: %s.%s %s%s\n", d->cls.c_str(), d->name.c_str(), d->sig.c_str(),
                d->is_static ? " (static)" : "");
    return JV();
}

void dyn_field_set(JNIEnv *env, jobject self, const FieldId *f, jvalue v)
{
    DynField *d = (DynField *)f->dyn;
    if (!d->is_static && self && dyn_is_dynamic_class(dyn_class_of(self)) && strcmp(dyn_class_of(self)->classpath, "java/lang/Class") != 0)
        ((DynObject *)self)->fields[d->name] = v;
}

void dyn_register_natives(Class *clz, const JNINativeMethod *methods, int n)
{
    std::lock_guard<std::recursive_mutex> g(g_lock);
    ClassInfo *ci = info_for(clz);
    for (int i = 0; i < n; i++) {
        ci->natives[methods[i].name] = methods[i].fnPtr;
        if (trace_jni())
            fprintf(stderr, "JNI native: %s.%s%s -> %p\n", ci->path.c_str(), methods[i].name,
                    methods[i].signature, methods[i].fnPtr);
    }
}

void *dyn_native(const char *classpath, const char *name)
{
    std::lock_guard<std::recursive_mutex> g(g_lock);
    auto it = classes().find(classpath);
    if (it == classes().end())
        return NULL;
    auto jt = it->second->natives.find(name);
    return jt == it->second->natives.end() ? NULL : jt->second;
}
