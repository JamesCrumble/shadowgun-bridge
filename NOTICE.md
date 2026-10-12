# Third-party code and licensing

Shadowgun Bridge is released under **GPL-3.0** (see `LICENSE`). It carries code from
several upstream projects; this file records where each part came from, because
the licences differ and the originals must keep their attribution.

The Shadowgun port itself - the `UnityPlayer` replacement, the dynamic JNI, the GLES bridge,
the Android wrapper and the qemu patches - was written for this project.

The loader glue, JNI layer, libc thunks and Android API stubs it started from were taken from
the Mass Effect Infiltrator port for PortMaster by **EapRules** (GPL-3.0) and adapted. EapRules
did not make, and is not associated with, the Shadowgun port.

## Where the code comes from

| Part | Origin | Licence |
|---|---|---|
| `loader/` - bionic ELF loader, relocations, symbol hooking | [gmloader-next](https://github.com/JohnnyonFlame/gmloader-next) by JohnnyonFlame, itself derived from the Vita so-loader by **Andy Nguyen** | GPL (see note) |
| `thunks/libc/` - bionic to glibc thunks | gmloader-next | GPL |
| `thunks/libc/time64.cpp`, `thunk_time64.h` | `y2038` by **Michael G Schwern** | MIT / Artistic |
| `thunks/libc/fortify.cpp` | The Android Open Source Project (parts (c) Regents of the University of California) | Apache-2.0 / BSD |
| `jni/jni.h` | The Android Open Source Project | Apache-2.0 |
| `loader/leb128.h` | Free Software Foundation (binutils) | GPL |
| `thunks/khronos/` | glad generator, Khronos headers | MIT / Apache-2.0 |
| `third_party/powervr/PVRTDecompress.*` | [PowerVR SDK](https://github.com/powervr-graphics/Native_SDK) by Imagination Technologies | MIT |
| base of `android/`, `jni/`, `src/symtab*`, `src/crash.cpp`, `harness/` | Mass Effect Infiltrator port for PortMaster by **EapRules**, adapted | GPL-3.0 |
| `src/main.cpp`, `jni/dyn.*`, `src/gl_bridge.cpp`, `src/pc_prof.cpp`, `apk/`, `qemu/`, the rest of `src/` and `tools/` | written for this project | GPL-3.0 (qemu patches: GPL-2.0-or-later, as QEMU) |

`third_party/gmloader`, `third_party/masseffect-vita` and `third_party/vfpvector` hold only the licence
texts of projects whose code the inherited base drew on.

## Note on the GPL version

Upstream is not self-consistent, so this is worth stating plainly rather than
leaving for someone to trip over:

- The gmloader-next README says the project is released under **GPLv2**.
- But `loader/so_util.cpp` carries Andy Nguyen's original header, which says
  **GPLv3**, and the bundled licence text includes the customary *"either
  version 2 of the License, or (at your option) any later version"*.

GPL-3.0 is the version that satisfies both readings, so that is what this
repository uses. `loader/LICENSE-gmloader.md` is kept verbatim as it was
received. If JohnnyonFlame states that gmloader-next is GPLv2-**only**, this
repository will relicense to match — open an issue and it will be corrected.

## What is *not* in here

No game code, assets, or data from SHADOWGUN are distributed by this project. The supported
build is Madfinger Games' v1.7.0 for Android (armeabi-v7a); you supply your own copy. The port
loads it at runtime and circumvents no protection.

The launcher icon of a built APK is the game's own icon, read from the builder's copy of the game
at build time; it is not stored in this repository.
