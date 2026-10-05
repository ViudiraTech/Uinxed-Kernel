#!/bin/sh
# Local KVM desktop run. Expose invariant TSC when the host advertises it so
# high-resolution vDSO reads can use the same clocksource as the kernel.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
iso=${DESKTOP_ISO:-$repo/build/xfce/xfce_clean.iso}
[ -r "$iso" ] || { echo "Missing desktop ISO: $iso" >&2; exit 1; }
if [ -z "${QEMU_CPU:-}" ]; then
    QEMU_CPU=host
    if grep -qw constant_tsc /proc/cpuinfo && grep -qw nonstop_tsc /proc/cpuinfo; then
        QEMU_CPU=host,+invtsc
    fi
fi
printf 'Desktop ISO: %s\nQEMU CPU: %s\n' "$iso" "$QEMU_CPU" >&2
exec qemu-system-x86_64 -machine q35 -accel kvm -cpu "$QEMU_CPU" \
    -smp "${QEMU_SMP:-4}" -m "${QEMU_MEMORY:-6G}" \
    -bios "$repo/assets/ovmf-code.fd" -cdrom "$iso" \
    -vga none -device virtio-gpu-pci -display "${QEMU_DISPLAY:-sdl}" \
    -nic user,model=e1000e -serial stdio "$@"
