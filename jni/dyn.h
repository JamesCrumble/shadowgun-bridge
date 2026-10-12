/*
 * Dynamic half of the fake JNI.
 *
 * The inherited class registry is static: a C++ struct per Java class, a method
 * table per struct. That fits a game that talks to a dozen of its own classes.
 * Unity does not: libunity.so reaches ~75 framework classes by name through its
 * generic jni:: wrappers, and which methods it touches depends on the code path.
 *
 * So anything the static registry does not know is answered here instead:
 * FindClass creates the class on first use, GetMethodID/GetFieldID create ids
 * that remember their name and descriptor, and a call lands in a handler looked
 * up by "name+descriptor" - first on the class the id was made for, then on the
 * wildcard class "*". A call with no handler is logged (once per method) and
 * returns zero/null, which is what makes the missing pieces discoverable from a
 * log instead of from a crash.
 */
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "jni.h"
#include "jni_internals.h"

struct DynMethod {
    std::string cls;      /* class path the id was created on */
    std::string name;
    std::string sig;      /* full descriptor, "(I)V" */
    bool is_static;
    int nargs;
    int calls = 0;
};

struct DynField {
    std::string cls;
    std::string name;
    std::string sig;
    bool is_static;
};

/* An instance of a class the registry has no struct for. */
struct DynObject : public Object {
    Class *cls;
    std::map<std::string, jvalue> fields;
    void *priv = nullptr;
    Class *_getClass() override { return cls; }
};

using DynFn = std::function<jvalue(JNIEnv *env, jobject self, const jvalue *args)>;
using DynGet = std::function<jvalue(JNIEnv *env, jobject self)>;

/* cls may be "*" (any class). key is "name" + descriptor, e.g.
 * "getPackageName()Ljava/lang/String;", or the bare name to match any descriptor. */
void dyn_method(const char *cls, const char *key, DynFn fn);
void dyn_field(const char *cls, const char *name, DynGet fn);

Class *dyn_class(const char *classpath);
jobject dyn_new(const char *classpath);
bool dyn_is(jobject obj, const char *classpath);
Class *dyn_class_of(jobject obj);   /* safe for a jclass too */
const char *dyn_class_name(jclass cls);
bool dyn_trace_jni();   /* SHADOWGUN_JNI_TRACE=1 */
const char *dyn_superclass_path(const char *cls);   /* NULL for java/lang/Object */
bool dyn_is_subclass(const char *cls, const char *ancestor);

/* Natives the game registered with RegisterNatives. */
void *dyn_native(const char *classpath, const char *name);

/* jvalue constructors, to keep handler bodies readable. */
static inline jvalue JV() { jvalue v; v.j = 0; return v; }
static inline jvalue JI(jint i) { jvalue v; v.j = 0; v.i = i; return v; }
static inline jvalue JZ(bool b) { jvalue v; v.j = 0; v.z = b ? 1 : 0; return v; }
static inline jvalue JJ(jlong j) { jvalue v; v.j = j; return v; }
static inline jvalue JF(jfloat f) { jvalue v; v.j = 0; v.f = f; return v; }
static inline jvalue JD(jdouble d) { jvalue v; v.d = d; return v; }
static inline jvalue JL(jobject l) { jvalue v; v.j = 0; v.l = l; return v; }
jvalue JS(JNIEnv *env, const char *utf);
const char *dyn_str(jobject jstr);   /* "" for null */

/* Internal entry points used by jni.cpp. */
ManagedMethod *dyn_get_method(Class *clz, const char *name, const char *sig, bool is_static);
FieldId *dyn_get_field(Class *clz, const char *name, const char *sig, bool is_static);
jvalue dyn_call_v(JNIEnv *env, jobject self, const ManagedMethod *m, va_list va);
jvalue dyn_call_a(JNIEnv *env, jobject self, const ManagedMethod *m, const jvalue *args);
jvalue dyn_field_get(JNIEnv *env, jobject self, const FieldId *f);
void dyn_field_set(JNIEnv *env, jobject self, const FieldId *f, jvalue v);
void dyn_register_natives(Class *clz, const JNINativeMethod *methods, int n);
bool dyn_is_dynamic_class(const Class *clz);
