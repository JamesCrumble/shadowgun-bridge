#ifndef SHADOWGUN_FPS_UNLOCK_H
#define SHADOWGUN_FPS_UNLOCK_H

/*
 * Frame-rate unlock.
 *
 * Unity on Android runs at 30 fps unless Application.targetFrameRate says otherwise, and Shadowgun
 * sets it itself. The limiter is a sleep loop inside libunity that the port cannot see, so the only
 * lever is the icall the C# side uses: UnityEngine.Application::set_targetFrameRate.
 *
 * libunity registers its icalls through il2cpp_add_internal_call, fetched with dlsym. dlsym_impl
 * passes every symbol it resolves through fps_unlock_hook_symbol(), which swaps that one function for
 * a hook that remembers the real icall and replaces the registered setter with one that forces the
 * configured rate. Whatever the game asks for - 30, -1 - the engine gets the configured value.
 *
 * SHADOWGUN_TARGET_FPS: rate to force (default 120; 0 leaves the game's own value alone).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Called by dlsym_impl with the symbol it resolved; returns the pointer the guest should get. */
void *fps_unlock_hook_symbol(const char *name, void *sym);

/* Called once per frame from the engine thread. Pushes the rate into the engine a few frames after
 * start-up, for the case where the game never calls set_targetFrameRate and the platform default
 * (30 on Android) applies. */
void fps_unlock_tick(long frame);

#ifdef __cplusplus
}
#endif

#endif
