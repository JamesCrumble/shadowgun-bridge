# How Shadowgun Bridge was built: a step-by-step account

Русская версия: [PORTING_GUIDE.md](PORTING_GUIDE.md).

SHADOWGUN 1.7.0 is a Unity 5.3 game with its code in IL2CPP. It differs from the other projects of this
family in that the game is not started by "its own" library with a known set of entry points but by a
general-purpose engine: Unity looks Java classes up by name itself, loads a second library itself and asks
for the GL functions itself. So this guide is mostly about playing along with an engine that knows nothing
about the port.

Every step has the same shape: what was seen, how the cause was found, what was changed and where. The
current state is in [PROJECT_STATE.md](../PROJECT_STATE.md) (Russian), the summary in
[README.md](../README.md).

Status: the game can be played through — menu, level loading, combat, checkpoints; 60–90 fps on a OnePlus
13s at 720p.

## Why not rebuild for arm64

Checked first, to avoid doing needless work:

- The game was built with **Unity 5.3.8f1**. Android arm64 builds appeared in Unity only in 2017.4/2018.1;
  a 64-bit `libunity.so` for this version does not exist.
- The game's code is **IL2CPP**: an ARM32 `libil2cpp.so` and a version 21 `global-metadata.dat`. There are
  no managed DLLs in the APK.
- The libraries are `armeabi-v7a` only.

## Which APK to start from

`gamebin/` holds two APKs of the same build (the `libunity.so` Build ID matches and the OBB is
byte-identical):

- Anniversary Edition — the dex is plain, no packer. **Used as the base.**
- "Mod Max Gfx" — packed with Qihoo Jiagu, with `libappcloner` and `libsandhook`. It differs from the
  first by 49 bytes in `globalgamemanagers` (quality settings).

Rule: do not take a packed APK apart when a clean one with the same Build ID is available.

## Part 1. The port as `UnityPlayer`

The frame was copied from Mass Effect Infiltrator Bridge. What belongs to the game was replaced.

### 1.1. Call order

- **How it was found:** the order comes from the game's `classes.dex`. `dexdump -d` produces the dump and
  `tools/dexclass.py` prints the class in question.
- **Result:** [main.cpp](../src/main.cpp) plays the part of the Java class `UnityPlayer`. It loads
  `libunity.so` (and `libmain.so` through DT_NEEDED), calls `JNI_OnLoad`, then the methods the engine
  registered itself through `RegisterNatives`: `initJni`, `nativeFile(apk)`, `nativeFile(obb)`,
  `nativeInitWWW`, `nativeRecreateGfxState`, `nativeResume`, `nativeFocusChanged`, and after that
  `nativeRender` every frame.
- **Difference from the Gameloft games:** the entry points are not exported under `Java_…` names but
  registered at run time. Their addresses have to be recorded in `RegisterNatives`.

### 1.2. Dynamic JNI

- **Seen:** Unity and the C# code reach for dozens of Android classes, and the list cannot be drawn up in
  advance.
- **Solution:** [dyn.cpp](../jni/dyn.cpp) creates unknown classes, methods and fields on the fly. A call
  goes to a handler keyed by `class.name+signature` (or `*`). With no handler, the log gets a
  `JNI unhandled …` line and the call returns 0 or null.
- **Why this way:** the game does not fault on the first unknown method, and the log turns into the list
  of what is left to do.
- **Subtlety:** the lookup is loose on the return type. That is how C# finds methods through
  `ReflectionHelper`.
- **Handlers** are in [unity_java.cpp](../jni/unity_java.cpp): `Context`, `File`, `SharedPreferences`
  (these are `PlayerPrefs`, kept in `files/playerprefs.txt`), `Build`, `Display`, `PackageManager`,
  `ReflectionHelper`, Madfinger's permissions plugin.

### 1.3. The second library

- **Seen:** Unity calls `dlopen("il2cpp")`.
- **Fix:** `dlopen`/`dlsym` in [misc.cpp](../thunks/libc/misc.cpp) load real modules with the port's
  loader instead of returning a stub.

### 1.4. Data layout

The engine reads its assets straight out of the zip, so the game's APK sits next to it as `base.apk`, and
the OBB at its usual path:
`Android/obb/com.madfingergames.shadowgun/main.170200014.com.madfingergames.shadowgun.obb`.

### 1.5. Network, audio, input

- **`UnityEngine.WWW`** — [unity_www.cpp](../jni/unity_www.cpp): `jar:file://` and `file://` addresses are
  read for real, network ones fail at once.
- **Audio** — [fmod_audio.cpp](../jni/fmod_audio.cpp): the `fmodProcess` loop writes to a queue, then a
  FIFO and an `AudioTrack` on the Java side.
- **Input** — [control.cpp](../src/control.cpp): the commands `touch`, `key`, `pause`, `resume`, `quit`
  from `ctl/commands` become `nativeInjectEvent` calls with fake `MotionEvent` and `KeyEvent` objects.
- **EGL** — [egl_shim.cpp](../android/egl_shim.cpp): one RGBA8888 + D24S8 config, **ES2 only**. An ES3
  context is refused so that Unity takes its ES2 path.

## Part 2. What stood between start-up and the menu

Found on the PC (the host's qemu-arm with Mesa llvmpipe: slow, but it catches faults). In order of
appearance:

### 2.1. 64-bit division returned garbage

- **Cause:** `__aeabi_ldivmod` and its neighbours in the generated thunk table returned a struct through a
  hidden pointer, while the ABI returns the result in registers.
- **Fix:** the functions are bound to the libgcc implementations —
  [symtab_unity.cpp](../src/symtab_unity.cpp).

### 2.2. il2cpp's garbage collector

- **Cause:** `pthread_getattr_np`, `pthread_attr_getstack` and signals went straight to glibc, while the
  structures (`sigset_t`, `sigaction`, thread attributes) have a different layout in bionic. The collector
  uses them to find stack bounds and to stop threads.
- **Fix:** bridges in [symtab_signal.cpp](../src/symtab_signal.cpp) and
  [symtab_pthread.cpp](../src/symtab_pthread.cpp).

### 2.3. `PopLocalFrame` returned NULL

- **Cause:** Unity returns every object from its JNI wrappers through `PopLocalFrame`. A stub that answers
  NULL nulls every result.
- **Fix:** the function returns the object it was given — [jni.cpp](../jni/jni.cpp).

### 2.4. LOADING forever

- **Seen:** the game sits on the loading screen with no errors.
- **Cause:** the game waits for an answer from the Google Play Games plugin. The answer does not come as a
  return value but as a `UnitySendMessage` named `_OnResolvePlayServicesAvailability`.
- **Fix:** the port sends the answer "false" itself — [unity_java.cpp](../jni/unity_java.cpp).
- **Takeaway:** in Unity some of Java's answers arrive asynchronously, as messages. When the game waits
  without an error, look for the message it has not received.

### 2.5. `GetObjectClass` on a class

- **Cause:** the C# code calls `GetObjectClass` passing a `jclass`.
- **Fix:** `dyn_class_of()` in [dyn.cpp](../jni/dyn.cpp).

### 2.6. GL functions

Unity asks for its GL functions through `dlsym` instead of importing them. The list was taken with a trace
(`SHADOWGUN_DLSYM_TRACE=all`): 103 names, all present in the bridge.

## Part 3. Speed

The phone's display is 120 Hz with `eglSwapInterval(1)`, so the frame rate takes the values 120, 60, 40, 30.

### 3.1. The 30 fps cap

The game holds itself at 30 fps. Lifted in [fps_unlock.cpp](../src/fps_unlock.cpp); the target is set with
`SHADOWGUN_TARGET_FPS`.

### 3.2. Where the time goes

The bottleneck is one thread, `UnityMain`: ~17 ms of CPU per frame, 70–75 % busy. The GL bridge is 5–8 %
busy.

Tools:

| What | With |
|---|---|
| profile of the game's code by thread | `SHADOWGUN_PC_PROF=1` ([pc_prof.cpp](../src/pc_prof.cpp)), parsed by `tools/pcprof.py` |
| system calls | [syscall_prof.cpp](../src/syscall_prof.cpp) |
| qemu profile | `simpleperf record --app <package> -p <qemu pid> -e cpu-clock` |
| thread positions | `tools/thread_dump.sh` (SIGUSR2) |

Three notes about `simpleperf`, each of which cost time: it does not work without `--app` or with the
`cpu-cycles` event; recording works only with `-p <pid>`, `-t <tid>` gives 0 samples; `libqemu` on the phone
has no symbols, so addresses are resolved with `nm -n` on the local build.

Measurements mean something only with the screen on and the game in the foreground (`dumpsys power`,
`dumpsys activity`). One measurement was taken with the screen off and gave a false result.

### 3.3. qemu

Test scene: the start of the game, 341 draws, ~300 thousand GL commands per second.

| Build | fps |
|---|---|
| original | 33.6 (17–22 under the frequency clamp) |
| NZCV flags in generated code, fast `vfp_cmps` | 38.5 |
| VCMP in generated code, hardening flags removed from the qemu build | 40.0 |
| block chaining across a page boundary | 41.4–42.2 |

Then two large changes that later went to the other projects as "variant F":

- **Scalar VFP in generated code.** `add/sub/mul/div` are emitted inline instead of a function call.
  `UnityMain` load 78 % → ~66 %.
- **NEON f32 on a whole register.** In the "room with crates" location: 47–54 → 64–70 fps, confirmed by
  the user ("65").

Details and test hashes are in [qemu/README.md](../qemu/README.md).

What **did not work**, recorded so it is not repeated:

- A target cache per indirect-branch site: calls became 18 % slower on the bench. Branch sites in engine
  code are polymorphic and a miss costs more than the ordinary lookup. The code was removed.
- Probing the jump cache in generated code (`SHADOWGUN_INLINE_LOOKUP`): no gain.
- The atomics patch from N.O.V.A. 3: one occurrence in `libil2cpp`, none in `libunity`.
- The matrix patch from Mass Effect: in Shadowgun that code is NEON already.

The `armbench.c` bench must be built with `-marm`: the compiler defaults to Thumb-2 while the game runs in
ARM mode, and the translator's paths for the two differ.

### 3.4. Unity's multithreaded rendering

- **What was done:** one settings byte is patched in a copy of `base.apk` — `m_MobileMTRendering` in
  `globalgamemanagers` (a format without a type tree, parsed by `tools/unity_objs.py`). A
  `UnityGfxDeviceWorker` thread appears.
- **Result:** no difference in scenes that sit at 60; in a heavy scene 50.5 → 56–57.5 fps. On by default.
- **Lesson:** the first measurement said "no gain" because the scene was vsync-bound. Measure where the
  CPU is short.

### 3.5. Cores and frequency

- Pinning qemu to cores 6–7 (`CPU_PRIME`) gives 10–15 % and is on by default.
- The frame rate follows the frequency ceiling of those cores exactly: 1209 MHz — 46 fps, 1690–1960 MHz —
  64–66, 4320 MHz — 80–90. The phone is cold all the while. The early conclusion "frequency matters
  little" was wrong.
- Did not help: turning ADPF off, an ADPF report of a frame twice as long, game mode `performance`,
  OnePlus's Games app.

## Part 4. Shadows

- **Seen:** 16 built-in shaders fail to compile; a shadow in the level is solid black with ragged edges.
- **Cause:** Unity treats Adreno as an ES3 device and uses `sampler2DShadow` under `#version 100`, counting
  on the `GL_EXT_shadow_samplers` extension. The Adreno 830 driver does not advertise it.
- **Fix:** the bridge's Java side replaces `sampler2DShadow` with an ordinary depth fetch compared in the
  shader — `shadowSamplers` in
  [GlBridge.java](../apk/src/com/jamescrumble/shadowgun/GlBridge.java).
- **Result:** 0 shader compile errors (was 16).

## Part 5. Carrying saves over

Changing the package name makes the app a different one to Android. A build under the old name exports the
saves to a `migrate` folder, and the new app picks it up at start —
[SaveMigration.java](../apk/src/com/jamescrumble/shadowgun/SaveMigration.java). The game's progress is in
`game/files/playerprefs.txt`.

Files copied with `adb shell cp` get the owner `shell` and mode 660: the app cannot open them without a
`chmod`.

## What is left

- The intro video `introHD.mp4` is not played.
- The system's frequency limiting (3.5).
- `Worker Thread` calls `nanosleep(0)` about 60 times a frame when idle; the effect has not been checked.
- The "Max Gfx" switch (49 bytes of quality settings) has not been made.
