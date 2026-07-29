#!/usr/bin/env bash
#
#       run_fs_tests.sh
#       Native filesystem red/green regression matrix
#
#       2026/7/29 By JiTianYu391
#       Copyright (C) 2026 ViudiraTech, based on the Apache 2.0 license.
#

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
test_dir=$(mktemp -d /tmp/uinxed-fs-tests.XXXXXX)

cleanup()
{
    case "$test_dir" in
        /tmp/uinxed-fs-tests.*) rm -rf -- "$test_dir" ;;
        *) printf 'Refusing to remove unexpected test path: %s\n' "$test_dir" >&2 ;;
    esac
}
trap cleanup EXIT

for command in gcc mke2fs e2fsck debugfs mkntfs ntfsfix ntfsls ntfscat; do
    command -v "$command" >/dev/null || {
        printf 'Missing filesystem test dependency: %s\n' "$command" >&2
        exit 1
    }
done

common_cflags=(-std=gnu11 -Wall -Wextra -Werror -I"$root_dir/include")

gcc "${common_cflags[@]}" "$root_dir/tools/tests/fs_txn_test.c" \
    "$root_dir/fs/core/fs_txn.c" -o "$test_dir/fs_txn_test"
gcc "${common_cflags[@]}" "$root_dir/tools/tests/extfs_extents_test.c" \
    "$root_dir/fs/extfs/extents.c" "$root_dir/libs/data/crc32c.c" -o "$test_dir/extfs_extents_test"
gcc "${common_cflags[@]}" "$root_dir/tools/tests/extfs_inode_test.c" \
    "$root_dir/fs/extfs/inode.c" "$root_dir/fs/extfs/extents.c" "$root_dir/libs/data/crc32c.c" \
    -o "$test_dir/extfs_inode_test"
gcc "${common_cflags[@]}" "$root_dir/tools/tests/jbd2_recovery_test.c" \
    "$root_dir/fs/extfs/jbd2.c" "$root_dir/fs/core/fs_txn.c" "$root_dir/libs/data/crc32c.c" \
    -o "$test_dir/jbd2_recovery_test"
gcc "${common_cflags[@]}" "$root_dir/tools/tests/extfs_image_test.c" \
    "$root_dir/fs/extfs/super.c" "$root_dir/fs/extfs/inode.c" "$root_dir/fs/extfs/alloc.c" \
    "$root_dir/fs/extfs/dir.c" "$root_dir/fs/extfs/extents.c" "$root_dir/fs/extfs/jbd2.c" \
    "$root_dir/fs/core/fs_txn.c" "$root_dir/libs/data/crc32c.c" -o "$test_dir/extfs_image_test"
gcc "${common_cflags[@]}" -ffunction-sections -fdata-sections "$root_dir/tools/tests/ntfs_image_test.c" \
    "$root_dir/fs/core/fs_txn.c" -Wl,--gc-sections -o "$test_dir/ntfs_image_test"

"$test_dir/fs_txn_test"
"$test_dir/extfs_extents_test"
"$test_dir/extfs_inode_test"
"$test_dir/jbd2_recovery_test"

for filesystem in ext2 ext3 ext4; do
    for block_size in 1024 2048 4096; do
        image="$test_dir/${filesystem}-${block_size}.img"
        mutated="$test_dir/${filesystem}-${block_size}-mutated.img"
        truncate -s 64M "$image"
        mke2fs -q -F -t "$filesystem" -b "$block_size" "$image"
        "$test_dir/extfs_image_test" "$image"
        "$test_dir/extfs_image_test" "$image" "$mutated"
        e2fsck -f -n "$mutated" >"$test_dir/${filesystem}-${block_size}.fsck" 2>&1
        printf 'PASS %s %s-byte image and e2fsck interoperability\n' "$filesystem" "$block_size"
    done
done

xattr_image="$test_dir/ext4-xattr.img"
xattr_mutated="$test_dir/ext4-xattr-mutated.img"
truncate -s 64M "$xattr_image"
mke2fs -q -F -t ext4 "$xattr_image"
debugfs -w -R 'mkdir /xattr-victim' "$xattr_image" >/dev/null 2>&1
dd if=/dev/zero of="$test_dir/xattr-value" bs=500 count=1 status=none
debugfs -w -R "ea_set -f $test_dir/xattr-value /xattr-victim user.test" "$xattr_image" >/dev/null 2>&1
debugfs -R 'stat /xattr-victim' "$xattr_image" 2>&1 | grep -Eq 'File ACL: [1-9][0-9]*'
"$test_dir/extfs_image_test" "$xattr_image" "$xattr_mutated"
e2fsck -f -n "$xattr_mutated" >"$test_dir/ext4-xattr.fsck" 2>&1
printf 'PASS ext4 external-xattr block release and checksum\n'

htree_image="$test_dir/ext4-htree.img"
htree_mutated="$test_dir/ext4-htree-mutated.img"
truncate -s 64M "$htree_image"
mke2fs -q -F -t ext4 "$htree_image"
printf 'x\n' >"$test_dir/payload"
printf 'mkdir /many\n' >"$test_dir/debugfs.commands"
for number in $(seq -w 1 180); do
    printf 'write %s /many/file-%s\n' "$test_dir/payload" "$number" >>"$test_dir/debugfs.commands"
done
debugfs -w -f "$test_dir/debugfs.commands" "$htree_image" >/dev/null 2>&1
e2fsck -fyD "$htree_image" >"$test_dir/ext4-htree-build.fsck" 2>&1
debugfs -R 'stat /many' "$htree_image" 2>&1 | grep -q 'Flags:.*0x.*1000'
"$test_dir/extfs_image_test" "$htree_image" "$htree_mutated"
e2fsck -f -n "$htree_mutated" >"$test_dir/ext4-htree.fsck" 2>&1
printf 'PASS ext4 HTree lookup and mutation interoperability\n'

ntfs_image="$test_dir/ntfs.img"
ntfs_mutated="$test_dir/ntfs-mutated.img"
truncate -s 64M "$ntfs_image"
mkntfs -F -Q -L UINXED_TEST "$ntfs_image" >/dev/null 2>&1
"$test_dir/ntfs_image_test" "$ntfs_image" "$ntfs_mutated"
ntfsfix -n "$ntfs_mutated" >"$test_dir/ntfsfix.log" 2>&1
ntfsls -l "$ntfs_mutated" | grep -q 'native-c.txt'
test "$(ntfscat "$ntfs_mutated" /native-c.txt)" = 'native-c-ntfs'
printf 'PASS NTFS transactional image, ntfsfix, ntfsls and ntfscat interoperability\n'

printf 'PASS complete native filesystem regression matrix\n'
