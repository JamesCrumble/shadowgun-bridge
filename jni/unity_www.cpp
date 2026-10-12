/*
 * com.unity3d.player.WWW - the Java half of UnityEngine.WWW.
 *
 * The engine constructs one per request and start()s it; the Java thread then
 * reports back through static natives (readCallback / doneCallback /
 * errorCallback) keyed by the native request pointer it was given.
 *
 * Local URLs are served for real - "jar:file://<apk>!/<entry>" out of the zip
 * and "file://<path>" from disk - because games load their StreamingAssets that
 * way. Anything else fails immediately with a network error: the servers this
 * 2018 build talks to are gone, and an instant error is better than a request
 * the game waits on forever.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include <zip.h>

#include "dyn.h"
#include "trace.h"

namespace {

JavaVM *g_vm = NULL;

struct Request {
    jint ptr;
    std::string url;
};

bool read_file(const std::string &path, std::vector<char> *out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out->insert(out->end(), buf, buf + n);
    fclose(f);
    return true;
}

bool read_zip_entry(const std::string &zip_path, const std::string &entry, std::vector<char> *out)
{
    int err = 0;
    zip_t *z = zip_open(zip_path.c_str(), ZIP_RDONLY, &err);
    if (!z)
        return false;
    bool ok = false;
    zip_file_t *zf = zip_fopen(z, entry.c_str(), 0);
    if (zf) {
        char buf[65536];
        zip_int64_t n;
        while ((n = zip_fread(zf, buf, sizeof(buf))) > 0)
            out->insert(out->end(), buf, buf + n);
        zip_fclose(zf);
        ok = true;
    }
    zip_close(z);
    return ok;
}

bool fetch_local(const std::string &url, std::vector<char> *out, bool *is_local)
{
    *is_local = true;
    if (url.compare(0, 9, "jar:file:") == 0) {
        std::string rest = url.substr(9);
        while (rest.compare(0, 2, "//") == 0)
            rest.erase(0, 1);
        size_t bang = rest.find("!/");
        if (bang == std::string::npos)
            return false;
        return read_zip_entry(rest.substr(0, bang), rest.substr(bang + 2), out);
    }
    if (url.compare(0, 5, "file:") == 0) {
        std::string rest = url.substr(5);
        while (rest.compare(0, 2, "//") == 0)
            rest.erase(0, 1);
        return read_file(rest, out);
    }
    if (!url.empty() && url[0] == '/')
        return read_file(url, out);
    *is_local = false;
    return false;
}

void *request_main(void *arg)
{
    Request *r = (Request *)arg;
    JNIEnv *env = NULL;
    g_vm->AttachCurrentThread(&env, NULL);

    const char *W = "com/unity3d/player/WWW";
    auto done  = (void (*)(JNIEnv *, jclass, jint))dyn_native(W, "doneCallback");
    auto error = (void (*)(JNIEnv *, jclass, jint, jstring))dyn_native(W, "errorCallback");
    auto read  = (jboolean (*)(JNIEnv *, jclass, jint, jbyteArray, jint))dyn_native(W, "readCallback");
    jclass cls = (jclass)dyn_class(W);

    std::vector<char> data;
    bool local = false;
    bool ok = fetch_local(r->url, &data, &local);
    trace("WWW %s -> %s (%zu bytes)", r->url.c_str(), ok ? "ok" : local ? "not found" : "no network", data.size());

    if (ok && read && done) {
        const size_t chunk = 256 * 1024;
        for (size_t off = 0; off < data.size(); off += chunk) {
            size_t n = data.size() - off < chunk ? data.size() - off : chunk;
            jbyteArray arr = env->NewByteArray((jsize)n);
            env->SetByteArrayRegion(arr, 0, (jsize)n, (const jbyte *)data.data() + off);
            if (!read(env, cls, r->ptr, arr, (jint)n))
                break;
        }
        done(env, cls, r->ptr);
    } else if (error) {
        error(env, cls, r->ptr, env->NewStringUTF(local ? "File not found" : "java.net.UnknownHostException: no network"));
    } else if (done) {
        done(env, cls, r->ptr);
    }
    delete r;
    return NULL;
}

} // namespace

void unity_www_register(JNIEnv *env)
{
    env->GetJavaVM(&g_vm);
    const char *W = "com/unity3d/player/WWW";
    dyn_method(W, "<init>(ILjava/lang/String;[BLjava/util/Map;)V", [](JNIEnv *, jobject self, const jvalue *a) {
        DynObject *o = (DynObject *)self;
        o->priv = new Request{a[0].i, dyn_str(a[1].l)};
        return JV();
    });
    dyn_method(W, "start()V", [](JNIEnv *, jobject self, const jvalue *) {
        DynObject *o = (DynObject *)self;
        Request *r = (Request *)o->priv;
        o->priv = NULL;
        if (r) {
            pthread_t t;
            pthread_attr_t at;
            pthread_attr_init(&at);
            pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&t, &at, request_main, r) != 0)
                delete r;
            pthread_attr_destroy(&at);
        }
        return JV();
    });
    dyn_method(W, "join()V", [](JNIEnv *, jobject, const jvalue *) { return JV(); });
    dyn_method(W, "isAlive()Z", [](JNIEnv *, jobject, const jvalue *) { return JZ(false); });
}
