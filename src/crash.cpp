/*
 * Fault reporting for the loaded module. See crash.h.
 *
 * The module is mapped at a base the loader chooses at runtime and it is not a
 * file the debugger can open anyway - it lives inside the user's APK. So the
 * one thing worth printing is the faulting PC *relative to the module's text*,
 * which is directly usable:
 *
 *     unzip -p shadowgun.apk lib/armeabi-v7a/libshadowgun.so > /tmp/g.so
 *     arm-linux-gnueabihf-objdump -d --start-address=0x<off-0x20> \
 *         --stop-address=0x<off+0x20> /tmp/g.so
 *
 * Everything below runs inside a signal handler on whichever thread faulted,
 * so it uses write(2) and hand-rolled hex only: no printf, no malloc, no
 * locks. After reporting it restores the default action and re-raises, so the
 * process still dies exactly as it would have.
 */
#include <dlfcn.h>
#include <sys/mman.h>
#include <stdlib.h>
#include "crash.h"
#include "unwind_arm.h"

#include <fcntl.h>
#include <sys/syscall.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

static uintptr_t   g_text_base = 0;
static size_t      g_text_size = 0;
static uintptr_t   g_exidx_base = 0;
static size_t      g_exidx_size = 0;
static const char *g_soname    = "module";

/*
 * Only the first thread to fault gets to report.
 *
 * This engine runs several threads and they do not fault politely one at a
 * time. When two go down together, both handlers run concurrently and both
 * write to fd 2 a few bytes at a time, and the two reports interleave into
 * something like
 *
 *     r0libMassEffect.so = +0x57bc27640x003e7198
 *
 * which is not a corrupted register, it is two lines braided together. That
 * cost an evening: it reads as though the crash handler itself had faulted, so
 * the real report - the only evidence of where the engine died - looked
 * untrustworthy when it was simply mixed with another one.
 *
 * A later thread is not silenced entirely, because "a second thread also
 * faulted" is itself worth knowing; it gets one atomic line and then dies.
 */
static volatile int g_reporting = 0;

static void put(const char *s)
{
    ssize_t n = write(2, s, strlen(s));
    (void)n;
}

static void put_hex(uintptr_t v)
{
    char buf[10] = { '0', 'x' };
    for (int i = 0; i < 8; i++)
        buf[9 - i] = "0123456789abcdef"[(v >> (4 * i)) & 0xf];
    ssize_t n = write(2, buf, sizeof(buf));
    (void)n;
}

/* One register, plus where it lands inside the module if it lands there. */
static void put_reg(const char *name, uintptr_t v)
{
    put("       ");
    put(name);
    put(" = ");
    put_hex(v);
    so_module *m = so_module_for_address(v);
    Dl_info di;
    if (!m && v > 0x10000 && dladdr((void *)v, &di) && di.dli_fname) {
        /* The loader itself or a host library: offset usable with addr2line. */
        const char *base = strrchr(di.dli_fname, '/');
        put("  (host ");
        put(base ? base + 1 : di.dli_fname);
        put("+");
        put_hex(v - (uintptr_t)di.dli_fbase);
        if (di.dli_sname) {
            put(" ");
            put(di.dli_sname);
        }
        put(")");
    }
    if (m) {
        put("  (");
        put(m->soname ? m->soname : "module");
        put("+");
        put_hex(v - m->text_base);
        put(")");
    }
    put("\n");
}

static const char *signal_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGILL:  return "SIGILL";
    case SIGFPE:  return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    default:      return "signal";
    }
}

/*
 * A return-address scan of the faulting stack.
 *
 * There is no unwinder here: the module was mapped by our own loader, so it is
 * in no dl_iterate_phdr list and .ARM.exidx never gets registered. Frame
 * pointers are out too - the game is built -fomit-frame-pointer like every
 * NDK release.
 *
 * What is left is the crude method, and for this job it is enough: walk the
 * stack word by word and print everything that points into the module's text.
 * It reports call sites that already returned as well as live frames, so it is
 * a list of suspects rather than a backtrace - but every real frame is in it,
 * and that is what turns "SIGSEGV somewhere in the C++ runtime" into a
 * specific engine function to disassemble.
 *
 * Thumb return addresses have bit 0 set; the offsets are printed with it
 * cleared so they can be pasted straight into objdump --start-address.
 */
/*
 * Is this word plausibly a return address, or just data that happens to fall
 * inside the module?
 *
 * The range check alone is not enough, and the reason is a detail of the
 * loader: text_size is the whole PT_LOAD segment, not just .text, so every
 * pointer-shaped constant in .rodata and .data passes it. A report full of
 * those is worse than no report - one run appeared to show an infinite
 * recursion between two addresses that objdump says are not code at all, and
 * that cost time to disbelieve.
 *
 * A real return address always has a call immediately before it. This game is
 * ARM throughout - no Thumb in the disassembly - so the four forms that can
 * precede one are cheap to recognise:
 *
 *     BL / BLcond      cond 101 1 <imm24>
 *     BLX <imm>        1111 101 <H> <imm24>
 *     BLX <reg>        cond 0001 0010 ... 0011 <Rm>
 *     ldr pc, [...]    the `mov lr, pc ; ldr pc, [r3]` virtual-call pair this
 *                      engine uses everywhere, where lr points just past the
 *                      load
 *
 * False positives are still possible - data can encode a valid BL - but they
 * drop from "most lines" to "rare", which is the difference between a list of
 * suspects and noise.
 */
static bool looks_like_return_address(uintptr_t v)
{
    uint32_t prev = *(const uint32_t *)((v & ~(uintptr_t)1) - 4);

    if ((prev & 0x0F000000) == 0x0B000000) return true;  /* BL / BLcond   */
    if ((prev & 0xFE000000) == 0xFA000000) return true;  /* BLX immediate */
    if ((prev & 0x0FFFFFF0) == 0x012FFF30) return true;  /* BLX register  */
    if ((prev & 0x0E10F000) == 0x0410F000) return true;  /* ldr pc, [..]  */

    return false;
}

/*
 * A fault a few bytes below sp is a stack overflow: sp itself already sits in
 * the guard page, and reading the stack from there faults again inside the
 * handler. Start the scan at the first page that is really stack.
 */
static uintptr_t stack_scan_start(uintptr_t sp, const siginfo_t *si)
{
    uintptr_t addr = si ? (uintptr_t)si->si_addr : 0;
    if (addr && addr <= sp && sp - addr < 4096) {
        put("       (fault just below sp: stack overflow; scanning from the next page)\n");
        return (sp + 4095) & ~(uintptr_t)4095;
    }
    return sp;
}

static void put_stack(uintptr_t sp)
{
    if (!sp || !g_text_size)
        return;

    put("       stack (module return addresses, innermost first):\n");

    /* Bounded so a corrupted sp cannot walk off the mapping and fault us a
     * second time inside the handler, where the report would be lost. */
    const uintptr_t *w   = (const uintptr_t *)(sp & ~(uintptr_t)3);
    const int        max = 1024;   /* 4 KiB of stack */
    int              hits = 0;

    for (int i = 0; i < max && hits < 24; i++) {
        uintptr_t v = w[i];
        so_module *m = so_module_for_address(v);
        if (!m || v < m->text_base + 4 || v >= m->text_base + m->text_size)
            continue;
        if (!looks_like_return_address(v))
            continue;
        put("         ");
        put(m->soname ? m->soname : "module");
        put("+");
        put_hex((v & ~(uintptr_t)1) - m->text_base);
        put("\n");
        hits++;
    }

    if (!hits)
        put("         (none - the fault is not below any module frame)\n");

    /* Unfiltered: every word in the first 8 KiB that points into a module's
     * text. Noisier than the list above, but it cannot miss a frame whose call
     * site the return-address test does not recognise. */
    put("       stack (any module text address, first 8 KiB):\n");
    for (int i = 0, n = 0; i < 2048 && n < 48; i++) {
        uintptr_t v = w[i];
        so_module *m = so_module_for_address(v);
        if (!m || v < m->text_base || v >= m->text_base + m->text_size)
            continue;
        put("         ");
        put(m->soname ? m->soname : "module");
        put("+");
        put_hex(v - m->text_base);
        put("\n");
        n++;
    }

    /* The loader's own frames: when the fault is in host code (a thunk, a JNI
     * fake) the chain above is empty and this is the only trace there is. */
    put("       stack (loader return addresses, innermost first):\n");
    int host_hits = 0;
    for (int i = 0; i < 16384 && host_hits < 40; i++) {
        uintptr_t v = w[i];
        if (v < 0x10000 || so_module_for_address(v))
            continue;
        Dl_info di;
        if (!dladdr((void *)v, &di) || !di.dli_fname || !strstr(di.dli_fname, "shadowgun"))
            continue;
        put("         +");
        put_hex(v - (uintptr_t)di.dli_fbase);
        put(" (sp+");
        put_hex((uintptr_t)i * 4);
        put(")\n");
        host_hits++;
    }
}

/*
 * A file descriptor on /dev/null, opened once at init, used to ask the kernel
 * whether an address is readable.
 *
 * write() validates its buffer and returns EFAULT rather than delivering a
 * signal, which is the one way to probe memory from inside a signal handler
 * without risking a second fault. Everything else - reading it directly,
 * installing a temporary handler, msync - either faults or is not
 * async-signal-safe.
 */
static int g_null_fd = -1;

static bool readable(const void *p, size_t len)
{
    if (g_null_fd < 0 || !p)
        return false;
    return write(g_null_fd, p, len) == (ssize_t)len;
}

/*
 * Four words at a pointer, when the pointer is readable.
 *
 * The registers alone say where a fault happened; they do not say what the
 * program thought it had. This port spent hours on a fault whose whole
 * explanation was "the std::vector at r12 has begin == 0" - three words that
 * were sitting in memory the entire time and that no report ever printed.
 */
static void put_words(const char *name, uintptr_t v)
{
    if (!readable((const void *)v, 16))
        return;

    const uintptr_t *w = (const uintptr_t *)v;
    put("       [");
    put(name);
    put("] = ");
    for (int i = 0; i < 4; i++) {
        put_hex(w[i]);
        put(i == 3 ? "\n" : " ");
    }
}

/*
 * The real frame chain, walked from .ARM.exidx.
 *
 * Printed before the suspect scan, and it does not replace it: anything this
 * cannot walk - a function the table marks CANTUNWIND, an opcode form the
 * interpreter refuses to guess at - falls through to the scan, so a fault is
 * never reported with less than it was before this existed.
 *
 * The distinction is the whole point. A frame here is one the unwind tables say
 * is live; a line in the scan below is a word on the stack that looks like a
 * return address, which includes calls that already returned.
 */
static void put_unwind(const mcontext_t *m)
{
    if (!g_exidx_size || !g_text_size) {
        put("       (no unwind table registered for the module)\n");
        return;
    }

    put("       backtrace (unwound, innermost first):\n");

    uint32_t regs[16];
    memset(regs, 0, sizeof(regs));
    regs[0]  = m->arm_r0;  regs[1]  = m->arm_r1;
    regs[2]  = m->arm_r2;  regs[3]  = m->arm_r3;
    regs[4]  = m->arm_r4;  regs[5]  = m->arm_r5;
    regs[6]  = m->arm_r6;  regs[7]  = m->arm_r7;
    regs[8]  = m->arm_r8;  regs[9]  = m->arm_r9;
    regs[10] = m->arm_r10; regs[11] = m->arm_fp;
    regs[12] = m->arm_ip;  regs[13] = m->arm_sp;
    regs[14] = m->arm_lr;  regs[15] = m->arm_pc;

    ArmUnwindContext ctx;
    ctx.exidx_base = g_exidx_base;
    ctx.exidx_size = g_exidx_size;
    ctx.readable   = readable;

    int printed = 0;
    for (int depth = 0; depth < 32; depth++) {
        uintptr_t pc = regs[15] & ~(uintptr_t)1;

        put("         ");
        if (pc >= g_text_base && pc < g_text_base + g_text_size) {
            put(g_soname);
            put("+");
            put_hex(pc - g_text_base);
        } else {
            put("pc ");
            put_hex(pc);
            put(" (outside the module)");
        }
        put("\n");
        printed++;

        if (!arm_unwind_step(&ctx, regs)) {
            put("         (chain ends here - the tables cannot go further)\n");
            break;
        }
    }

    if (!printed)
        put("         (nothing to walk)\n");
}

static void on_fault(int sig, siginfo_t *si, void *ucontext)
{
    const ucontext_t *uc = (const ucontext_t *)ucontext;

    /*
     * __sync_lock_test_and_set rather than a mutex: this is a signal handler
     * and pthread_mutex_lock is not async-signal-safe. One atomic word is, and
     * it is all the exclusion needed - the loser never writes again.
     */
    if (__sync_lock_test_and_set(&g_reporting, 1)) {
        put("\nFATAL: a second thread faulted while the first was still "
            "reporting; its details are omitted so the report above stays "
            "readable.\n");
        /*
         * Wait, do not die.
         *
         * The obvious thing here is to restore SIG_DFL and re-raise, and it is
         * wrong: this thread dies, the process goes with it, and the winner is
         * killed mid-sentence. The first attempt at this fix produced
         *
         *     pc = 0x4f66f384  (libMassEffect.so
         *
         * - the report truncated exactly where the other thread pulled the
         * floor out. So the loser parks instead and lets the winner finish and
         * take the process down. Bounded, because a winner that never finishes
         * must not turn a crash into a hang: after two seconds this thread
         * gives up and dies itself, which at least preserves the exit status.
         */
        for (int i = 0; i < 200; i++) {
            struct timespec ts = { 0, 10 * 1000 * 1000 };  /* 10 ms */
            nanosleep(&ts, NULL);
        }
        struct sigaction dfl2;
        memset(&dfl2, 0, sizeof(dfl2));
        dfl2.sa_handler = SIG_DFL;
        sigaction(sig, &dfl2, NULL);
        raise(sig);
        return;
    }

    /*
     * Which thread. This engine runs six of them and the fault is on a worker,
     * not on the frame path - a fact that took three sessions to establish
     * because nothing in the report distinguished them. syscall(SYS_gettid) is
     * async-signal-safe; pthread_self is not, and gettid() the glibc wrapper
     * did not exist before 2.30.
     */
    /* The memory map first: a fault at a stack edge is only readable against it. */
    if (getenv("SHADOWGUN_CRASH_MAPS")) {
        int fd = open("/proc/self/maps", O_RDONLY);
        if (fd >= 0) {
            char buf[4096];
            ssize_t n;
            while ((n = read(fd, buf, sizeof(buf))) > 0)
                if (write(2, buf, (size_t)n) < 0)
                    break;
            close(fd);
        }
    }
    put("\nFATAL: [tid ");
    put_hex((uintptr_t)syscall(SYS_gettid));
    put("] ");
    put(signal_name(sig));
    put(" at ");
    put_hex((uintptr_t)(si ? si->si_addr : 0));
    put("\n");

    if (uc) {
        const mcontext_t *m = &uc->uc_mcontext;
        put_reg("pc", m->arm_pc);
        put_reg("lr", m->arm_lr);
        put_reg("sp", m->arm_sp);
        put_reg("r0", m->arm_r0);
        put_reg("r1", m->arm_r1);
        put_reg("r2", m->arm_r2);
        put_reg("r3", m->arm_r3);
        put_reg("ip", m->arm_ip);

        /* What the registers point at, which is usually the actual answer. */
        put_words("r0", m->arm_r0);
        put_words("r1", m->arm_r1);
        put_words("r2", m->arm_r2);
        put_words("r3", m->arm_r3);
        put_words("ip", m->arm_ip);
        put_words("sp", m->arm_sp);
    }

    if (uc)
        put_unwind(&uc->uc_mcontext);

    put("       module text at ");
    put_hex(g_text_base);
    put("\n");

    if (uc)
        put_stack(stack_scan_start(uc->uc_mcontext.arm_sp, si));

    /* Die the way we would have died without this handler. */
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigaction(sig, &dfl, NULL);
    raise(sig);
}

/*
 * The same stack scan, on demand and without a fault.
 *
 * A JNI method this port implements knows what it was asked for and nothing
 * about who asked. That is fine while the engine is making progress and useless
 * the moment it parks: "GetPrimaryExternalStorageDirectory is called once per
 * frame" does not say which of the engine's state machines is doing the
 * calling, and that is the only question worth answering.
 *
 * Reuses put_stack() rather than a second implementation, so a frame listed
 * here means what it means in a crash report: a word on this thread's stack
 * that points just past a call instruction in the module.
 */
extern "C" void crash_report_backtrace(const char *tag)
{
    if (!g_text_size)
        return;

    put("\nTRACE: module frames at ");
    put(tag ? tag : "(here)");
    put(":\n");
    put_stack((uintptr_t)__builtin_frame_address(0));
}

/*
 * SIGUSR2: where is this thread right now? Sent per thread from outside
 * (tools/thread_dump.sh) when the game is alive but waiting on something. Prints
 * and returns; nothing is changed.
 */
static void on_dump(int sig, siginfo_t *si, void *ucontext)
{
    (void)sig; (void)si;
    const ucontext_t *uc = (const ucontext_t *)ucontext;
    if (!uc)
        return;
    const mcontext_t *m = &uc->uc_mcontext;
    put("\nDUMP: [tid ");
    put_hex((uintptr_t)syscall(SYS_gettid));
    put("]\n");
    put_reg("pc", m->arm_pc);
    put_reg("lr", m->arm_lr);
    put_stack(m->arm_sp);
}

/* An alternate signal stack for the calling thread (leaked on purpose: threads are few). */
extern "C" void crash_report_thread_init(void)
{
    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_size = 1024 * 1024;
    ss.ss_sp = mmap(NULL, ss.ss_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ss.ss_sp != MAP_FAILED) {
        sigaltstack(&ss, NULL);
        if (getenv("SHADOWGUN_ALTSTACK_TRACE")) {
            put("altstack tid ");
            put_hex((uintptr_t)syscall(SYS_gettid));
            put(" at ");
            put_hex((uintptr_t)ss.ss_sp);
            put("\n");
        }
    }
}

extern "C" void crash_report_init(so_module *mod, const char *soname)
{
    if (mod) {
        g_text_base  = mod->text_base;
        g_text_size  = mod->text_size;
        g_exidx_base = mod->arm_exidx;
        g_exidx_size = mod->arm_exidx_size;
    }
    if (soname)
        g_soname = soname;

    /* O_WRONLY so a probe never actually consumes anything. */
    g_null_fd = open("/dev/null", O_WRONLY);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    crash_report_thread_init();
    sigemptyset(&sa.sa_mask);

    {
        struct sigaction du;
        memset(&du, 0, sizeof(du));
        du.sa_sigaction = on_dump;
        du.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&du.sa_mask);
        sigaction(SIGUSR2, &du, NULL);
    }
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);

    /*
     * SIGABRT too. glibc's heap checks ("free(): invalid pointer",
     * "malloc(): corrupted top size") abort() instead of faulting, and without
     * this the process just dies with "uncaught target signal 6" and no clue
     * about which module frame handed the bad pointer to free(). The stack
     * scan below is exactly as useful there as it is for a SIGSEGV.
     */
    sigaction(SIGABRT, &sa, NULL);
}
