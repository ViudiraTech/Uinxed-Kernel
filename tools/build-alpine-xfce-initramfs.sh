#!/usr/bin/env bash
# Build an Alpine Xfce initramfs for Uinxed and wire it into Limine.

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
        "Build Alpine Xfce as assets/Limine/initramfs.cpio." \
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

printf '[3/7] Installing Xfce desktop, Xorg, applications, and OpenRC\n'
chroot "$ROOTFS" /bin/sh -eux <<CHROOT_SETUP
apk update
apk add alpine-base openrc eudev udev-init-scripts dbus dbus-x11 \
    xorg-server xinit xauth xf86-input-libinput \
    xfce4 xfce4-session xfce4-terminal xfce4-screensaver \
    mesa-dri-gallium mesa-egl mesa-gbm mesa-gl \
    font-dejavu capitaine-cursors \
    libinput-tools evtest bash \
    fastfetch htop nano less file \
    xterm xeyes xclock \
    clang gcc musl-dev binutils make coreutils ${EXTRA_PACKAGES}
addgroup -S input 2>/dev/null || true
addgroup -S video 2>/dev/null || true
for service in udev udev-trigger; do rc-update add "\$service" sysinit; done
rc-update add dbus default
rc-update del elogind default 2>/dev/null || true
rm -rf /var/cache/apk/*
CHROOT_SETUP

printf '[4/7] Configuring automatic root Xfce desktop, Xorg, and input permissions\n'
install -d -m755 "$ROOTFS/etc/udev/rules.d" "$ROOTFS/etc/profile.d" \
    "$ROOTFS/etc/X11/xorg.conf.d" "$ROOTFS/etc/xdg/xfce4/xfconf/xfce-perchannel-xml" \
    "$ROOTFS/usr/local/bin" "$ROOTFS/usr/local/sbin" "$ROOTFS/root/.config/xfce4/xfconf/xfce-perchannel-xml"
cat >"$ROOTFS/etc/profile.d/uinxed-prompt.sh" <<'BASH_PROFILE'
# Debian-style prompt: green user@host, blue working directory.
# Keep the escape sequences wrapped so bash/readline counts the width correctly.
if [ -n "${BASH_VERSION:-}" ]; then
    case $- in
        *i*)
            PS1='\[\e[1;32m\]\u@\h\[\e[0m\]:\[\e[1;34m\]\w\[\e[0m\]\$ '
            export PS1
            ;;
    esac
fi
BASH_PROFILE
cat >"$ROOTFS/usr/local/sbin/start-bash" <<'START_BASH'
#!/bin/sh
exec /bin/bash --login
START_BASH
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
cat >"$ROOTFS/etc/X11/Xwrapper.config" <<'XWRAPPER_CONFIG'
# Xfce is started automatically as root on tty1 in this development image.
allowed_users=anybody
needs_root_rights=yes
XWRAPPER_CONFIG
cat >"$ROOTFS/etc/X11/xorg.conf.d/20-uinxed.conf" <<'XORG_CONF'
Section "Device"
    Identifier "Uinxed DRM"
    Driver "modesetting"
    Option "AccelMethod" "none"
    Option "SWCursor" "true"
EndSection

Section "ServerFlags"
    Option "AutoAddDevices" "true"
    Option "AutoEnableDevices" "true"
    Option "DontVTSwitch" "true"
    Option "BlankTime" "0"
    Option "StandbyTime" "0"
    Option "SuspendTime" "0"
    Option "OffTime" "0"
EndSection

Section "InputClass"
    Identifier "Uinxed libinput keyboard"
    MatchIsKeyboard "on"
    Driver "libinput"
EndSection

Section "InputClass"
    Identifier "Uinxed libinput pointer"
    MatchIsPointer "on"
    Driver "libinput"
EndSection
XORG_CONF
cat >"$ROOTFS/root/.config/xfce4/xfconf/xfce-perchannel-xml/xfwm4.xml" <<'XFWM4_CONFIG'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xfwm4" version="1.0">
  <property name="general" type="empty">
    <property name="use_compositing" type="bool" value="false"/>
    <property name="vblank_mode" type="string" value="none"/>
  </property>
</channel>
XFWM4_CONFIG
cat >"$ROOTFS/root/.xinitrc" <<'ROOT_XINITRC'
#!/bin/sh
export XDG_SESSION_TYPE=x11
export XDG_CURRENT_DESKTOP=XFCE
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/0}"
export LIBGL_ALWAYS_SOFTWARE=1
unset WAYLAND_DISPLAY
exec /usr/bin/dbus-run-session -- /usr/bin/startxfce4
ROOT_XINITRC
cat >"$ROOTFS/usr/local/sbin/start-xfce-root" <<'START_XFCE'
#!/bin/sh
export HOME=/root USER=root LOGNAME=root
exec </dev/tty1
if [ -e /dev/ttyS0 ]; then
    exec >/dev/ttyS0 2>&1
else
    exec >/dev/tty1 2>&1
fi
install -d -m700 /run/user/0
export XDG_RUNTIME_DIR=/run/user/0
export XDG_CONFIG_HOME=/root/.config
export XDG_DATA_HOME=/root/.local/share
export XDG_CACHE_HOME=/root/.cache
export XDG_VTNR=1
export XDG_SESSION_TYPE=x11
export XDG_CURRENT_DESKTOP=XFCE
export LIBGL_ALWAYS_SOFTWARE=1
export XCURSOR_THEME=capitaine-cursors-dark
export XCURSOR_SIZE=24
unset DISPLAY WAYLAND_DISPLAY XAUTHORITY

# Start Xorg directly.  startx creates a transient .serverauth.* file, while
# this root-only development image has no need for X11 cookie authentication.
# -ac also avoids making Xorg depend on xauth's locking/rename path.
XORG_LOG=/var/log/Xorg.0.log
/usr/bin/Xorg :0 vt1 -keeptty -novtswitch -nolisten tcp -ac -logfile "$XORG_LOG" &
XORG_PID=$!
cleanup_xorg() {
    kill "$XORG_PID" 2>/dev/null || true
    wait "$XORG_PID" 2>/dev/null || true
}
trap cleanup_xorg EXIT INT TERM

attempt=0
while [ ! -e /tmp/.X11-unix/X0 ] && [ "$attempt" -lt 100 ]; do
    if ! kill -0 "$XORG_PID" 2>/dev/null; then
        echo "Xorg exited before creating display :0"
        cat "$XORG_LOG" 2>/dev/null || true
        exit 1
    fi
    sleep 0.1
    attempt=$((attempt + 1))
done
if [ ! -e /tmp/.X11-unix/X0 ]; then
    echo "Timed out waiting for Xorg display :0"
    cat "$XORG_LOG" 2>/dev/null || true
    exit 1
fi

export DISPLAY=:0
unset XAUTHORITY
SESSION_LOG=/var/log/xfce-session.log
: >"$SESSION_LOG"
echo "Starting Xfce session on DISPLAY=$DISPLAY" >>"$SESSION_LOG"
# Xorg is already running above.  Run Alpine's Xfce xinitrc directly: it sets
# DESKTOP_SESSION/XDG_* and prepares the activation environment before it
# execs xfce4-session.  Calling startxfce4 here would add another xinit layer.
/usr/bin/dbus-run-session -- /etc/xdg/xfce4/xinitrc >>"$SESSION_LOG" 2>&1
session_status=$?
cat "$SESSION_LOG"
echo "Xfce session exited with status $session_status"
exit "$session_status"
START_XFCE
chmod 0755 "$ROOTFS/root/.xinitrc" "$ROOTFS/usr/local/sbin/start-xfce-root" "$ROOTFS/usr/local/sbin/start-bash"

if grep -q '^tty1::respawn:' "$ROOTFS/etc/inittab"; then
    sed -i 's#^tty1::respawn:.*#tty1::respawn:/sbin/getty -n -l /usr/local/sbin/start-xfce-root 38400 tty1#' "$ROOTFS/etc/inittab"
else
    printf '%s\n' 'tty1::respawn:/sbin/getty -n -l /usr/local/sbin/start-xfce-root 38400 tty1' >>"$ROOTFS/etc/inittab"
fi
if grep -q '^#\?ttyS0::respawn:' "$ROOTFS/etc/inittab"; then
    sed -i 's|^#\?ttyS0::respawn:.*|# ttyS0 is reserved for kernel and Xorg logs.|' "$ROOTFS/etc/inittab"
fi
cat >"$ROOTFS/etc/motd" <<'MOTD'
Uinxed Alpine Xfce rootfs

Xorg and Xfce start automatically as root on tty1 with software rendering.
The original Weston builder is kept in tools/build-alpine-weston-initramfs.sh.
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
