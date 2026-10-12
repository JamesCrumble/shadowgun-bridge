/*
 * The control channel between the Android wrapper (or a test script) and the port.
 *
 * The host appends one command per line to $SHADOWGUN_CONTROL_DIR/commands:
 *
 *     touch <action> <x> <y> <id>    action: 1 down, 2 move, 0 up; pixels of the render surface
 *     key <android keycode> down|up
 *     pause | resume
 *     quit
 *
 * and reads $SHADOWGUN_CONTROL_DIR/status.json ("running" / "stopped").
 */
#pragma once

#include "jni.h"

void control_init(void);
/* Drains pending commands and injects them into the engine. Returns false on "quit". */
bool control_tick(JNIEnv *env, long frame);
void control_shutdown(long frame);
