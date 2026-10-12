# qemu-arm for the Android wrapper

The APK runs the ARM32 game under our own static aarch64 `qemu-arm` (linux-user), built from
QEMU 11.1.2 with `nova3-qemu-11.1.2.patch`. The patch, all of it Android/Oryon-specific:

- **Fast host FP**: VFP add/sub/mul/div/fma and float<->int conversions as single AArch64
  instructions (same IEEE and NaN rules as ARMv7); only the cumulative FPSCR flags are not produced.
  `QEMU_STRICT_FP=1` turns it off.
- **Neon f32 on the host FPU** (vadd/vsub/vmul/vmla/vmls, also by scalar) under FPCR FZ+DN, the
  AArch32 "standard FPSCR". The mode is entered lazily and left in place: writing FPCR serialises
  Oryon, and switching per helper call made Neon 6.7x slower.
- **Fast A32 indirect-branch lookup** (`helper_arm_lookup_tb_ptr`, `NOVA3_FAST_LOOKUP`): flags built
  inline, generic path only on a jump-cache miss. Calls/returns 1.7x faster on the bench.
- Jump cache 4096 -> 65536 entries (`NOVA3_JMP_BITS=16`).
- `cpsr_write` does not rebuild hflags for NZCV/Q/GE writes.
- `realpath()` in guest `open` only for paths mentioning "proc" (glibc's realpath reaches the
  faccessat2 syscall, which Android's app seccomp filter kills the process for).
- `QEMU_SYSCALL_STATS=1`: guest syscall counts every 10 s and sampled open/access paths on stderr.

`android/patch_syscalls.py` additionally rewrites set_robust_list/rseq/faccessat2 call sites in the
static glibc for the seccomp filter.

## Build (Docker, from the WSL host)

```sh
docker build -t nova3-qemu-build android/qemu
# qemu source: download.qemu.org/qemu-11.1.2.tar.xz unpacked to <work>/qemu-src/qemu-11.1.2,
# patched with: patch -p1 < android/qemu/nova3-qemu-11.1.2.patch
docker run --rm -u 1000:1000 -e HOME=/tmp -v <work>:/work nova3-qemu-build bash /work/qemu-build/build_variant.sh D \
  '-DNOVA3_JMP_BITS=16 -DNOVA3_FAST_LOOKUP -O3 -march=armv8.2-a+lse+rcpc+dotprod -mtune=neoverse-v1'
```

Do not add SVE to `-march`: Oryon has none (SIGILL).

## Tests

- `fptest.c` (armhf static): hash of VFP/Neon results over denormals, NaNs, infinities; must match with
  and without `QEMU_STRICT_FP=1` and Debian's qemu 7.2.
- `armbench.c`: engine-shaped workloads (calls/vtables, VFP, Neon mat*vec, qsort, memcpy) to compare
  builds on the phone from `/data/local/tmp`.

## Shadowgun-specific changes (qemu/patches/shadowgun_fast_vfp.patch)

Diff of qemu-src vs nova3probe's qemu-src (11.1.2). Test scene: 33.6 fps -> ~42 fps.
- translate.c: inline NZCV(Q) writes in gen_set_cpsr (no cpsr_write helper); optional inline
  jump-cache probe for indirect jumps (env SHADOWGUN_INLINE_LOOKUP=1, off: no gain).
- helper-defs.h: vfp_* helpers are TCG_CALL_NO_RWG (no guest register spill around scalar VFP).
- vfp_helper.c / translate-vfp.c: fast VCMP/VCMPE (inline TCG, off with QEMU_STRICT_FP=1).
- translator.c: translator_use_goto_tb allows cross-page direct chaining (user-only semantics).
- include/fpu/softfloat.h: nova3_fpcr_default() is empty under NOVA3_NEON_NOFPCR (that variant never leaves FPCR
  switched); the thread-local read was the first instruction of every scalar VFP helper. armbench vfp 225 -> 216 ms.
- Inline scalar VFP (tcg-opc.h, tcg.c, tcg-op.c, tcg/aarch64, cpu.h, hflags.c, op_helper.c, translate*.c): new TCG ops
  hfadd/hfsub/hfmul/hfdiv (I32 = single, I64 = double; aarch64 host only, V30 reserved) emit fmov/op/fmov instead of a
  helper call for VADD/VSUB/VMUL/VDIV/VMLA/VMLS/VNMUL.. in translate-vfp.c. Used only in TBs translated with the new
  dynamic flag TBFLAG_A32.FASTFP (FPSCR: round to nearest, no FZ, no DN), computed in hflags.c and in the fast lookup
  in op_helper.c. Off with QEMU_STRICT_FP=1 or SHADOWGUN_INLINE_FP=0. armbench vfp 200 -> 160 ms; fptest/cmptest
  hashes equal to strict and to the previous build.
- vec_helper.c: Neon f32 add/sub/mul/mla/mls (vector and by-scalar) on a whole 128-bit register with host Neon when all
  four lanes pass the neon_f32_host() rule (operands plain, results settled), else the per-element loop; vceq/vcge/vcgt/
  vmax/vmin on the host when both operands are plain in every lane. armbench neon 48 -> 28 ms. Test: `neoncmptest.c`
  (compares and min/max, hash equal to QEMU_STRICT_FP=1 and to the previous build).
- cpu.h: arm_a32_fastfp() reads the FPCR control bits (one load) - the bitfield version cost ~8% on armbench calls.
- meson.build: hardening flags (-ftrivial-auto-var-init, -fzero-call-used-regs) removed.
Tests: cmptest.c hash d6137e57, fptest hash 94e3b209 (fast == strict == Debian qemu 7.2).
