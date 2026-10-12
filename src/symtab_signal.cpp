/*
 * Signals, across the bionic/glibc layout difference.
 *
 * On 32-bit ARM bionic's sigset_t is one unsigned long and its struct sigaction
 * is 16 bytes; glibc's sigset_t is 128 bytes and sits in the middle of struct
 * sigaction. Bound straight through, sigemptyset() alone writes 124 bytes past
 * the caller's variable. libil2cpp.so needs these to work for real: its
 * collector stops the world by signalling every thread and parking it in
 * sigsuspend().
 *
 * Handlers for the fatal signals are not installed: Unity registers its own
 * crash handler for them, which would replace the port's crash report with a
 * silent exit.
 */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "so_util.h"
#include "thunk_gen.h"
#include "trace.h"

extern "C" {

int  bionic_setjmp(void *env);
void bionic_longjmp(void *env, int value);

typedef uint32_t bionic_sigset_t;

struct bionic_sigaction {
    void *handler;
    bionic_sigset_t mask;
    int flags;
    void *restorer;
};

static void to_host(const bionic_sigset_t *in, sigset_t *out)
{
    sigemptyset(out);
    for (int s = 1; s <= 32; s++)
        if (*in & (1u << (s - 1)))
            sigaddset(out, s);
}
static void to_bionic(const sigset_t *in, bionic_sigset_t *out)
{
    *out = 0;
    for (int s = 1; s <= 32; s++)
        if (sigismember(in, s) == 1)
            *out |= 1u << (s - 1);
}
static bool is_fatal(int sig)
{
    return sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGABRT ||
           sig == SIGTRAP || sig == SIGSTKFLT;
}

static int s_sigemptyset(bionic_sigset_t *s) { *s = 0; return 0; }
static int s_sigfillset(bionic_sigset_t *s) { *s = ~0u; return 0; }
static int s_sigaddset(bionic_sigset_t *s, int sig) { if (sig < 1 || sig > 32) { errno = EINVAL; return -1; } *s |= 1u << (sig - 1); return 0; }
static int s_sigdelset(bionic_sigset_t *s, int sig) { if (sig < 1 || sig > 32) { errno = EINVAL; return -1; } *s &= ~(1u << (sig - 1)); return 0; }
static int s_sigismember(const bionic_sigset_t *s, int sig) { if (sig < 1 || sig > 32) return 0; return (*s >> (sig - 1)) & 1; }

static int mask_common(int (*fn)(int, const sigset_t *, sigset_t *), int how, const bionic_sigset_t *set,
                       bionic_sigset_t *old)
{
    sigset_t hs, ho;
    if (set)
        to_host(set, &hs);
    int r = fn(how, set ? &hs : NULL, old ? &ho : NULL);
    if (old && r == 0)
        to_bionic(&ho, old);
    return r;
}
static int s_sigprocmask(int how, const bionic_sigset_t *set, bionic_sigset_t *old)
{
    return mask_common(sigprocmask, how, set, old);
}
static int s_pthread_sigmask(int how, const bionic_sigset_t *set, bionic_sigset_t *old)
{
    return mask_common(pthread_sigmask, how, set, old);
}
static int s_sigsuspend(const bionic_sigset_t *mask)
{
    sigset_t hs;
    to_host(mask, &hs);
    return sigsuspend(&hs);
}
static int s_sigpending(bionic_sigset_t *set)
{
    sigset_t hs;
    int r = sigpending(&hs);
    if (r == 0)
        to_bionic(&hs, set);
    return r;
}

static int s_sigaction(int sig, const struct bionic_sigaction *act, struct bionic_sigaction *old)
{
    if (is_fatal(sig)) {
        if (old)
            memset(old, 0, sizeof(*old));
        return 0;
    }
    struct sigaction ha, ho;
    if (act) {
        memset(&ha, 0, sizeof(ha));
        ha.sa_handler = (void (*)(int))act->handler;
        to_host(&act->mask, &ha.sa_mask);
        ha.sa_flags = act->flags & ~0x04000000; /* SA_RESTORER */
    }
    int r = sigaction(sig, act ? &ha : NULL, old ? &ho : NULL);
    if (old && r == 0) {
        old->handler = (void *)ho.sa_handler;
        to_bionic(&ho.sa_mask, &old->mask);
        old->flags = ho.sa_flags;
        old->restorer = NULL;
    }
    return r;
}
static void *s_bsd_signal(int sig, void *handler)
{
    if (is_fatal(sig))
        return NULL;
    return (void *)signal(sig, (void (*)(int))handler);
}
static int s_tkill(int tid, int sig) { return (int)syscall(SYS_tkill, tid, sig); }

/* bionic's pthread_cleanup_push/pop are macros over these two. */
struct bionic_cleanup { struct bionic_cleanup *prev; void (*routine)(void *); void *arg; };
static void s_cleanup_push(struct bionic_cleanup *c, void (*routine)(void *), void *arg)
{
    c->prev = NULL;
    c->routine = routine;
    c->arg = arg;
}
static void s_cleanup_pop(struct bionic_cleanup *c, int execute)
{
    if (execute && c->routine)
        c->routine(c->arg);
}

static double s_cosh(double x) { return cosh(x); }
static double s_sinh(double x) { return sinh(x); }
static double s_tanh(double x) { return tanh(x); }

extern char **environ;

} /* extern "C" */

DynLibFunction symtable_signal[] = {
    THUNK_SPECIFIC("sigemptyset", s_sigemptyset),
    THUNK_SPECIFIC("sigfillset", s_sigfillset),
    THUNK_SPECIFIC("sigaddset", s_sigaddset),
    THUNK_SPECIFIC("sigdelset", s_sigdelset),
    THUNK_SPECIFIC("sigismember", s_sigismember),
    THUNK_SPECIFIC("sigprocmask", s_sigprocmask),
    THUNK_SPECIFIC("pthread_sigmask", s_pthread_sigmask),
    THUNK_SPECIFIC("sigsuspend", s_sigsuspend),
    THUNK_SPECIFIC("sigpending", s_sigpending),
    THUNK_SPECIFIC("sigaction", s_sigaction),
    THUNK_SPECIFIC("bsd_signal", s_bsd_signal),
    THUNK_SPECIFIC("signal", s_bsd_signal),
    THUNK_SPECIFIC("tkill", s_tkill),
    THUNK_SPECIFIC("__pthread_cleanup_push", s_cleanup_push),
    THUNK_SPECIFIC("__pthread_cleanup_pop", s_cleanup_pop),
    /* The signal mask is not saved; the engine only uses these as setjmp/longjmp. */
    NO_THUNK("sigsetjmp",  (uintptr_t)&bionic_setjmp),
    NO_THUNK("siglongjmp", (uintptr_t)&bionic_longjmp),
    NO_THUNK("_setjmp",    (uintptr_t)&bionic_setjmp),
    NO_THUNK("_longjmp",   (uintptr_t)&bionic_longjmp),
    NO_THUNK("environ",    (uintptr_t)&environ),
    THUNK_SPECIFIC("cosh", s_cosh),
    THUNK_SPECIFIC("sinh", s_sinh),
    THUNK_SPECIFIC("tanh", s_tanh),
    { NULL, 0 },
};
