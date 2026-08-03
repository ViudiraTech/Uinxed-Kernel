#!/usr/bin/env bash
# Native VM/COW red-green regression for Python-style write faults.

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
test_dir=$(mktemp -d /tmp/uinxed-vm-cow-tests.XXXXXX)
trap 'rm -rf -- "$test_dir"' EXIT

cc -std=gnu11 -Wall -Wextra -Werror -O2 -fno-builtin \
    -ffunction-sections -fdata-sections -I"$root_dir/include" \
    "$root_dir/tools/tests/vm_cow_test.c" "$root_dir/mem/page.c" \
    -Wl,--gc-sections -o "$test_dir/vm_cow_test"

"$test_dir/vm_cow_test"
printf 'PASS VM/COW writable-VMA, mprotect and frame-lifetime regressions\n'
