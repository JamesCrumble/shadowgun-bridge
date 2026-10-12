#ifndef SHADOWGUN_SYSCALL_PROF_H
#define SHADOWGUN_SYSCALL_PROF_H

/*
 * Who is calling the hot libc thunks, from which thread, and how long they stay blocked.
 *
 * qemu's own QEMU_SYSCALL_STATS says how many syscalls the guest makes but not who makes them. This records,
 * per (caller address, thread, kind), the call count and the time spent in the call, and prints the busiest
 * callers every 300 frames as "module+offset", ready for objdump on the module. The "ms/frame" figure of a
 * kind is how much of each frame, summed over all threads, is spent inside that call.
 *
 * SHADOWGUN_SYSCALL_PROF=0 turns it off (default on). Every clock read here is a real syscall under qemu, so
 * sysprof_now() returns 0 without reading the clock when the profile is off, and the hottest call (the Unity
 * worker thread's nanosleep, ~40 a frame) is counted but not timed. Blocking calls are timed with two clock
 * reads; contended-only for mutexes (a successful trylock costs nothing extra).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SP_NANOSLEEP = 0,       /* ns = the sleep that was asked for (not timed) */
    SP_CLOCK_GETTIME,       /* ns = 0 */
    SP_SEM_WAIT,            /* ns = time blocked */
    SP_COND_WAIT,           /* ns = time blocked (timed and untimed) */
    SP_MUTEX_WAIT,          /* ns = time blocked, contended locks only */
    SP_GL_WRITE,            /* ns = time blocked writing to the GL command pipe */
    SP_KINDS
};

/* Monotonic nanoseconds, or 0 when the profile is off. */
uint64_t sysprof_now(void);

/* Records one call. ret is the caller's return address, ns the time spent (see the kinds above). */
void sysprof_note(int kind, void *ret, uint64_t ns);

/* Called once per frame from the engine thread; prints and resets the table every 300 frames. */
void sysprof_tick(long frame);

#ifdef __cplusplus
}
#endif

#endif
