#!/bin/sh
# Swap the kernel inside an existing desktop ISO and optionally layer a small
# newc archive over the Debian initramfs.
#
# The Debian rootfs archive itself is never rebuilt or rewritten: it is copied
# out of the input ISO and back in byte for byte. That is asserted below, so a
# mistake here fails the run instead of silently shipping a repacked cpio.
set -eu

usage() {
    echo "Usage: $0 INPUT.iso OUTPUT.iso [OVERLAY.cpio]" >&2
    exit 2
}

[ "$#" -ge 2 ] && [ "$#" -le 3 ] || usage

input=$(realpath "$1")
output=$(realpath -m "$2")
overlay=${3:-}

[ "$input" != "$output" ] || { echo 'Input and output must differ' >&2; exit 2; }
[ ! -e "$output" ] || { echo "Output already exists; choose a new file: $output" >&2; exit 2; }
[ "$input" != "$overlay" ] || { echo 'Overlay must not be the input ISO' >&2; exit 2; }

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kernel=$repo/UxImage
[ -f "$kernel" ] || { echo "Missing kernel image: $kernel" >&2; exit 2; }
[ -z "$overlay" ] || [ -f "$overlay" ] || { echo "Missing overlay archive: $overlay" >&2; exit 2; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

echo "== extracting $input"
xorriso -osirrox on -indev "$input" -extract / "$work/stage" >/dev/null 2>&1
chmod -R u+w "$work/stage"

[ -f "$work/stage/initramfs.cpio" ] || { echo 'Input ISO has no /initramfs.cpio' >&2; exit 2; }
[ -f "$work/stage/Limine/limine.conf" ] || { echo 'Input ISO has no Limine config' >&2; exit 2; }

base_cpio_before=$(sha256sum "$work/stage/initramfs.cpio" | cut -d' ' -f1)
echo "== base initramfs.cpio sha256 $base_cpio_before"

echo "== installing kernel $(sha256sum "$kernel" | cut -c1-16)"
cp "$kernel" "$work/stage/EFI/Boot/UxImage"

conf=$work/stage/Limine/limine.conf
# Drop any overlay lines left over from a previous pass, then attach the module
# to the same entry as the base initramfs. Appending at end of file would graft
# the module onto whatever entry happens to be last.
sed -i '/^[[:space:]]*module_path:[[:space:]]*boot():.*initramfs-overlay\.cpio$/d' "$conf"
sed -i '/^[[:space:]]*module_cmdline:[[:space:]]*initramfs-overlay$/d' "$conf"

if [ -n "$overlay" ]; then
    echo "== layering overlay $(sha256sum "$overlay" | cut -c1-16)"
    cp "$overlay" "$work/stage/initramfs-overlay.cpio"
    sed -i '/^[[:space:]]*module_cmdline:[[:space:]]*initramfs[[:space:]]*$/a\
    module_path: boot():/initramfs-overlay.cpio\
    module_cmdline: initramfs-overlay' "$conf"
fi

grep -q 'kernel_path: boot():/EFI/Boot/UxImage' "$conf" || { echo 'Limine config does not load UxImage' >&2; exit 2; }
if [ -n "$overlay" ]; then
    grep -q 'module_path: boot():/initramfs-overlay.cpio' "$conf" || { echo 'Failed to register the overlay module' >&2; exit 2; }
fi

echo "== building $output"
xorriso -as mkisofs -R -r -J -b Limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    -hfsplus -apm-block-size 2048 -efi-boot-part --efi-boot-image --protective-msdos-label \
    --efi-boot Limine/limine-uefi-cd.bin -o "$output" "$work/stage" >/dev/null

# Prove the Debian rootfs archive survived the round trip untouched.
rm -rf "$work/verify"
mkdir -p "$work/verify"
xorriso -osirrox on -indev "$output" -extract /initramfs.cpio "$work/verify/initramfs.cpio" >/dev/null 2>&1
base_cpio_after=$(sha256sum "$work/verify/initramfs.cpio" | cut -d' ' -f1)
if [ "$base_cpio_before" != "$base_cpio_after" ]; then
    echo "Refusing to ship: initramfs.cpio changed ($base_cpio_before -> $base_cpio_after)" >&2
    rm -f "$output"
    exit 1
fi

xorriso -osirrox on -indev "$output" -extract /EFI/Boot/UxImage "$work/verify/UxImage" >/dev/null 2>&1
if [ "$(sha256sum "$kernel" | cut -d' ' -f1)" != "$(sha256sum "$work/verify/UxImage" | cut -d' ' -f1)" ]; then
    echo "Refusing to ship: kernel image mismatch in $output" >&2
    rm -f "$output"
    exit 1
fi

echo "== ok: $output"
echo "   initramfs.cpio unchanged ($base_cpio_after)"
