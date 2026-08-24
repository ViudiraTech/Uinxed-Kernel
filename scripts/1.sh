#!/usr/bin/env bash
# Build an Alpine rootfs initramfs for Uinxed.

set -Eeuo pipefail

if ((EUID != 0)); then
    printf 'This script must be run with sudo.\n' >&2
    printf 'Usage: sudo %s\n' "$0" >&2
    exit 1
fi

trap cleanup EXIT

# ==============================================================================
# Configuration
# ==============================================================================

# Alpine release.
ALPINE_VERSION="3.24.1"
ALPINE_ARCH="x86_64"
ALPINE_BRANCH="v${ALPINE_VERSION%.*}"
ALPINE_MIRROR="${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}"

# Rootfs and output paths.
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
ROOTFS_DIR="$(mktemp -d "${TMPDIR:-/tmp}/uinxed-rootfs.XXXXXX")"
OUTPUT_DIR="$PROJECT_ROOT/assets/Limine"
OUTPUT_FILE="$OUTPUT_DIR/initramfs.cpio"

# Alpine repositories.
ALPINE_REPOSITORIES=(
    "$ALPINE_MIRROR/$ALPINE_BRANCH/main"
    "$ALPINE_MIRROR/$ALPINE_BRANCH/community"
)

# Packages installed into the rootfs.
PACKAGES=(
    # Base system.
    alpine-base
    openrc
    eudev
    udev-init-scripts
    dbus

    # Graphics.
    mesa-dri-gallium
    mesa-egl
    mesa-gbm
    mesa-gl

    # Weston / Wayland.
    weston
    weston-backend-drm
    weston-shell-desktop
    weston-terminal
    weston-clients
    weston-xwayland
    xwayland

    # Input / device management.
    seatd
    libinput-tools
    evtest
    pciutils

    # Shell / utilities.
    bash
    fastfetch
    htop
    nano
    less
    file

    # X11 applications.
    xterm
    xeyes
    xclock

    # Development tools.
    clang
    gcc
    musl-dev
    binutils
    make
    coreutils
    qemu-system-x86_64

    # Fonts / cursor.
    font-dejavu
    capitaine-cursors
)

# Whether to verify the downloaded Alpine archive.
VERIFY_DOWNLOAD=true

# Whether to update limine.conf automatically.
UPDATE_LIMINE_CONFIG=true

# Whether to remove the APK cache after installation.
CLEAN_APK_CACHE=true

# Whether to break hard links before creating the cpio archive.
BREAK_HARDLINKS=true

# cpio format.
CPIO_FORMAT="newc"

# Extra kernel/initramfs configuration.
HOSTNAME="uinxed"
MOTD="Uinxed Alpine Weston rootfs"

# Weston configuration.
WESTON_RENDERER="pixman"
WESTON_IDLE_TIME="0"
WESTON_CURSOR_THEME="capitaine-cursors-dark"
WESTON_CURSOR_SIZE="24"

# ==============================================================================
# End of configuration
# ==============================================================================

ARCHIVE_NAME="alpine-minirootfs-${ALPINE_VERSION}-${ALPINE_ARCH}.tar.gz"
ARCHIVE_URL="$ALPINE_MIRROR/$ALPINE_BRANCH/releases/$ALPINE_ARCH/$ARCHIVE_NAME"

# ==============================================================================
# Helpers
# ==============================================================================

die()
{
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

log()
{
    printf '\n[%s] %s\n' "$1" "$2"
}

require_commands()
{
    local command_name
    for command_name in "$@"; do
        command -v "$command_name" >/dev/null 2>&1 ||
            die "Missing host command: $command_name"
    done
}

run_as_root()
{
    if ((EUID == 0)); then
        "$@"
        return
    fi

    command -v sudo >/dev/null 2>&1 ||
        die "This script must run as root or sudo must be installed."
    sudo "$@"
}

cleanup()
{
    rm -rf -- "$ROOTFS_DIR"
}

# ==============================================================================
# Download Alpine rootfs
# ==============================================================================

download_rootfs()
{
    local archive
    local checksum

    log "1/5" "Downloading Alpine rootfs"
    archive="$ROOTFS_DIR/$ARCHIVE_NAME"
    checksum="$archive.sha256"

    rm -rf -- "$ROOTFS_DIR"
    mkdir -p -- "$ROOTFS_DIR"

    curl \
        --fail \
        --location \
        --retry 3 \
        --output "$archive" \
        "$ARCHIVE_URL"

    if [[ "$VERIFY_DOWNLOAD" == true ]]; then
        curl \
            --fail \
            --location \
            --retry 3 \
            --output "$checksum" \
            "$ARCHIVE_URL.sha256"

        (
            cd "$ROOTFS_DIR"
            sha256sum --check "$ARCHIVE_NAME.sha256"
        )
    fi

    tar \
        --extract \
        --gzip \
        --file "$archive" \
        --directory "$ROOTFS_DIR"

    rm -f -- "$archive" "$checksum"

    install -Dm644 /etc/resolv.conf \
        "$ROOTFS_DIR/etc/resolv.conf"

    printf '%s\n' "${ALPINE_REPOSITORIES[@]}" \
        >"$ROOTFS_DIR/etc/apk/repositories"
}

# ==============================================================================
# Install packages
# ==============================================================================

install_packages()
{
    log "2/5" "Installing Alpine packages"
    local package_list=()

    package_list+=("${PACKAGES[@]}")
    chroot "$ROOTFS_DIR" /bin/sh -eux <<CHROOT
apk update
apk add ${package_list[*]}
CHROOT

    if [[ "$CLEAN_APK_CACHE" == true ]]; then
        rm -rf -- "$ROOTFS_DIR/var/cache/apk/"*
    fi
}


# ==============================================================================
# Configure rootfs
# ==============================================================================

configure_rootfs()
{
    log "3/5" "Configuring rootfs"
    install -d -m755 \
        "$ROOTFS_DIR/etc/udev/rules.d" \
        "$ROOTFS_DIR/etc/profile.d" \
        "$ROOTFS_DIR/etc/xdg/weston" \
        "$ROOTFS_DIR/usr/local/bin" \
        "$ROOTFS_DIR/usr/local/sbin"

    # --------------------------------------------------------------------------
    # Hostname
    # --------------------------------------------------------------------------

    printf '%s\n' "$HOSTNAME" \
        >"$ROOTFS_DIR/etc/hostname"

    # --------------------------------------------------------------------------
    # Shell prompt
    # --------------------------------------------------------------------------

    cat >"$ROOTFS_DIR/etc/profile.d/uinxed-prompt.sh" <<'EOF'
if [ -n "${BASH_VERSION:-}" ]; then
    case $- in
        *i*)
            PS1='\[\e[1;32m\]\u@\h\[\e[0m\]:\[\e[1;34m\]\w\[\e[0m\]\$ '
            export PS1
            ;;
    esac
fi
EOF

    # --------------------------------------------------------------------------
    # Bash
    # --------------------------------------------------------------------------

    cat >"$ROOTFS_DIR/usr/local/sbin/start-bash" <<'EOF'
#!/bin/sh
exec /bin/bash --login
EOF

    # --------------------------------------------------------------------------
    # Filesystems
    # --------------------------------------------------------------------------

    cat >"$ROOTFS_DIR/etc/fstab" <<'EOF'
proc  /proc  proc  defaults  0 0
sysfs /sys   sysfs defaults  0 0
tmpfs /run   tmpfs mode=0755,nosuid,nodev 0 0
EOF

    # --------------------------------------------------------------------------
    # udev
    # --------------------------------------------------------------------------

    cat >"$ROOTFS_DIR/etc/udev/rules.d/99-uinxed-input.rules" <<'EOF'
ACTION!="remove", SUBSYSTEM=="input", KERNEL=="event[0-9]*", \
    ENV{ID_INPUT}="1", MODE="0660", GROUP="input", TAG+="seat"

ACTION!="remove", SUBSYSTEM=="input", KERNEL=="event[0-9]*", \
    ATTRS{name}=="AT Translated Set 2 keyboard", \
    ENV{ID_INPUT_KEYBOARD}="1"

ACTION!="remove", SUBSYSTEM=="input", KERNEL=="event[0-9]*", \
    ATTRS{name}=="PS/2 Generic Mouse", \
    ENV{ID_INPUT_MOUSE}="1"

ACTION!="remove", SUBSYSTEM=="drm", KERNEL=="card[0-9]*", \
    ENV{ID_SEAT}="seat0", MODE="0660", GROUP="video", TAG+="seat"
EOF

    # --------------------------------------------------------------------------
    # Groups
    # --------------------------------------------------------------------------

    chroot "$ROOTFS_DIR" /bin/sh -c '
        addgroup -S input 2>/dev/null || true
        addgroup -S video 2>/dev/null || true
    '

    # --------------------------------------------------------------------------
    # OpenRC
    # --------------------------------------------------------------------------

    chroot "$ROOTFS_DIR" /bin/sh -eux <<'CHROOT'
for service in udev udev-trigger; do
    rc-update add "$service" sysinit
done

rc-update del elogind default 2>/dev/null || true
CHROOT

    # --------------------------------------------------------------------------
    # Weston
    # --------------------------------------------------------------------------

    cat >"$ROOTFS_DIR/etc/xdg/weston/weston.ini" <<EOF
[core]
shell=desktop-shell.so
renderer=$WESTON_RENDERER
idle-time=$WESTON_IDLE_TIME
require-input=false
xwayland=true

[shell]
locking=false
panel-position=top
background-image=/usr/share/weston/background.png
background-type=scale-crop
cursor-theme=$WESTON_CURSOR_THEME
cursor-size=$WESTON_CURSOR_SIZE

[launcher]
icon=/usr/share/weston/icon_terminal.png
path=/usr/local/sbin/start-bash-terminal

[launcher]
icon=/usr/share/weston/icon_terminal.png
path=/usr/local/sbin/start-fastfetch

[terminal]
font=DejaVu Sans Mono
font-size=16

[xwayland]
path=/usr/local/sbin/Xwayland-software
EOF

    # --------------------------------------------------------------------------
    # Weston terminal
    # --------------------------------------------------------------------------

    cat >"$ROOTFS_DIR/usr/local/sbin/start-fastfetch" <<'EOF'
#!/bin/sh
exec /usr/bin/weston-terminal \
    --shell=/usr/local/sbin/fastfetch-shell
EOF

    cat >"$ROOTFS_DIR/usr/local/sbin/start-bash-terminal" <<'EOF'
#!/bin/sh
exec /usr/bin/weston-terminal \
    --shell=/usr/local/sbin/start-bash
EOF

    cat >"$ROOTFS_DIR/usr/local/sbin/fastfetch-shell" <<'EOF'
#!/bin/sh
/usr/bin/fastfetch
exec /bin/bash --login
EOF

    # --------------------------------------------------------------------------
    # Xwayland
    # --------------------------------------------------------------------------

    cat >"$ROOTFS_DIR/usr/local/sbin/Xwayland-software" <<'EOF'
#!/bin/sh

export XWAYLAND_NO_GLAMOR=1

exec /usr/bin/Xwayland "$@" -shm -verbose 3
EOF

    # --------------------------------------------------------------------------
    # Weston startup
    # --------------------------------------------------------------------------

    cat >"$ROOTFS_DIR/usr/local/sbin/start-weston-root" <<'EOF'
#!/bin/sh

export HOME=/root
export USER=root
export LOGNAME=root

exec </dev/tty1 >/dev/ttyS0 2>&1

mkdir -p /tmp/.X11-unix /tmp/.ICE-unix
chmod 1777 /tmp/.X11-unix /tmp/.ICE-unix
rm -f /tmp/.X11-unix/X0

install -d -m700 /run/user/0

export XDG_RUNTIME_DIR=/run/user/0
export XDG_CONFIG_HOME=/root/.config
export XDG_VTNR=1
export WAYLAND_DISPLAY=wayland-0
export XDG_SESSION_TYPE=wayland
export XCURSOR_THEME=capitaine-cursors-dark
export XCURSOR_SIZE=24

if [ ! -e /run/seatd.sock ]; then
    /usr/bin/seatd -g video >/dev/null 2>&1 &
    sleep 1
fi

export LIBSEAT_BACKEND=seatd
export SEATD_SOCK=/run/seatd.sock
export SEATD_VTBOUND=0

exec /usr/bin/weston \
    -B drm \
    --renderer=pixman \
    --seat=seat0 \
    --continue-without-input \
    --config=/etc/xdg/weston/weston.ini \
    --log=/dev/ttyS0
EOF

    chmod 0755 \
        "$ROOTFS_DIR/usr/local/sbin/start-bash" \
        "$ROOTFS_DIR/usr/local/sbin/start-fastfetch" \
        "$ROOTFS_DIR/usr/local/sbin/start-bash-terminal" \
        "$ROOTFS_DIR/usr/local/sbin/fastfetch-shell" \
        "$ROOTFS_DIR/usr/local/sbin/Xwayland-software" \
        "$ROOTFS_DIR/usr/local/sbin/start-weston-root"

    # --------------------------------------------------------------------------
    # inittab
    # --------------------------------------------------------------------------

    if grep -q '^tty1::respawn:' "$ROOTFS_DIR/etc/inittab"; then
        sed -i \
            's#^tty1::respawn:.*#tty1::respawn:/sbin/getty -n -l /usr/local/sbin/start-weston-root 38400 tty1#' \
            "$ROOTFS_DIR/etc/inittab"
    else
        printf '%s\n' \
            'tty1::respawn:/sbin/getty -n -l /usr/local/sbin/start-weston-root 38400 tty1' \
            >>"$ROOTFS_DIR/etc/inittab"
    fi

    if grep -q '^#\?ttyS0::respawn:' "$ROOTFS_DIR/etc/inittab"; then
        sed -i \
            's|^#\?ttyS0::respawn:.*|# ttyS0 is reserved for kernel and Weston logs.|' \
            "$ROOTFS_DIR/etc/inittab"
    fi

    # --------------------------------------------------------------------------
    # MOTD
    # --------------------------------------------------------------------------

    printf '%s\n\n' "$MOTD" >"$ROOTFS_DIR/etc/motd"
    printf '%s\n' \
        'Weston starts automatically as root on tty1 with its DRM desktop shell.' \
        >>"$ROOTFS_DIR/etc/motd"
}


# ==============================================================================
# Pack rootfs
# ==============================================================================

pack_rootfs()
{
    log "4/5" "Packing rootfs"

    mkdir -p -- "$OUTPUT_DIR"
    if [[ "$BREAK_HARDLINKS" == true ]]; then
        local file
        local replacement

        while IFS= read -r -d '' file; do
            replacement="$file.uinxed-copy"

            cp \
                --preserve=mode,ownership,timestamps \
                --reflink=auto \
                -- "$file" "$replacement"

            mv -- "$replacement" "$file"
        done < <(
            find "$ROOTFS_DIR" \
                -xdev \
                -type f \
                -links +1 \
                -print0
        )
    fi

    (
        cd "$ROOTFS_DIR"

        find . \
            -xdev \
            \( -path './dev' -o -path './dev/*' \) \
            -prune \
            -o \
            -print0 |
            LC_ALL=C sort -z |
            cpio \
                --null \
                --create \
                --format="$CPIO_FORMAT" \
                --reproducible \
                --quiet
    ) >"$OUTPUT_FILE"

    printf 'Created: %s\n' "$OUTPUT_FILE"
    printf 'Size:    %s\n' "$(du -h "$OUTPUT_FILE" | awk '{print $1}')"
}

# ==============================================================================
# Update Limine configuration
# ==============================================================================

update_limine_config()
{
    local limine_config
    local temporary_config

    [[ "$UPDATE_LIMINE_CONFIG" == true ]] || return 0
    limine_config="$OUTPUT_DIR/Limine/limine.conf"

    [[ -f "$limine_config" ]] ||
        die "Limine configuration not found: $limine_config"
    temporary_config="$(mktemp)"

    awk '
        /module_path:[[:space:]].*\/init\.elf([^[:space:]]*)/ {
            next
        }

        /module_path:[[:space:]].*\/initramfs\.cpio([^[:space:]]*)/ {
            if (!emitted) {
                print "    module_path: boot():/initramfs.cpio"
                emitted = 1
            }
            next
        }

        {
            print
        }

        END {
            if (!emitted)
                print "    module_path: boot():/initramfs.cpio"
        }
    ' "$limine_config" >"$temporary_config"

    if ! cmp -s "$temporary_config" "$limine_config"; then
        chmod --reference="$limine_config" "$temporary_config"
        mv -- "$temporary_config" "$limine_config"
    else
        rm -f -- "$temporary_config"
    fi
}

# ==============================================================================
# Main
# ==============================================================================

main()
{
    require_commands \
        awk \
        chroot \
        cpio \
        curl \
        find \
        mktemp \
        sha256sum \
        sort \
        tar

    [[ -f "$PROJECT_ROOT/Makefile" ]] ||
        die "Not a Uinxed-Kernel source tree."

    download_rootfs
    install_packages
    configure_rootfs
    pack_rootfs
    update_limine_config

    printf '\nDone.\n'
}

main "$@"
