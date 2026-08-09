#!/usr/bin/env bash
# Build an Alpine Weston initramfs for Uinxed and wire it into Limine.

set -Eeuo pipefail

ALPINE_VERSION="${ALPINE_VERSION:-3.24.1}"
ALPINE_BRANCH="${ALPINE_BRANCH:-v${ALPINE_VERSION%.*}}"
ALPINE_MIRROR="${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}"
EXTRA_PACKAGES="${EXTRA_PACKAGES:-}"
BUILD_ISO=1
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
SCRIPT_PATH="$SCRIPT_DIR/$(basename -- "$0")"

usage() {
    printf '%s\n' \
        "Usage: $0 [--no-iso]" \
        "Build Alpine Weston as assets/Limine/initramfs.cpio." \
        "The script uses sudo automatically when required." \
        "Environment: ALPINE_VERSION, ALPINE_BRANCH, ALPINE_MIRROR, EXTRA_PACKAGES"
}

while (($#)); do
    case "$1" in
        --no-iso) BUILD_ISO=0 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
LIMINE_DIR="$PROJECT_ROOT/assets/Limine"
LIMINE_CONFIG="$LIMINE_DIR/Limine/limine.conf"
OUTPUT_IMAGE="$LIMINE_DIR/initramfs.cpio"
LEGACY_COMPRESSED_IMAGE="$LIMINE_DIR/initramfs.cpio.gz"

if [[ ! -f "$PROJECT_ROOT/Makefile" || ! -f "$LIMINE_CONFIG" ]]; then
    printf 'Run this script from an intact Uinxed-Kernel checkout.\n' >&2
    exit 1
fi

if ((EUID != 0)); then
    command -v sudo >/dev/null 2>&1 || { printf 'sudo is required to build the rootfs.\n' >&2; exit 1; }
    sudo_args=()
    ((BUILD_ISO)) || sudo_args+=(--no-iso)
    exec sudo --preserve-env=ALPINE_VERSION,ALPINE_BRANCH,ALPINE_MIRROR,EXTRA_PACKAGES -- /bin/bash "$SCRIPT_PATH" "${sudo_args[@]}"
fi

for command_name in awk chroot cpio curl find mktemp sha256sum sort tar; do
    command -v "$command_name" >/dev/null 2>&1 || { printf 'Missing host command: %s\n' "$command_name" >&2; exit 1; }
done

WORK_DIR="$(mktemp -d /tmp/uinxed-alpine.XXXXXX)"
ROOTFS="$WORK_DIR/rootfs"
ARCHIVE_NAME="alpine-minirootfs-${ALPINE_VERSION}-x86_64.tar.gz"
ARCHIVE="$WORK_DIR/$ARCHIVE_NAME"
ARCHIVE_URL="$ALPINE_MIRROR/$ALPINE_BRANCH/releases/x86_64/$ARCHIVE_NAME"
cleanup() { [[ -d "${WORK_DIR:-}" && "$WORK_DIR" == /tmp/uinxed-alpine.* ]] && rm -rf -- "$WORK_DIR"; }
trap cleanup EXIT INT TERM

printf '[1/7] Downloading %s\n' "$ARCHIVE_URL"
curl --fail --location --retry 3 --output "$ARCHIVE" "$ARCHIVE_URL"
curl --fail --location --retry 3 --output "$ARCHIVE.sha256" "$ARCHIVE_URL.sha256"
(cd "$WORK_DIR" && sha256sum --check "$ARCHIVE_NAME.sha256")

printf '[2/7] Extracting Alpine minirootfs\n'
mkdir -p "$ROOTFS"
tar --extract --gzip --file "$ARCHIVE" --directory "$ROOTFS"
install -Dm644 /etc/resolv.conf "$ROOTFS/etc/resolv.conf"
printf '%s\n%s\n' "$ALPINE_MIRROR/$ALPINE_BRANCH/main" "$ALPINE_MIRROR/$ALPINE_BRANCH/community" >"$ROOTFS/etc/apk/repositories"

printf '[3/7] Installing Weston desktop, Xwayland, applications, and OpenRC\n'
chroot "$ROOTFS" /bin/sh -eux <<CHROOT_SETUP
apk update
apk add alpine-base openrc eudev udev-init-scripts dbus \
    mesa-dri-gallium mesa-egl mesa-gbm mesa-gl \
    weston weston-backend-drm weston-shell-desktop weston-terminal weston-clients weston-xwayland xwayland \
    seatd \
    font-dejavu \
    libinput-tools evtest bash \
    fastfetch htop nano less file \
    clang gcc musl-dev binutils make coreutils ${EXTRA_PACKAGES}
addgroup -S input 2>/dev/null || true
addgroup -S video 2>/dev/null || true
for service in udev udev-trigger; do rc-update add "\$service" sysinit; done
rc-update del elogind default 2>/dev/null || true
rm -rf /var/cache/apk/*
CHROOT_SETUP

printf '[4/7] Configuring automatic root Weston desktop and input permissions\n'
install -d -m755 "$ROOTFS/etc/udev/rules.d" "$ROOTFS/etc/xdg/weston" "$ROOTFS/usr/local/bin" "$ROOTFS/usr/local/sbin"
cat >"$ROOTFS/etc/fstab" <<'FSTAB'
proc /proc proc defaults 0 0
sysfs /sys sysfs defaults 0 0
tmpfs /run tmpfs mode=0755,nosuid,nodev 0 0
FSTAB
cat >"$ROOTFS/etc/udev/rules.d/99-uinxed-input.rules" <<'UDEV_RULES'
ACTION!="remove", SUBSYSTEM=="input", KERNEL=="event[0-9]*", ENV{ID_INPUT}="1", MODE="0660", GROUP="input", TAG+="seat"
ACTION!="remove", SUBSYSTEM=="input", KERNEL=="event[0-9]*", ATTRS{name}=="AT Translated Set 2 keyboard", ENV{ID_INPUT_KEYBOARD}="1"
ACTION!="remove", SUBSYSTEM=="input", KERNEL=="event[0-9]*", ATTRS{name}=="PS/2 Generic Mouse", ENV{ID_INPUT_MOUSE}="1"
ACTION!="remove", SUBSYSTEM=="drm", KERNEL=="card[0-9]*", ENV{ID_SEAT}="seat0", MODE="0660", GROUP="video", TAG+="seat"
UDEV_RULES
cat >"$ROOTFS/etc/xdg/weston/weston.ini" <<'WESTON_INI'
[core]
shell=desktop-shell.so
renderer=pixman
idle-time=0
require-input=false
xwayland=true

[shell]
locking=false
panel-position=top
background-image=/usr/share/weston/background.png
background-type=scale-crop

[launcher]
icon=/usr/share/weston/icon_terminal.png
path=/usr/bin/weston-terminal

[launcher]
icon=/usr/share/weston/icon_terminal.png
path=/usr/local/sbin/start-fastfetch

[terminal]
font=DejaVu Sans Mono
font-size=16

[xwayland]
path=/usr/local/sbin/Xwayland-software
WESTON_INI
cat >"$ROOTFS/usr/local/sbin/start-fastfetch" <<'START_FASTFETCH'
#!/bin/sh
exec /usr/bin/weston-terminal --shell=/usr/local/sbin/fastfetch-shell
START_FASTFETCH
cat >"$ROOTFS/usr/local/sbin/fastfetch-shell" <<'FASTFETCH_SHELL'
#!/bin/sh
/usr/bin/fastfetch
exec /bin/bash
FASTFETCH_SHELL
cat >"$ROOTFS/usr/local/sbin/Xwayland-software" <<'START_XWAYLAND'
#!/bin/sh
# Uinxed's DRM implementation intentionally exposes the dumb-buffer/pixman
# path used by Weston, not a complete GBM acceleration stack.  Make Xwayland
# use wl_shm buffers instead of probing glamor/DRI3.
export XWAYLAND_NO_GLAMOR=1
exec /usr/bin/Xwayland "$@" -shm -verbose 3
START_XWAYLAND
cat >"$ROOTFS/usr/local/sbin/start-weston-root" <<'START_WESTON'
#!/bin/sh
export HOME=/root USER=root LOGNAME=root
exec </dev/tty1 >/dev/ttyS0 2>&1
install -d -m700 /run/user/0
export XDG_RUNTIME_DIR=/run/user/0
export XDG_CONFIG_HOME=/root/.config
export XDG_VTNR=1
unset DISPLAY XAUTHORITY
export WAYLAND_DISPLAY=wayland-0
export XDG_SESSION_TYPE=wayland

# Alpine's libseat package does not include the builtin backend.  Start the
# packaged seatd daemon explicitly before Weston and use its Unix socket.
if [ ! -e /run/seatd.sock ]; then
    /usr/bin/seatd -g video >/dev/null 2>&1 &
    sleep 1
fi
export LIBSEAT_BACKEND=seatd
export SEATD_SOCK=/run/seatd.sock
# Disabling VT binding avoids depending on Linux VT switching that this
# kernel does not implement completely.
export SEATD_VTBOUND=0

exec /usr/bin/weston -B drm --renderer=pixman --seat=seat0 \
    --continue-without-input --config=/etc/xdg/weston/weston.ini \
    --log=/dev/ttyS0
START_WESTON
chmod 0755 "$ROOTFS/usr/local/sbin/start-weston-root" "$ROOTFS/usr/local/sbin/start-fastfetch" \
    "$ROOTFS/usr/local/sbin/fastfetch-shell" "$ROOTFS/usr/local/sbin/Xwayland-software"

if grep -q '^tty1::respawn:' "$ROOTFS/etc/inittab"; then
    sed -i 's#^tty1::respawn:.*#tty1::respawn:/sbin/getty -n -l /usr/local/sbin/start-weston-root 38400 tty1#' "$ROOTFS/etc/inittab"
else
    printf '%s\n' 'tty1::respawn:/sbin/getty -n -l /usr/local/sbin/start-weston-root 38400 tty1' >>"$ROOTFS/etc/inittab"
fi
if grep -q '^#\?ttyS0::respawn:' "$ROOTFS/etc/inittab"; then
    sed -i 's|^#\?ttyS0::respawn:.*|# ttyS0 is reserved for kernel and Weston logs.|' "$ROOTFS/etc/inittab"
fi
cat >"$ROOTFS/etc/motd" <<'MOTD'
Uinxed Alpine Weston rootfs

Weston starts automatically as root on tty1 with its DRM desktop shell.
MOTD

printf '[5/7] Breaking hard links for the Uinxed cpio loader\n'
find "$ROOTFS" -xdev -type f -links +1 -print0 >"$WORK_DIR/hardlinks"
while IFS= read -r -d '' linked_file; do
    replacement="$linked_file.uinxed-copy"
    cp --preserve=mode,ownership,timestamps --reflink=auto -- "$linked_file" "$replacement"
    mv -- "$replacement" "$linked_file"
done <"$WORK_DIR/hardlinks"

printf '[6/7] Packing %s\n' "$OUTPUT_IMAGE"
mkdir -p "$LIMINE_DIR"
TEMP_IMAGE="$WORK_DIR/initramfs.cpio"
(cd "$ROOTFS" && find . -xdev \( -path ./dev -o -path './dev/*' \) -prune -o -print0 \
    | LC_ALL=C sort -z | cpio --null --create --format=newc --reproducible --quiet) >"$TEMP_IMAGE"
mv -- "$TEMP_IMAGE" "$OUTPUT_IMAGE"

printf '[7/7] Updating %s\n' "$LIMINE_CONFIG"
TEMP_CONFIG="$WORK_DIR/limine.conf"
awk '
    BEGIN { emitted = 0; module = "    module_path: boot():/initramfs.cpio" }
    /module_path:[[:space:]].*\/initramfs([^[:space:]]*)/ { if (!emitted) print module; emitted = 1; next }
    { print }
    !emitted && /module_path:[[:space:]].*\/init\.elf/ { print module; emitted = 1 }
    END { if (!emitted) exit 42 }
' "$LIMINE_CONFIG" >"$TEMP_CONFIG" || { printf 'Could not find init.elf module in %s\n' "$LIMINE_CONFIG" >&2; exit 1; }
chmod --reference="$LIMINE_CONFIG" "$TEMP_CONFIG"
if ! cmp -s "$TEMP_CONFIG" "$LIMINE_CONFIG"; then
    mv -- "$TEMP_CONFIG" "$LIMINE_CONFIG"
else
    rm -f -- "$TEMP_CONFIG"
fi
rm -f -- "$LEGACY_COMPRESSED_IMAGE"
if [[ -n "${SUDO_UID:-}" ]]; then chown "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$OUTPUT_IMAGE" "$LIMINE_CONFIG"; fi

printf 'Created %s (%s)\n' "$OUTPUT_IMAGE" "$(du -h "$OUTPUT_IMAGE" | awk '{print $1}')"
if ((BUILD_ISO)); then
    if [[ -n "${SUDO_USER:-}" && "$SUDO_USER" != root ]]; then sudo -u "$SUDO_USER" -- make -C "$PROJECT_ROOT" Uinxed-x64.iso; else make -C "$PROJECT_ROOT" Uinxed-x64.iso; fi
fi
printf '%s\n' 'Done.'
