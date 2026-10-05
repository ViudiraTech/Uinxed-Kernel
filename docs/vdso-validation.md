# vDSO validation on the WIP desktop image

2026-10-05, `codex/propagation-wip-20261005`, baseline `ccd68b38`.

The tested baseline was `build/xfce/xfce_clean.iso`, SHA-256
`6c0db41a9c2367435c6119959e22632d1c3fe7ee91ee1033bb87bf58b4293fa1`.
Its embedded kernel matched the baseline `UxImage` byte for byte.

## Findings

The retry-exhaustion syscall fallback and removal of the 1,000,000-second
monotonic ceiling were already present in this WIP baseline. Keep those fixes:
GNU gnulib's [gethrxtime](https://github.com/coreutils/gnulib/blob/master/lib/gethrxtime.c)
tries CLOCK_REALTIME when CLOCK_MONOTONIC fails, which can mix epochs between
calls. A vDSO fallback must retain the requested clock.

QEMU `-cpu host` did not advertise invariant TSC to this guest. The kernel
therefore selected HPET and published `VDSO_CLOCKMODE_NONE`; every high-resolution
vDSO read entered the kernel. On this host, `-cpu host,+invtsc` advertised the
capability, selected the calibrated TSC clocksource, and restored userspace reads.
This is a local KVM configuration choice, not a reason to use an unsafe TSC when
the guest does not advertise the capability. QEMU discusses the migration
constraints of invariant TSC in its [KVM presentation](https://kvm-forum.qemu.org/2022/Hyper-V%202022.pdf).

A second defect remained even with invariant TSC: the publisher read monotonic
time and then separately sampled `cycle_last`. Preemption between those reads
made the time base older than its cycle anchor. Four threads making 2,000,000
bracketed reads found four timestamps about 2 ms behind the syscall timeline.
The publisher now derives monotonic time and its cycle anchor from one serialized
TSC read, and derives realtime from that same monotonic sample. A deterministic
fixture injecting 5 ms between counter reads reproduces the old 5 ms mismatch
and shows zero mismatch after the change.

## Measurements

QEMU q35/KVM, four vCPUs, 6 GiB RAM, virtio-gpu, the same Debian rootfs and
coreutils 9.7. Latency is measured over 200,000 calls. An errno-tagging seccomp
filter identifies actual syscall fallbacks in a separate 100,000-call phase.

| Configuration | vDSO ns/call | syscall ns/call | fast / syscall fallback |
| --- | ---: | ---: | ---: |
| Original ISO, `host` | 22,235 | 22,115 | 0 / 100,000 |
| Original ISO, `host,+invtsc` | 24.2 | 3,843 | 100,000 / 0 |
| Fixed ISO, `host,+invtsc` | 23.8 | 3,824 | 100,000 / 0 |

The original ISO failed the four-thread stress check 4/2,000,000 times; the fixed
ISO passed with 0/2,000,000 failures. The check permits 1 ms around the syscall
bracket, so this is not proof of nanosecond-perfect cross-CPU ordering.

For `dd if=/dev/zero of=/dev/null bs=512 count=131072 status=progress`:

| Configuration | normal libc clock path | forced clock_gettime syscall |
| --- | ---: | ---: |
| Original ISO, `host` | 21.1–21.3 MB/s | 21.7–21.8 MB/s |
| Original ISO, `host,+invtsc` | 2.2 GB/s | 125 MB/s |
| Fixed ISO, `host,+invtsc` | 2.2 GB/s | 124–128 MB/s |

The forced-syscall control uses an LD_PRELOAD clock_gettime wrapper, retaining the
same kernel, clock ID, buffers and I/O. Each variant ran twice, alternating the
clock path. With `status=none`, bs=512 elapsed times were essentially unchanged
between clock paths (about 37–38 ms on the TSC baseline). This matches
[GNU dd's copy loop](https://github.com/coreutils/coreutils/blob/master/src/dd.c):
`status=progress` reads the clock every block. These are memory/device-node tests,
not disk-throughput measurements. The 1 MiB-block test has only 64 blocks and is
less sensitive to clock-call overhead.

The original report's enormous/epoch-like monotonic value has not been reproduced
in this investigation. The observed 2 ms sampling mismatch is a distinct proven
defect. One unmodified WIP boot with `+invtsc` panicked with #GP during process
exit; a retry completed. The snapshot change does not establish that the WIP's
separate intermittent boot failure is fixed.

## Run and reproduce

The launcher defaults to `host,+invtsc` when the Linux host advertises both
constant and nonstop TSC. Override it with `QEMU_CPU=host` to check the HPET
fallback. It uses local KVM; do not assume this CPU configuration is migratable.

```sh
scripts/run-desktop.sh
# Use the separately generated repaired image:
DESKTOP_ISO="$PWD/build/vdso-investigation-20261005/xfce-vdso-fixed.iso" scripts/run-desktop.sh
```

The original `xfce_clean.iso` was preserved. The repaired image was made by
`scripts/update-desktop-iso.sh`, which verified that the base `initramfs.cpio`
remained unchanged (SHA-256
`28e64ca32e1ea5542de1bcd40839b51cb69a9772c723a431fa5f7d406c65191b`).

```sh
make vdso/vdso.so
cc -O2 -Wall -Wextra -Werror -Iinclude scripts/tests/vdso-regression.c -ldl -o /tmp/vdso-regression
/tmp/vdso-regression ./vdso/vdso.so
python3 scripts/tests/vdso-snapshot.py --baseline ccd68b38
python3 scripts/tests/vdso-snapshot.py
cc -O2 -Wall -Wextra -Werror -Iinclude scripts/tests/vdso-clock.c -ldl -o /tmp/vdso-clock
cc -O2 -pthread scripts/tests/vdso-stress.c -o /tmp/vdso-stress
```

Run the latter two binaries inside the Uinxed guest. The clock probe installs a
seccomp filter only in itself, after latency and correctness measurements. It
reports the actual userspace/syscall path ratio rather than inferring it from
elapsed time. The host regression checks long uptime and tags fallback syscall
results by clock ID, including odd sequence, disabled TSC, malformed nanoseconds,
backwards cycles and stale cycles. The snapshot fixture's `--baseline REF` mode uses the publisher at the specified
Git revision and expects the pre-fix mismatch.

The fixed HPET guest also passed 100,000 bracketed reads and correctly reported
0 fast / 100,000 syscall fallback calls.

Raw transcripts, the deterministic run harness, preload control, build logs and
repaired ISO are under `build/vdso-investigation-20261005/`.

Validation completed: `make -j4 UxImage`, full `make check JOBS=4`, targeted
clang-format checks, ASan/UBSan host fallback regression, UBSan deterministic
snapshot fixture, repaired-ISO kernel/rootfs checks, both clocksource guest probes,
and the 2,000,000-read TSC guest stress check. This records the initial WIP validation; later delivery integration is documented separately.
