# Alpine/Weston initramfs

`scripts/build-alpine-xfce-initramfs.sh` builds an Alpine/Weston userspace that can
be unpacked by Uinxed-Kernel and started through `/sbin/init` (OpenRC).  The
archive is an uncompressed `newc` cpio because the kernel already accepts that
format directly and the module name is derived from the basename `initramfs`.

The image uses only the Weston stack for the desktop: OpenRC, eudev, D-Bus,
Weston, its DRM backend, Xwayland, `seatd` and fonts.  Xfce, the Xorg display
server, `fbdev`, `startxfce4` and the old Xorg launch/configuration files are
not included.
`seatd` is enabled before Weston so libseat can hand it the DRM device and
input seat.

The builder explicitly enables `bootmisc` in the OpenRC `boot` runlevel.  The
serial console uses the explicitly installed `/sbin/agetty`, so a failed
Weston start still leaves a recovery shell path on `ttyS0`.

Weston’s log is written directly to `/dev/ttyS0`, and its standard output and
error streams inherit the same serial sink.  The desktop shell autolaunches
`weston-terminal` so the image has an immediately visible Wayland client.

## Build

The builder must run as root on an x86_64 Alpine-compatible host because it
executes Alpine's native `/sbin/apk` in a chroot.  It does not start QEMU.

To build only the initramfs (the default release is resolved from Alpine's
`latest-stable` directory):

```sh
sudo ./scripts/build-alpine-xfce-initramfs.sh \
  --output build/alpine-xfce/initramfs.cpio
```

To select a release, pass environment variables or options:

```sh
sudo ALPINE_BRANCH=latest-stable ALPINE_VERSION=3.24.1 \
  ./scripts/build-alpine-xfce-initramfs.sh
```

Weston is fixed to the DRM/KMS backend with the Pixman CPU software renderer.
There is no GL renderer or `fbdev` fallback.  Xwayland is enabled in
`weston.ini` and is launched by Weston for X11 applications.

An existing Alpine minirootfs can be supplied with `--rootfs PATH`; package
installation is skipped, but the OpenRC runlevels and root Weston settings are
still applied.  The builder does not inspect or purge packages in an existing
tree; use a clean minirootfs when the image must contain no legacy desktop
packages.

## Build and embed an ISO

The normal `make all` target remains offline and does not download a rootfs.
The opt-in target builds the archive and embeds it in a staging ISO.  The
target appends this Limine entry only to the temporary staging configuration:

```text
module_path: boot():/boot/initramfs.cpio
```

Run:

```sh
sudo make alpine-xfce-iso
```

The result is `Uinxed-x64.iso`.  `make alpine-initramfs` is available when the
archive is needed without an ISO.  `ALPINE_MIRROR`, `ALPINE_BRANCH`,
`ALPINE_VERSION` and `ALPINE_ARCH` are forwarded as ordinary environment
overrides.

The generated `etc/xdg/weston/weston.ini` selects the desktop shell,
autolaunches the terminal and enables Xwayland.  Pixman still requires a
working `/dev/dri/card0` because DRM/KMS drives the display; it does not fall
back to `/dev/fb0`.
