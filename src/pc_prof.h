#ifndef SHADOWGUN_PC_PROF_H
#define SHADOWGUN_PC_PROF_H

/*
 * Sampling profile of the engine thread.
 *
 * QEMU_GUEST_PROF samples qemu's main guest thread, which in this port is asleep: the engine runs on a thread of
 * its own. So the engine samples itself: a helper thread sends it SIGPROF 250 times a second, the handler stores
 * the interrupted guest PC, and every 10 s the helper prints a histogram - first by module, then the hottest
 * 64-byte blocks of each module as "0xOFFSET=count", offsets relative to the module's text base, i.e. directly
 * usable with objdump/nm (tools/pcprof.py turns the lines into function names).
 *
 * SHADOWGUN_PC_PROF=1 turns it on (the app sets it with PROFILING). Off, it costs nothing.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Called once per frame from the engine thread; starts sampling that thread a few frames after start-up. */
void pc_prof_tick(long frame);

#ifdef __cplusplus
}
#endif

#endif
