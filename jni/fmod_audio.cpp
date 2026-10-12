/*
 * org.fmod.FMODAudioDevice - Unity's audio output on Android.
 *
 * In the original the Java class runs a thread that asks FMOD for mixed PCM
 * (fmodProcess fills a direct ByteBuffer with one DSP block of 16-bit stereo)
 * and writes it to an AudioTrack. The loop below is that thread, with SDL's
 * audio queue in place of the AudioTrack; the queue depth is what paces it.
 * In the Android wrapper SDL's "disk" driver carries the samples on to a real
 * AudioTrack.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>

#include <SDL2/SDL.h>

#include "dyn.h"
#include "port_env.h"
#include "trace.h"

namespace {

std::atomic<bool> g_run{false};
pthread_t g_thread;
jobject g_device = NULL;
JavaVM *g_vm = NULL;

void *audio_main(void *)
{
    JNIEnv *env = NULL;
    g_vm->AttachCurrentThread(&env, NULL);

    auto get_info = (jint (*)(JNIEnv *, jobject, jint))dyn_native("org/fmod/FMODAudioDevice", "fmodGetInfo");
    auto process  = (jint (*)(JNIEnv *, jobject, jobject))dyn_native("org/fmod/FMODAudioDevice", "fmodProcess");
    if (!get_info || !process) {
        trace("fmod: natives not registered - no sound");
        return NULL;
    }

    int rate = get_info(env, g_device, 0);
    int block = get_info(env, g_device, 1);      /* frames per DSP block */
    int blocks = get_info(env, g_device, 2);
    if (rate <= 0 || block <= 0) {
        trace("fmod: bad output format (rate=%d block=%d)", rate, block);
        return NULL;
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        trace("fmod: SDL audio init failed: %s", SDL_GetError());
        return NULL;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = rate;
    want.format = AUDIO_S16LSB;
    want.channels = 2;
    want.samples = (Uint16)(block < 256 ? 256 : block);
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) {
        trace("fmod: SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return NULL;
    }
    SDL_PauseAudioDevice(dev, 0);

    const int bytes = block * 2 * 2;
    void *pcm = calloc(1, (size_t)bytes);
    jobject buffer = env->NewDirectByteBuffer(pcm, bytes);
    /* How much may sit in the queue before this thread waits: latency vs. underruns. */
    const Uint32 high_water = (Uint32)bytes * (Uint32)port_getenv_long("AUDIO_QUEUE_BLOCKS", 3);
    trace("[audio] fmod: %d Hz, block %d frames x %d, driver %s", rate, block, blocks,
          SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");

    while (g_run.load()) {
        if (get_info(env, g_device, 3) != 1) {   /* mixer not running yet / paused */
            SDL_Delay(10);
            continue;
        }
        if (SDL_GetQueuedAudioSize(dev) > high_water) {
            SDL_Delay(2);
            continue;
        }
        process(env, g_device, buffer);
        SDL_QueueAudio(dev, pcm, (Uint32)bytes);
    }
    SDL_CloseAudioDevice(dev);
    free(pcm);
    return NULL;
}

} // namespace

void fmod_audio_register(JNIEnv *env)
{
    env->GetJavaVM(&g_vm);
    const char *F = "org/fmod/FMODAudioDevice";
    dyn_method(F, "start()V", [](JNIEnv *, jobject self, const jvalue *) {
        if (!g_run.exchange(true)) {
            g_device = self;
            pthread_create(&g_thread, NULL, audio_main, NULL);
        }
        return JV();
    });
    dyn_method(F, "stop()V", [](JNIEnv *, jobject, const jvalue *) {
        if (g_run.exchange(false))
            pthread_join(g_thread, NULL);
        return JV();
    });
    dyn_method(F, "close()V", [](JNIEnv *, jobject, const jvalue *) { return JV(); });
    dyn_method(F, "isRunning()Z", [](JNIEnv *, jobject, const jvalue *) { return JZ(g_run.load()); });
    dyn_method(F, "startAudioRecord(III)I", [](JNIEnv *, jobject, const jvalue *) { return JI(0); });
    dyn_method(F, "stopAudioRecord()V", [](JNIEnv *, jobject, const jvalue *) { return JV(); });
}
