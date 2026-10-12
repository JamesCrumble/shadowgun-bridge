#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdio.h>
#include <stdarg.h>
#include <link.h>
#include <sys/syscall.h>
#include "platform.h"
#include "so_util.h"

#include "bionic_file.h"
#include "fps_unlock.h"

extern "C" ABI_ATTR int dl_iterate_phdr_impl(
                 int (*callback)(struct dl_phdr_info *info,
                                 size_t size, void *data),
                 void *data)
{
    // TODO:: Implement a reasonable version of this.
    fatal_error("-- dl_iterate_phdr was called! --\n");
    return -1;
}

extern "C" ABI_ATTR int login_tty_impl(int fd)
{
    return -1;
}

extern "C" ABI_ATTR long syscall_impl(long number, ...)
{
#ifdef gettid
    if (number == 0xb2)
        return gettid();
#else
    if (number == 0xb2)
        return syscall(SYS_gettid);
#endif
    return 0;
}

extern "C" ABI_ATTR void abort_impl(void)
{
    fatal_error("Guest called abort!\n");
    exit(-1);
}

/*
 * dlopen/dlsym over the loader's own modules.
 *
 * System libraries (libc, libEGL, libGLESv2, libOpenSLES...) are answered by the
 * import tables, so they get a token handle and dlsym resolves through
 * so_resolve_link(). A library that ships with the game - Unity loads
 * libil2cpp.so this way - is mapped for real and its handle is the module.
 */
#define DL_TOKEN ((void *)0xDEAD)
static thread_local const char *dl_last_error = NULL;

extern "C" ABI_ATTR void *dlopen_impl(const char *filename, int flags)
{
    if (filename == NULL)
        return DL_TOKEN;

    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;

    for (int i = 0; so_builtin_libs[i] != NULL; i++)
        if (strncmp(base, so_builtin_libs[i], strlen(so_builtin_libs[i])) == 0)
            return DL_TOKEN;

    /* Unity asks for its own libraries by short name: dlopen("il2cpp"). */
    char full[256];
    if (strncmp(base, "lib", 3) != 0 && !strstr(base, ".so")) {
        snprintf(full, sizeof(full), "lib%s.so", base);
        base = full;
    }

    so_module *mod = so_find_module(base);
    if (!mod) {
        warning("dlopen: mapping '%s'\n", filename);
        mod = so_load_module(base, NULL, NULL);
    }
    if (!mod) {
        dl_last_error = "library not found";
        warning("dlopen: '%s' not found\n", filename);
        return NULL;
    }
    return mod;
}

extern "C" ABI_ATTR char *dlerror_impl(void)
{
    const char *e = dl_last_error;
    dl_last_error = NULL;
    return (char *)e;
}

extern "C" ABI_ATTR int dlclose_impl(void *handle)
{
    return 0;
}

extern "C" ABI_ATTR void *dlsym_impl(void *handle, const char *name)
{
    void *sym = NULL;
    if (handle && so_is_module(handle))
        sym = (void *)so_symbol((so_module *)handle, name);
    if (!sym)
        sym = (void *)so_resolve_link(NULL, name);
    if (!sym)
        sym = (void *)so_symbol(NULL, name);
    sym = fps_unlock_hook_symbol(name, sym);
    static const char *trace_all = getenv("SHADOWGUN_DLSYM_TRACE");
    if (trace_all && !strcmp(trace_all, "all"))
        warning("dlsym: %s -> %s\n", name, sym ? "ok" : "missing");
    if (!sym) {
        dl_last_error = "symbol not found";
        if (getenv("SHADOWGUN_DLSYM_TRACE"))
            warning("dlsym: '%s' not found\n", name);
    }
    return sym;
}

extern "C" ABI_ATTR const void *
memchr_impl (const void *__s, int __c, size_t __n)
{
  return __builtin_memchr (__s, __c, __n);
}

extern "C" ABI_ATTR int sigsetmask_impl(int mask)
{
    WARN_STUB
    return -1;
}

extern "C" ABI_ATTR char *tempnam_impl(const char *dir, const char *pfx)
{
    WARN_STUB
    return NULL;
}

extern "C" ABI_ATTR char *tmpnam_impl(char *s)
{
    WARN_STUB
    return NULL;
}

extern "C" ABI_ATTR char *mktemp_impl(char *_template)
{
    WARN_STUB
    return NULL;
}

extern "C" ABI_ATTR int* __errno_impl(void)
{
    return __errno_location();
}

extern "C" ABI_ATTR int __android_log_write_impl(int prio, const char *tag, const char *text)
{
    char andlog[2048] = {};
    warning("LOG[%s]: %s\n", tag, text);
    return 1;
}

extern "C" ABI_ATTR int __android_log_print_impl(int prio, const char *tag, const char *fmt, ...)
{
    char andlog[2048] = {};
    va_list va;
    va_start(va, fmt);
    warning("LOG[%s]: ", tag);
    int r = vsnprintf(andlog, 2047, fmt, va);
    warning("%s\n", andlog);
    va_end(va);
    return r;
}

extern "C" ABI_ATTR int __android_log_vprint_impl(int prio, const char *tag, const char *fmt, va_list va)
{
    char andlog[2048] = {};
    warning("LOG[%s]: ", tag);
    int r = vsnprintf(andlog, 2047, fmt, va);
    warning("%s\n", andlog);
    return r;
}

extern "C" ABI_ATTR const char* __strchr_chk(const char* __s, int __ch, size_t __n) { return strchr(__s, __ch); }
extern "C" ABI_ATTR const char* __strrchr_chk(const char* __s, int __ch, size_t __n) { return strrchr(__s, __ch); }
extern "C" ABI_ATTR size_t __strlen_chk(const char* __s, size_t __n) { return strnlen(__s, __n); }

extern "C" ABI_ATTR void android_set_abort_message_impl(const char* msg)
{
    fatal_error("%s", msg);
    abort();
}

extern "C" ABI_ATTR int __system_property_get_impl(const char *name, char *value)
{
    /*
     * The game reads its UI language from Android system properties, not from
     * the SystemAndroidDelegate.GetLanguage JNI (which already returned "en"
     * and was being ignored). As an empty stub, persist.sys.language came back
     * blank and libMassEffect.so - the Russian "Fix" build - fell to its RU
     * default, so the whole menu rendered in Russian even though the asset tree
     * ships ENG_US, SPA_ES and seven more string sets.
     *
     * SHADOWGUN_LANG picks the two-letter code (default "en"); "es" gives the
     * Spanish set. Country/region answer to match so a locale-aware path does
     * not disagree with the language. Everything else keeps the empty answer
     * the game treats as "unset".
     */
    if (!name || !value)
        return 0;

    const char *lang = getenv("SHADOWGUN_LANG");
    if (!lang || !*lang)
        lang = "en";

    const char *out = NULL;
    if (strstr(name, "language"))
        out = lang;
    else if (strstr(name, "locale"))
        out = (strcmp(lang, "es") == 0) ? "es_ES"
            : (strcmp(lang, "en") == 0) ? "en_US" : lang;
    else if (strstr(name, "country") || strstr(name, "region"))
        out = (strcmp(lang, "es") == 0) ? "ES" : "US";

    if (out) {
        size_t n = strlen(out);
        if (n > 31) n = 31;
        memcpy(value, out, n);
        value[n] = 0;
        return (int)n;
    }

    WARN_STUB;
    value[0] = 0;
    return 0;
}

extern "C" ABI_ATTR int __open_2_impl(const char* pathname, int flags) {
  return open(pathname, flags);
}

// Taken from https://github.com/libhybris/libhybris/blob/master/hybris/common/hooks.c
ABI_ATTR int scandirat_impl(int fd, const char *dir,
                      struct bionic_dirent ***namelist,
                      int (*filter) (const struct bionic_dirent *),
                      int (*compar) (const struct bionic_dirent **,
                                     const struct bionic_dirent **))
{
    struct dirent **namelist_r;
    struct bionic_dirent **result;
    struct bionic_dirent *filter_r;

    int i = 0;
    size_t nItems = 0;

    int res = scandirat(fd, dir, &namelist_r, NULL, NULL);

    if (res > 0 && namelist_r != NULL) {
        result = (bionic_dirent**)malloc(res * sizeof(struct bionic_dirent));
        if (!result)
            return -1;

        for (i = 0; i < res; i++) {
            filter_r = (bionic_dirent*)malloc(sizeof(struct bionic_dirent));
            if (!filter_r) {
                while (i-- > 0)
                    free(result[i]);
                free(result);
                return -1;
            }

            filter_r->d_ino = namelist_r[i]->d_ino;
            filter_r->d_off = namelist_r[i]->d_off;
            filter_r->d_reclen = namelist_r[i]->d_reclen;
            filter_r->d_type = namelist_r[i]->d_type;

            strcpy(filter_r->d_name, namelist_r[i]->d_name);
            filter_r->d_name[sizeof(namelist_r[i]->d_name) - 1] = '\0';

            if (filter != NULL && !(*filter)(filter_r)) {//apply filter
                free(filter_r);
                continue;
            }

            result[nItems++] = filter_r;
        }
        
        if (nItems && compar != NULL) // sort
            qsort(result, nItems, sizeof(struct bionic_dirent *), (__compar_fn_t)compar);

        *namelist = result;
    } else {
        return res;
    }

    return nItems;
}

ABI_ATTR int scandir_impl(const char *dir,
                      struct bionic_dirent ***namelist,
                      int (*filter) (const struct bionic_dirent *),
                      int (*compar) (const struct bionic_dirent **,
                                     const struct bionic_dirent **))
{
    return scandirat_impl(AT_FDCWD, dir, namelist, filter, compar);
}
