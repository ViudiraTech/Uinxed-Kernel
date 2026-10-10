# systemd shutdown and kernel-thread identification

systemd 257 filters kernel threads before its final signal-and-wait pass. Its
`pid_is_kernel_thread()` reads field 9 (`flags`) of `/proc/<pid>/stat` and checks
the Linux `PF_KTHREAD` bit, `0x00200000`:

- [process-util.c](https://github.com/systemd/systemd/blob/v257/src/basic/process-util.c)
- [killall.c](https://github.com/systemd/systemd/blob/v257/src/shared/killall.c)

Uinxed already sets that bit when creating a kernel thread, and those threads
ignore userspace signals. Procfs nevertheless emitted a literal zero for the
flags field. Consequently systemd classified persistent workers as userspace
processes, sent signals, and waited for workers that would never exit in response.

Procfs now emits the task flags in the existing field, retaining all 52 stat
fields, and exposes the corresponding `Kthread: 0/1` status entry. Kernel-thread
signal dispositions and lifecycle are unchanged.

## Regression and image verification

Build and run the guest probe in the initial PID namespace:

```sh
cc -O2 -Wall -Wextra -Werror scripts/tests/proc-kthread.c -o proc-kthread
./proc-kthread
systemctl poweroff
```

The probe checks PID 1 and itself as userspace, PID 2 (`kthreadd`) and persistent
subsystem workers as kernel threads, all 52 stat fields, the status flag, and
empty kernel-thread command lines. QEMU q35/KVM with four vCPUs, 6 GiB RAM,
`host,+invtsc`, and VirtIO GPU reported five workers and zero probe failures.
`graphical.target`, LightDM, and `xfce4-session` were active before shutdown.
The first poweroff test reached the kernel's power-off syscall and exited QEMU
in 1.60 seconds after `systemctl poweroff`, with no kernel-worker wait message.
An independent `systemctl reboot` run also passed the probe and reached the
reboot syscall in 1.60 seconds; QEMU used `-no-reboot` to observe the reset as a
process exit. This verifies the reset request, not a subsequent second boot.

The desktop kernel was built with:

```sh
make -j4 CONFIG_VIRTIO=y CONFIG_VIRTIO_PCI=y CONFIG_VIRTIO_GPU=y UxImage
```

Kernel SHA-256:
`db3f91b8e77c376c892ab84e087e311724a8207ee8a0fb231844bc42b9400e4d`.
New `build/xfce/xfce_clean.iso` SHA-256:
`32e2ad1585af4eb4368c7c56413bb2cfe7466277d589e15a3fcb2069a69cae2d`.
The packaging script verified the embedded kernel and preserved the Debian
`initramfs.cpio` byte for byte (SHA-256
`28e64ca32e1ea5542de1bcd40839b51cb69a9772c723a431fa5f7d406c65191b`).
The previous ISO is retained as
`build/xfce/xfce_clean-before-shutdown-fix-20261010.iso`.

## Limits

Transient `Failed to get cgroup path ... No data available` warnings from exiting
processes remain visible; they did not block the verified shutdown. An earlier
4-GiB baseline run of the previous image faulted in `process_exit` before login,
so that run did not reproduce the original shutdown wait. The existing WIP
runtime instability is not claimed fixed by this procfs change.

Full local default clang-tidy and the existing vDSO snapshot and VFS child-reclaim
host regressions passed. Local raw logs are in the managed PR worktree's
`build/shutdown-validation/` directory. This change does not merge a PR or write
to either upstream `develop` or the PR 94 branch.
