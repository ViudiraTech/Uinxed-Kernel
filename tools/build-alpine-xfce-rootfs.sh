#!/bin/sh
# Reproducible Alpine/Xfce initramfs builder for the Uinxed kernel.
set -eu

PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CACHE_DIR="$PROJECT_ROOT/.cache"
ROOTFS="$CACHE_DIR/alpine-rootfs"
OVERLAY="$PROJECT_ROOT/assets/rootfs-overlay"
OUTPUT="$PROJECT_ROOT/assets/initramfs.cpio"
ALPINE_VERSION=3.24.1
ALPINE_ARCH=x86_64
MINIROOTFS="alpine-minirootfs-${ALPINE_VERSION}-${ALPINE_ARCH}.tar.gz"
MINIROOTFS_URL="https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_VERSION%.*}/releases/${ALPINE_ARCH}/${MINIROOTFS}"
MINIROOTFS_SHA256=41f73e3cf5fa919b8aa5ca6b30dc48f0da2720776d7423e2a7748211456fe081

for tool in bwrap cpio curl fakeroot sha256sum sort tar; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "rootfs: required host tool is missing: $tool" >&2
        exit 1
    }
done

mkdir -p "$CACHE_DIR"
archive="$CACHE_DIR/$MINIROOTFS"
if [ ! -f "$archive" ]; then
    curl --fail --location --retry 4 --output "$archive.part" "$MINIROOTFS_URL"
    mv "$archive.part" "$archive"
fi
printf '%s  %s\n' "$MINIROOTFS_SHA256" "$archive" | sha256sum --check --status || {
    echo "rootfs: Alpine minirootfs checksum mismatch" >&2
    exit 1
}
rm -rf "$ROOTFS"
mkdir -p "$ROOTFS"
tar -xzf "$archive" -C "$ROOTFS"

in_root() {
    bwrap --unshare-user --uid 0 --gid 0 --unshare-pid --unshare-uts --unshare-ipc \
        --bind "$ROOTFS" / --proc /proc --dev /dev \
        --ro-bind /etc/resolv.conf /etc/resolv.conf "$@"
}

# Build a clean, reproducible Weston image.  Weston is the Wayland reference
# compositor and its desktop shell is the only graphical session in this
# profile; there is no Xorg/Xfce/display-manager fallback.
in_root /sbin/apk add --no-scripts --usermode \
    alpine-base openrc busybox-openrc \
    eudev eudev-openrc eudev-hwids udev-init-scripts-openrc \
    dbus dbus-openrc \
    bash clang binutils python3 nano

# --usermode deliberately avoids privileged package scripts on the host.
# Reproduce their required account and cache effects inside the isolated root.
in_root /bin/sh -eu -c '
    /bin/busybox --install -s
    if ! /bin/busybox grep -q "^messagebus:" /etc/group; then /bin/busybox addgroup -S messagebus; fi
    if ! /bin/busybox grep -q "^messagebus:" /etc/passwd; then
        /bin/busybox adduser -S -D -H -h /dev/null -s /sbin/nologin -G messagebus -g messagebus messagebus
    fi
    mkdir -p /run/user/0 /var/lib/dbus /var/log
    dbus-uuidgen --ensure=/etc/machine-id
    udevadm hwdb --update
    fc-cache --system-only >/dev/null || true
'

cp -a "$OVERLAY/." "$ROOTFS/"

# Use the distro service definitions and enforce the required ordering:
# daemon -> coldplug trigger -> settle, followed by desktop services.
in_root /bin/sh -eu -c '
    /sbin/rc-update del mdev sysinit >/dev/null 2>&1 || true
    /sbin/rc-update add udev sysinit
    /sbin/rc-update add udev-trigger sysinit
    /sbin/rc-update add udev-settle sysinit
    /sbin/rc-update add udev-postmount default
    /sbin/rc-update add dbus default
    /sbin/rc-update add local default
'

shadow_gid=$(awk -F: '$1 == "shadow" { print $3; exit }' "$ROOTFS/etc/group")
messagebus_gid=$(awk -F: '$1 == "messagebus" { print $3; exit }' "$ROOTFS/etc/group")
test -n "$shadow_gid" && test -n "$messagebus_gid"

tmp_output="$OUTPUT.tmp"
export ROOTFS OUTPUT tmp_output shadow_gid messagebus_gid
fakeroot -- sh -eu -c '
    chown -R 0:0 "$ROOTFS"
    chown 0:"$shadow_gid" "$ROOTFS/etc/shadow"
    chmod 0640 "$ROOTFS/etc/shadow"
    if [ -e "$ROOTFS/usr/libexec/dbus-daemon-launch-helper" ]; then
        chown 0:"$messagebus_gid" "$ROOTFS/usr/libexec/dbus-daemon-launch-helper"
        chmod 04750 "$ROOTFS/usr/libexec/dbus-daemon-launch-helper"
    fi
    chmod 04711 "$ROOTFS/bin/bbsuid"
    cd "$ROOTFS"
    find . -xdev -print0 | LC_ALL=C sort -z | cpio --null --quiet -o --format=newc >"$tmp_output"
'
mv "$tmp_output" "$OUTPUT"

size=$(wc -c <"$OUTPUT")
if [ "$size" -lt 104857600 ]; then
    echo "rootfs: generated archive is unexpectedly small ($size bytes)" >&2
    exit 1
fi
echo "rootfs: wrote $OUTPUT ($size bytes)"
