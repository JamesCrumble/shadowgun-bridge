/*
 * The Java side of Unity, as far as libunity.so can tell.
 *
 * Handlers for the framework and com.unity3d.player methods the engine calls
 * through JNI, registered with the dynamic layer (dyn.h). Anything not listed
 * here is logged by that layer the first time the engine looks it up.
 */
#pragma once

#include "jni.h"

struct UnityJavaConfig {
    const char *game_dir;
    const char *apk_path;
    const char *obb_path;
    int width;
    int height;
};

void unity_java_init(JNIEnv *env, const UnityJavaConfig &cfg);
jobject unity_java_player(void);    /* com.unity3d.player.UnityPlayer instance */
jobject unity_java_activity(void);  /* the Activity / Context */
jobject unity_java_surface(void);   /* android.view.Surface */
void unity_java_frame(JNIEnv *env, long frame);
