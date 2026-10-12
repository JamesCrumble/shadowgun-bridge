/* Call-site profile of the hot libc thunks; see syscall_prof.h. */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "port_env.h"
#include "so_util.h"
#include "syscall_prof.h"
#include "trace.h"

namespace {

struct Slot {
    uintptr_t ret;
    pthread_t th;
    int       kind;
    uint32_t  calls;
    uint64_t  ns;
    bool      used;
};

constexpr int kSlots = 512;
Slot g_slots[kSlots];
volatile int g_lock;
int g_enabled = -1;
uint64_t g_total_calls[SP_KINDS], g_total_ns[SP_KINDS];
const char *const kNames[SP_KINDS] = { "nanosleep", "clock_gettime", "sem_wait", "cond_wait", "mutex_wait", "gl_write" };

inline void lock()   { while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) { } }
inline void unlock() { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

}  // namespace

static inline bool enabled()
{
    if (__builtin_expect(g_enabled < 0, 0))
        g_enabled = port_getenv_long("SYSCALL_PROF", 1) ? 1 : 0;
    return g_enabled != 0;
}

extern "C" uint64_t sysprof_now(void)
{
    if (!enabled())
        return 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

extern "C" void sysprof_note(int kind, void *ret, uint64_t ns)
{
    if (!enabled())
        return;
    uintptr_t r = (uintptr_t)ret & ~(uintptr_t)1;
    pthread_t me = pthread_self();
    uint32_t h = (uint32_t)((r >> 2) * 2654435761u) ^ (uint32_t)(uintptr_t)me ^ (uint32_t)kind;
    lock();
    g_total_calls[kind]++;
    g_total_ns[kind] += ns;
    for (int i = 0; i < kSlots; i++) {
        Slot &s = g_slots[(h + i) % kSlots];
        if (!s.used) {
            s.used = true;
            s.ret = r;
            s.th = me;
            s.kind = kind;
        }
        if (s.ret == r && s.th == me && s.kind == kind) {
            s.calls++;
            s.ns += ns;
            break;
        }
    }
    unlock();
}

static void describe(uintptr_t addr, char *out, size_t n)
{
    so_module *m = so_module_for_address(addr);
    if (m)
        snprintf(out, n, "%s+0x%lx", m->soname ? m->soname : "module", (unsigned long)(addr - m->text_base));
    else
        snprintf(out, n, "host:0x%lx", (unsigned long)addr);
}

extern "C" void sysprof_tick(long frame)
{
    const long kWindow = 300;
    if (!g_enabled || frame == 0 || frame % kWindow != 0)
        return;
    std::vector<Slot> snap;
    uint64_t calls[SP_KINDS], ns[SP_KINDS];
    lock();
    for (int i = 0; i < kSlots; i++)
        if (g_slots[i].used)
            snap.push_back(g_slots[i]);
    memcpy(calls, g_total_calls, sizeof(calls));
    memcpy(ns, g_total_ns, sizeof(ns));
    memset(g_slots, 0, sizeof(g_slots));
    memset(g_total_calls, 0, sizeof(g_total_calls));
    memset(g_total_ns, 0, sizeof(g_total_ns));
    unlock();

    for (int k = 0; k < SP_KINDS; k++) {
        if (!calls[k])
            continue;
        std::vector<Slot> mine;
        for (const Slot &s : snap)
            if (s.kind == k)
                mine.push_back(s);
        /* Blocking kinds are ranked by the time they cost, the rest by how often they are called. */
        bool by_time = k != SP_CLOCK_GETTIME;
        std::sort(mine.begin(), mine.end(), [by_time](const Slot &a, const Slot &b) {
            return by_time ? a.ns > b.ns : a.calls > b.calls;
        });
        char line[1100];
        int len = snprintf(line, sizeof(line), "sysprof[%s]: %llu calls, %.2f ms/frame over %ld frames:", kNames[k],
                           (unsigned long long)calls[k], ns[k] / 1e6 / kWindow, kWindow);
        for (size_t i = 0; i < mine.size() && i < 6 && len < (int)sizeof(line) - 160; i++) {
            char where[96], tname[32] = "?";
            describe(mine[i].ret, where, sizeof(where));
            pthread_getname_np(mine[i].th, tname, sizeof(tname));
            len += snprintf(line + len, sizeof(line) - len, " [%ux %s thr=%s/%lx", mine[i].calls, where, tname,
                            (unsigned long)mine[i].th);
            if (k != SP_CLOCK_GETTIME)
                len += snprintf(line + len, sizeof(line) - len, " %.2f ms/frame avg %.0f us", mine[i].ns / 1e6 / kWindow,
                                mine[i].ns / 1000.0 / mine[i].calls);
            len += snprintf(line + len, sizeof(line) - len, "]");
        }
        trace("%s", line);
    }
}
