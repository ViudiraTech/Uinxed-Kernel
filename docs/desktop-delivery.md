# Debian desktop integration delivery

This PR carries the complete propagation WIP (`c6cb2f9d`, based on `ccd68b38`),
not the older namespace checkpoint. It is based on develop `7698d611` and uses
its architecture directory layout. The VFS propagation engine, mount namespace
owner records and PID namespace creation limit are retained.

## initctl regression

PID 1 had effective/filesystem GID 0 but real/saved GID 1000. systemd 257 checks
its FIFO's owner against `getuid()` and `getgid()`; its correctly created
`/run/initctl` (FIFO, 0600, 0:0) therefore failed the group check with EEXIST.
Initialize all of init's real, effective, saved and filesystem IDs to zero, and
report real/saved IDs correctly in `/proc/<pid>/status`.

A named FIFO also became permanently closed after a temporary metadata reference
was released. Retain its ring until unlink and discard buffered data when its
last endpoint closes. The guest regression covers duplicate mkfifo, repeated
open, last-close data discard and anonymous-pipe buffered data/EOF.

## Evidence

The final PR kernel was built from scratch with the WIP desktop configuration:

```sh
make -B -j4 CONFIG_VIRTIO=y CONFIG_VIRTIO_PCI=y CONFIG_VIRTIO_GPU=y UxImage
```

Its desktop ISO uses the original Debian initramfs, byte for byte. The test used
QEMU q35/KVM, `host,+invtsc`, four vCPUs, 6 GiB RAM and virtio-gpu (2D).
The root shell was reached in 24.30 seconds including serial login automation;
this is not a boot-to-login-prompt-only measurement.

- initctl, lightdm, graphical.target, logind and udevd were active; xfce4-session
  was running. A QEMU screenshot was inspected and showed the rendered XFCE
  panel, desktop icons and dock.
- `initctl-fifo`: 0 failures; initctl socket restart returned 0 and stayed active.
- `mount-namespace`, `mount-propagation`, `user-ipc-isolation`: 0 failures.
- `mount-view-io`: 37 checks, 0 failures.
- `vdso-stress`: 2,000,000 samples, 0 failures. The separate clock probe reported
  100,000 userspace calls and zero syscall fallbacks with invariant TSC enabled.
- Host vDSO fallback and snapshot-anchor regressions passed with ASan/UBSan.
  Build and full static checks are recorded in the local delivery logs.

These mount tests cover their included cases. Peer/slave propagation event
forwarding remains experimental and is not guaranteed correct across all cases.

## Image provenance and limits

The protected base is `build/xfce/xfce-clean.iso`, SHA-256
`153e8f1c66d7e4f8132f50cabc4ca3e21bad22589d8e29edabecc9099d8fc09e`.
The unchanged `initramfs.cpio` SHA-256 is
`28e64ca32e1ea5542de1bcd40839b51cb69a9772c723a431fa5f7d406c65191b`.
The validated PR `UxImage` SHA-256 is
`8f8ef38faddbb735a68ef837d4416780df09e4f1140ddd111a06417d06a65ee4`.
The delivery ISO SHA-256 is
`195d8d8d466e81d7d8dde150dc48c4c3ac0efb26b0ec5db767a9fe90c665724c`.
`scripts/update-desktop-iso.sh` checks both the embedded kernel and base archive.

The delivery target is `build/xfce/xfce_clean.iso`; the prior delivery image is
preserved separately. Never run the Debian rootfs rebuilding script to update
this image. With the kernel built in the PR worktree, use that worktree's
`scripts/update-desktop-iso.sh`: the script takes its own repository's UxImage.

Virgl rendering has not received pixel acceptance. Guest DNS/HTTP has not passed
an end-to-end test. The WIP's intermittent page fault/panic under host memory
pressure remains unresolved; successful boots here are not proof of its absence.
The previously reported systemctl status assertion and GLib fatal traps are not
claimed fixed. No change is made to PR #88.

Raw serial logs, screenshots, image/build verification, ref inventories and the
cleanup recovery bundle are in the main checkout's `build/delivery-20261005/`.
