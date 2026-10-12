/* Sampling profile of the engine thread; see pc_prof.h. */

#define _GNU_SOURCE 1
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "pc_prof.h"
#include "port_env.h"
#include "so_util.h"
#include "trace.h"

namespace {

constexpr uint32_t kRing = 1u << 12;            /* samples kept between two reports */
constexpr long     kPeriodUs = 4000;            /* 250 Hz */
constexpr int      kReportEverySamples = 2500;  /* ~10 s */
constexpr int      kStackWords = 96;            /* stack words kept per sample, for the caller chain */
constexpr int      kChain = 8;

struct Sample {
    uint32_t pc, lr, sp, r7;
    uint32_t stack[kStackWords];
};

Sample            g_ring[kRing];
volatile uint32_t g_head;
pthread_t         g_target;
bool              g_started;
std::string       g_want;               /* SHADOWGUN_PC_PROF_THREAD: sample the busiest thread whose name contains this */
volatile int      g_tid;                /* its tid once found; 0 = sample the engine thread through g_target */

/* Runs on the engine thread: copies registers and the top of the stack, nothing else. The analysis is done by
 * the sampler thread, where it may take its time and call anything. */
void on_prof(int, siginfo_t *, void *ctx)
{
    const ucontext_t *uc = (const ucontext_t *)ctx;
    Sample &s = g_ring[g_head & (kRing - 1)];
    s.pc = (uint32_t)uc->uc_mcontext.arm_pc;
    s.lr = (uint32_t)uc->uc_mcontext.arm_lr;
    s.sp = (uint32_t)uc->uc_mcontext.arm_sp;
    s.r7 = (uint32_t)uc->uc_mcontext.arm_r7;
    memcpy(s.stack, (const void *)(uintptr_t)s.sp, sizeof(s.stack));
    g_head = g_head + 1;
}

/* "libunity.so" and the offset in it for a module of ours; "host:<file>" and the file offset otherwise. */
void locate(uint32_t pc, std::string &mod, uint32_t &off)
{
    so_module *m = so_module_for_address(pc);
    if (m) {
        mod = m->soname ? m->soname : "module";
        off = pc - (uint32_t)m->text_base;
        return;
    }
    Dl_info di;
    if (dladdr((void *)(uintptr_t)pc, &di) && di.dli_fname) {
        const char *b = strrchr(di.dli_fname, '/');
        mod = std::string("host:") + (b ? b + 1 : di.dli_fname);
        off = pc - (uint32_t)(uintptr_t)di.dli_fbase;
        return;
    }
    mod = "unknown";
    off = pc;
}

/* True when r (a return address, Thumb bit included) is in one of the game's modules and the instruction before it
 * is a call - which is what separates a real return address from a stale code pointer on the stack. */
bool is_guest_return(uint32_t r)
{
    uint32_t a = r & ~1u;
    so_module *m = so_module_for_address(a);
    if (!m || a < (uint32_t)m->text_base + 8 || a >= (uint32_t)(m->text_base + m->text_size))
        return false;
    if (r & 1) {
        uint16_t h2 = *(const uint16_t *)(uintptr_t)(a - 2);
        if ((h2 & 0xFF80) == 0x4780)                                    /* blx reg */
            return true;
        uint16_t h1 = *(const uint16_t *)(uintptr_t)(a - 4);
        return (h1 & 0xF800) == 0xF000 && ((h2 & 0xD000) == 0xD000 || (h2 & 0xD000) == 0xC000);   /* bl / blx imm */
    }
    uint32_t i = *(const uint32_t *)(uintptr_t)(a - 4);
    return (i & 0x0F000000) == 0x0B000000 || (i & 0x0FFFFFF0) == 0x012FFF30;
}

/* The guest frames of a sample, innermost first: lr, then every return address found on the stack. */
int chain_of(const Sample &s, uint32_t *out)
{
    int n = 0;
    auto add = [&](uint32_t r) {
        if (n < kChain && (n == 0 || out[n - 1] != r) && is_guest_return(r))
            out[n++] = r;
    };
    add(s.lr);
    for (int i = 0; i < kStackWords && n < kChain; i++)
        add(s.stack[i]);
    return n;
}

typedef std::map<std::string, std::map<uint32_t, uint32_t>> BlockMap;      /* module -> 64-byte block -> samples */

void print_blocks(const char *tag, const BlockMap &by_mod, uint32_t n, bool percent_cut)
{
    std::vector<std::pair<uint32_t, std::string>> order;
    for (auto &m : by_mod) {
        uint32_t t = 0;
        for (auto &b : m.second)
            t += b.second;
        order.push_back({t, m.first});
    }
    std::sort(order.rbegin(), order.rend());
    for (auto &o : order) {
        if (percent_cut && o.first * 100 < n)       /* under 1 % of the thread */
            continue;
        std::vector<std::pair<uint32_t, uint32_t>> blocks;
        for (auto &b : by_mod.at(o.second))
            blocks.push_back({b.second, b.first});
        std::sort(blocks.rbegin(), blocks.rend());
        std::string out = std::string(tag) + "[" + o.second + "]";
        char tok[40];
        size_t shown = 0, total = 0;
        for (auto &b : blocks) {
            snprintf(tok, sizeof(tok), " 0x%x=%u", b.second, b.first);
            out += tok;
            total++;
            if (++shown >= 40 || out.size() > 800) {
                trace("%s", out.c_str());
                out = std::string(tag) + "[" + o.second + "]";
                shown = 0;
            }
            if (total >= 120 || b.first * 1000 < n)     /* the rest is under 0.1 % each */
                break;
        }
        if (shown)
            trace("%s", out.c_str());
    }
}

void report(uint32_t first, uint32_t last)
{
    BlockMap leaf, incl;
    std::map<std::string, uint32_t> totals;
    std::map<std::string, uint32_t> waits;      /* "nr=240 libunity.so+0x1c3700" -> samples */
    uint32_t n = 0;
    for (uint32_t i = first; i != last; i++, n++) {
        const Sample &s = g_ring[i & (kRing - 1)];
        std::string mod;
        uint32_t off;
        locate(s.pc, mod, off);
        leaf[mod][off & ~63u]++;
        totals[mod]++;

        uint32_t chain[kChain];
        int depth = chain_of(s, chain);
        for (int d = 0; d < depth; d++) {
            std::string m2;
            uint32_t o2;
            locate(chain[d], m2, o2);
            incl[m2][o2 & ~63u]++;
        }
        /* Sitting in a syscall wrapper of glibc: say which syscall (r7) and which game function is waiting. */
        if (mod.compare(0, 5, "host:") == 0 && (mod.find("libpthread") != std::string::npos ||
                                                 mod.find("libc.so") != std::string::npos) && s.r7 < 600) {
            char key[160] = "";
            if (depth) {
                std::string m2;
                uint32_t o2;
                locate(chain[0], m2, o2);
                snprintf(key, sizeof(key), "nr=%u %s+0x%x", s.r7, m2.c_str(), o2 & ~15u);
            } else {
                snprintf(key, sizeof(key), "nr=%u (no game frame)", s.r7);
            }
            waits[key]++;
        }
    }
    if (!n)
        return;
    std::vector<std::pair<uint32_t, std::string>> order;
    for (auto &t : totals)
        order.push_back({t.second, t.first});
    std::sort(order.rbegin(), order.rend());

    char line[600];
    int len = snprintf(line, sizeof(line), "pcprof: %u samples of the engine thread:", n);
    for (auto &o : order)
        len += snprintf(line + len, sizeof(line) - len, " %s %.1f%%", o.second.c_str(), 100.0 * o.first / n);
    trace("%s", line);

    print_blocks("pcprof", leaf, n, true);
    print_blocks("pcprof_incl", incl, n, false);

    std::vector<std::pair<uint32_t, std::string>> w;
    for (auto &x : waits)
        w.push_back({x.second, x.first});
    std::sort(w.rbegin(), w.rend());
    std::string out = "pcprof_wait:";
    char tok[200];
    for (size_t i = 0; i < w.size() && i < 14; i++) {
        snprintf(tok, sizeof(tok), " [%s]=%u", w[i].second.c_str(), w[i].first);
        out += tok;
    }
    trace("%s", out.c_str());
}

/* The busiest thread (user+system CPU time) of this process whose name contains g_want; 0 if there is none. */
int find_named_thread()
{
    DIR *d = opendir("/proc/self/task");
    if (!d)
        return 0;
    int best = 0;
    unsigned long best_ticks = 0;
    while (struct dirent *e = readdir(d)) {
        int tid = atoi(e->d_name);
        if (tid <= 0)
            continue;
        char path[64], buf[512];
        snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        size_t len = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[len] = 0;
        /* "tid (comm) S ...": comm may hold spaces and parentheses, so cut at the first '(' and the last ')'. */
        char *open = strchr(buf, '('), *close = strrchr(buf, ')');
        if (!open || !close || close < open)
            continue;
        *close = 0;
        if (!strstr(open + 1, g_want.c_str()))
            continue;
        unsigned long ut = 0, st = 0;
        /* After ") ": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime */
        if (sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2)
            continue;
        if (!best || ut + st > best_ticks) {
            best = tid;
            best_ticks = ut + st;
        }
    }
    closedir(d);
    return best;
}

void *sampler(void *)
{
    pthread_setname_np(pthread_self(), "pcprof");
    uint32_t reported = g_head;
    long since_lookup = 0;
    for (;;) {
        struct timespec ts = { 0, kPeriodUs * 1000 };
        nanosleep(&ts, NULL);
        if (!g_want.empty()) {
            /* Look again about every 5 s until a thread is chosen: workers are created late and CPU time is only
             * telling once they have run for a while. Sampling starts only then. */
            if (!g_tid && ++since_lookup >= 5000000L / kPeriodUs) {
                since_lookup = 0;
                g_tid = find_named_thread();
                if (g_tid)
                    trace("pcprof: sampling thread \"%s\" tid=%d instead of the engine thread", g_want.c_str(), (int)g_tid);
            }
            if (!g_tid)
                continue;
            syscall(SYS_tgkill, (int)getpid(), (int)g_tid, SIGPROF);
        } else {
            pthread_kill(g_target, SIGPROF);
        }
        uint32_t head = g_head;
        if (head - reported >= (uint32_t)kReportEverySamples && head - reported < kRing - 64) {
            report(reported, head);
            reported = g_head;
        } else if (head - reported >= kRing - 64) {
            reported = head - (kRing - 64);     /* fell behind: drop the oldest */
        }
    }
    return NULL;
}

}  // namespace

extern "C" void pc_prof_tick(long frame)
{
    if (g_started || frame < 30)
        return;
    g_started = true;
    if (!port_getenv_long("PC_PROF", 0))
        return;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    g_target = pthread_self();
    const char *want = port_getenv("PC_PROF_THREAD");
    if (want && *want)
        g_want = want;
    pthread_t t;
    if (pthread_create(&t, NULL, sampler, NULL) == 0) {
        pthread_detach(t);
        trace("pcprof: sampling the engine thread at %ld Hz, with caller chains", 1000000L / kPeriodUs);
    }
}
