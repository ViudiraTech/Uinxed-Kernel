#!/usr/bin/env bash
# Build a minimal Alpine Xfce/Pixman initramfs for Uinxed-Kernel.

set -Eeuo pipefail

# Alpine 3.23 is intentional: its librsvg package still provides the native
# GdkPixbuf SVG loader.  Alpine 3.24 routes SVG through glycin/bubblewrap,
# which is larger and requires Linux namespace/seccomp features that are not
# part of the Uinxed userspace ABI yet.
ALPINE_VERSION="${ALPINE_VERSION:-3.23.5}"
ALPINE_ARCH="${ALPINE_ARCH:-x86_64}"
ALPINE_BRANCH="${ALPINE_BRANCH:-v${ALPINE_VERSION%.*}}"
ALPINE_MIRROR="${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}"

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
CACHE_DIR="$PROJECT_ROOT/.cache"
OUTPUT_IMAGE="$PROJECT_ROOT/assets/Limine/initramfs.cpio"
KEEP_ROOTFS=""

usage()
{
    printf '%s\n' \
        "Usage: $0 [--output FILE] [--keep-rootfs DIR]" \
        "Build a minimal Alpine/OpenRC/Xorg/Xfce initramfs." \
        "Xorg uses the modesetting driver with glamor disabled, so GTK/Cairo renders through Pixman." \
        "Environment: ALPINE_VERSION, ALPINE_ARCH, ALPINE_BRANCH, ALPINE_MIRROR"
}

while (($#)); do
    case "$1" in
        --output)
            [[ $# -ge 2 ]] || { usage >&2; exit 2; }
            OUTPUT_IMAGE="$2"
            shift 2
            ;;
        --keep-rootfs)
            [[ $# -ge 2 ]] || { usage >&2; exit 2; }
            KEEP_ROOTFS="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

for command_name in bwrap cpio curl fakeroot find install mktemp sha256sum sort tar; do
    command -v "$command_name" >/dev/null 2>&1 || {
        printf 'Missing host command: %s\n' "$command_name" >&2
        exit 1
    }
done

WORK_DIR="$(mktemp -d /tmp/uinxed-xfce-rootfs.XXXXXX)"
ROOTFS="$WORK_DIR/rootfs"
ARCHIVE_NAME="alpine-minirootfs-${ALPINE_VERSION}-${ALPINE_ARCH}.tar.gz"
ARCHIVE="$CACHE_DIR/$ARCHIVE_NAME"
ARCHIVE_URL="$ALPINE_MIRROR/$ALPINE_BRANCH/releases/$ALPINE_ARCH/$ARCHIVE_NAME"

cleanup()
{
    if [[ -d "${WORK_DIR:-}" && "$WORK_DIR" == /tmp/uinxed-xfce-rootfs.* ]]; then
        rm -rf -- "$WORK_DIR"
    fi
}
trap cleanup EXIT INT TERM

mkdir -p -- "$CACHE_DIR"
if [[ ! -f "$ARCHIVE" ]]; then
    printf '[1/6] Downloading %s\n' "$ARCHIVE_URL"
    curl --fail --location --retry 3 --output "$ARCHIVE.part" "$ARCHIVE_URL"
    mv -- "$ARCHIVE.part" "$ARCHIVE"
else
    printf '[1/6] Reusing %s\n' "$ARCHIVE"
fi
curl --fail --location --retry 3 --output "$WORK_DIR/$ARCHIVE_NAME.sha256" "$ARCHIVE_URL.sha256"
(
    cd "$CACHE_DIR"
    sha256sum --check "$WORK_DIR/$ARCHIVE_NAME.sha256"
)

printf '[2/6] Extracting Alpine minirootfs\n'
mkdir -p -- "$ROOTFS"
tar --extract --gzip --file "$ARCHIVE" --directory "$ROOTFS"
install -Dm644 /etc/resolv.conf "$ROOTFS/etc/resolv.conf"
printf '%s\n%s\n' \
    "$ALPINE_MIRROR/$ALPINE_BRANCH/main" \
    "$ALPINE_MIRROR/$ALPINE_BRANCH/community" \
    >"$ROOTFS/etc/apk/repositories"

in_root()
{
    bwrap \
        --die-with-parent \
        --unshare-user \
        --uid 0 \
        --gid 0 \
        --unshare-pid \
        --unshare-uts \
        --unshare-ipc \
        --setenv PATH /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        --bind "$ROOTFS" / \
        --proc /proc \
        --dev /dev \
        "$@"
}

printf '[3/6] Installing the minimal OpenRC, Xorg, Xfce, and GTK userspace\n'
in_root /sbin/apk add --no-scripts --usermode \
    alpine-base \
    bash \
    dbus \
    dbus-openrc \
    dbus-x11 \
    eudev \
    eudev-hwids \
    eudev-openrc \
    openrc \
    udev-init-scripts-openrc \
    xorg-server \
    xinit \
    xauth \
    xf86-input-libinput \
    xf86-video-modesetting \
    xfce4 \
    xfce4-terminal \
    xdg-utils \
    adwaita-icon-theme \
    librsvg \
    font-dejavu

in_root /bin/sh -eu -c '
    for group in input video messagebus; do
        grep -q "^${group}:" /etc/group || addgroup -S "$group"
    done
    if ! grep -q "^messagebus:" /etc/passwd; then
        adduser -S -D -H -h /dev/null -s /sbin/nologin -G messagebus -g messagebus messagebus
    fi

    mkdir -p /run/user/0 /var/lib/dbus /var/log
    dbus-uuidgen --ensure=/etc/machine-id
    ln -snf /etc/machine-id /var/lib/dbus/machine-id

    rc-update del mdev sysinit >/dev/null 2>&1 || true
    rc-update add udev sysinit
    rc-update add udev-trigger sysinit
    rc-update add udev-settle sysinit
    rc-update add bootmisc boot
    rc-update add dbus default
    rc-update add local default

    command -v udevadm >/dev/null && udevadm hwdb --update || true
    command -v glib-compile-schemas >/dev/null && glib-compile-schemas /usr/share/glib-2.0/schemas || true
    command -v update-mime-database >/dev/null && update-mime-database /usr/share/mime || true
    command -v gdk-pixbuf-query-loaders >/dev/null && gdk-pixbuf-query-loaders --update-cache || true
    command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -f /usr/share/icons/hicolor || true
    command -v fc-cache >/dev/null && fc-cache -s -f >/dev/null || true
    rm -rf /var/cache/apk/* /usr/share/doc /usr/share/man /usr/share/info /usr/share/installed-tests
'

printf '[4/6] Configuring OpenRC, Xorg/Pixman, Xfce, and serial logging\n'
install -d -m755 \
    "$ROOTFS/etc/X11/xorg.conf.d" \
    "$ROOTFS/etc/udev/rules.d" \
    "$ROOTFS/root/.config/xfce4/xfconf/xfce-perchannel-xml" \
    "$ROOTFS/usr/local/sbin" \
    "$ROOTFS/var/log"
install -d -m700 "$ROOTFS/run/user/0"
install -d -m1777 "$ROOTFS/tmp" "$ROOTFS/var/tmp"

printf '%s\n' uinxed-xfce >"$ROOTFS/etc/hostname"
cat >"$ROOTFS/etc/fstab" <<'FSTAB'
proc  /proc  proc  defaults  0 0
sysfs /sys   sysfs defaults  0 0
tmpfs /run   tmpfs mode=0755,nosuid,nodev 0 0
FSTAB

cat >"$ROOTFS/etc/udev/rules.d/99-uinxed-input.rules" <<'UDEV_RULES'
ACTION!="remove", SUBSYSTEM=="input", KERNEL=="event[0-9]*", ENV{ID_INPUT}="1", MODE="0660", GROUP="input", TAG+="seat"
ACTION!="remove", SUBSYSTEM=="input", KERNEL=="event[0-9]*", ATTRS{name}=="AT Translated Set 2 keyboard", ENV{ID_INPUT_KEYBOARD}="1"
ACTION!="remove", SUBSYSTEM=="input", KERNEL=="event[0-9]*", ATTRS{name}=="PS/2 Generic Mouse", ENV{ID_INPUT_MOUSE}="1"
ACTION!="remove", SUBSYSTEM=="drm", KERNEL=="card[0-9]*", ENV{ID_SEAT}="seat0", MODE="0660", GROUP="video", TAG+="seat"
UDEV_RULES

cat >"$ROOTFS/etc/X11/Xwrapper.config" <<'XWRAPPER'
allowed_users=anybody
needs_root_rights=yes
XWRAPPER

cat >"$ROOTFS/etc/X11/xorg.conf.d/20-uinxed-pixman.conf" <<'XORG_CONFIG'
Section "Device"
    Identifier "Uinxed DRM Pixman"
    Driver "modesetting"
    Option "AccelMethod" "none"
    # VirtIO-GPU dumb buffers are ordinary guest RAM: draw into them directly
    # and use DIRTYFB damage instead of copying through a second shadow buffer.
    Option "ShadowFB" "false"
    # The hardware cursor is not visible on every VirtIO-GPU backend yet.
    # Direct dumb-buffer rendering plus ordered DIRTYFB updates keeps this
    # reliable fallback cheap without the old ShadowFB copy.
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
XORG_CONFIG

cat >"$ROOTFS/usr/local/sbin/openrc-serial" <<'OPENRC_SERIAL'
#!/bin/sh
exec </dev/console >/dev/ttyS0 2>&1
exec /sbin/openrc "$@"
OPENRC_SERIAL

cat >"$ROOTFS/root/.config/xfce4/xfconf/xfce-perchannel-xml/xfwm4.xml" <<'XFWM4_CONFIG'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xfwm4" version="1.0">
  <property name="general" type="empty">
    <property name="use_compositing" type="bool" value="false"/>
  </property>
</channel>
XFWM4_CONFIG

cat >"$ROOTFS/usr/local/sbin/start-xfce-root" <<'START_XFCE'
#!/bin/sh

export HOME=/root USER=root LOGNAME=root
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export XDG_RUNTIME_DIR=/run/user/0
export XDG_CONFIG_HOME=/root/.config
export XDG_CACHE_HOME=/root/.cache
export XDG_DATA_HOME=/root/.local/share
export XDG_SESSION_TYPE=x11
export XDG_SESSION_DESKTOP=xfce
export XDG_CURRENT_DESKTOP=XFCE
export DESKTOP_SESSION=xfce
export XDG_VTNR=1
export LIBGL_ALWAYS_SOFTWARE=1
unset G_DEBUG G_MESSAGES_DEBUG GTK_DEBUG GDK_DEBUG GIO_DEBUG XFCONF_DEBUG
unset DISPLAY WAYLAND_DISPLAY XAUTHORITY

SESSION_LOG=/var/log/xfce-session.log
: >"$SESSION_LOG"
echo "[uinxed-xfce] desktop diagnostics: $SESSION_LOG" >/dev/ttyS0
exec </dev/tty1 >>"$SESSION_LOG" 2>&1

install -d -m700 /run/user/0
install -d -m1777 /tmp/.X11-unix /tmp/.ICE-unix
rm -f /root/.serverauth.* /tmp/.X0-lock /tmp/.X11-unix/X0

XORG_LOG=/var/log/Xorg.0.log
: >"$XORG_LOG"

echo "[uinxed-xfce] starting Xorg modesetting + Pixman on tty1"
/usr/bin/Xorg :0 vt1 -keeptty -novtswitch -nolisten tcp -ac \
    -extension GLX -verbose 1 -logverbose 1 -logfile "$XORG_LOG" &
XORG_PID=$!

cleanup()
{
    kill "$XORG_PID" 2>/dev/null || true
    wait "$XORG_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

attempt=0
while [ ! -S /tmp/.X11-unix/X0 ] && [ "$attempt" -lt 200 ]; do
    if ! kill -0 "$XORG_PID" 2>/dev/null; then
        echo "[uinxed-xfce] Xorg exited before display :0 became ready"
        exit 1
    fi
    sleep 0.05
    attempt=$((attempt + 1))
done
if [ ! -S /tmp/.X11-unix/X0 ]; then
    echo "[uinxed-xfce] timed out waiting for Xorg display :0"
    exit 1
fi

export DISPLAY=:0
echo "[uinxed-xfce] starting D-Bus and Xfce"
# Xfce's documented command-line entry point is startxfce4.  Give it a
# private session bus so all Xfce components share the same D-Bus lifetime.
/usr/bin/dbus-run-session -- /usr/bin/startxfce4
status=$?
echo "[uinxed-xfce] Xfce session exited with status $status"
exit "$status"
START_XFCE

cat >"$ROOTFS/etc/inittab" <<'INITTAB'
# Keep finite OpenRC boot diagnostics on ttyS0; desktop logs stay in /var/log.
::sysinit:/usr/local/sbin/openrc-serial sysinit
::sysinit:/usr/local/sbin/openrc-serial boot
::wait:/usr/local/sbin/openrc-serial default

tty1::respawn:/sbin/getty -n -l /usr/local/sbin/start-xfce-root 38400 tty1
tty2::respawn:/sbin/getty 38400 tty2

::ctrlaltdel:/sbin/reboot
::shutdown:/usr/local/sbin/openrc-serial shutdown
INITTAB

chmod 0755 "$ROOTFS/usr/local/sbin/openrc-serial" "$ROOTFS/usr/local/sbin/start-xfce-root"

printf '[5/6] Packing %s\n' "$OUTPUT_IMAGE"
mkdir -p -- "$(dirname -- "$OUTPUT_IMAGE")"
TEMP_IMAGE="$WORK_DIR/initramfs.cpio"
SHADOW_GID="$(awk -F: '$1 == "shadow" { print $3; exit }' "$ROOTFS/etc/group")"
MESSAGEBUS_GID="$(awk -F: '$1 == "messagebus" { print $3; exit }' "$ROOTFS/etc/group")"
[[ -n "$SHADOW_GID" && -n "$MESSAGEBUS_GID" ]] || {
    printf 'Required rootfs groups are missing.\n' >&2
    exit 1
}
export ROOTFS TEMP_IMAGE SHADOW_GID MESSAGEBUS_GID
fakeroot -- sh -eu -c '
    chown -R 0:0 "$ROOTFS"
    chown 0:"$SHADOW_GID" "$ROOTFS/etc/shadow"
    chmod 0640 "$ROOTFS/etc/shadow"
    if [ -e "$ROOTFS/usr/libexec/dbus-daemon-launch-helper" ]; then
        chown 0:"$MESSAGEBUS_GID" "$ROOTFS/usr/libexec/dbus-daemon-launch-helper"
        chmod 04750 "$ROOTFS/usr/libexec/dbus-daemon-launch-helper"
    fi
    # Alpine installs bbsuid execute-only.  Give its owner read permission so
    # an unprivileged host can copy it while preserving the guest setuid mode.
    chmod 04711 "$ROOTFS/bin/bbsuid"
    cd "$ROOTFS"
    find . -xdev \( -path ./dev -o -path "./dev/*" \) -prune -o -print0 |
        LC_ALL=C sort -z |
        cpio --null --create --format=newc --reproducible --quiet >"$TEMP_IMAGE"
'
mv -- "$TEMP_IMAGE" "$OUTPUT_IMAGE"

if [[ -n "$KEEP_ROOTFS" ]]; then
    if [[ -e "$KEEP_ROOTFS" ]]; then
        printf 'Refusing to replace existing keep-rootfs path: %s\n' "$KEEP_ROOTFS" >&2
        exit 1
    fi
    mv -- "$ROOTFS" "$KEEP_ROOTFS"
fi

printf '[6/6] Created %s (%s)\n' "$OUTPUT_IMAGE" "$(du -h "$OUTPUT_IMAGE" | awk '{print $1}')"
