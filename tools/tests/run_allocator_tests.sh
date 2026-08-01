#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

cc -std=c11 -Wall -Wextra -Werror -O2 -fno-builtin -pthread \
    -Dmalloc=uinxed_malloc -Dfree=uinxed_free \
    -Drealloc=uinxed_realloc -Daligned_alloc=uinxed_aligned_alloc \
    -Dusable_size=uinxed_usable_size \
    -I "$root_dir/include" \
    "$root_dir/tools/tests/allocator_test.c" \
    "$root_dir/mem/alloc.c" "$root_dir/mem/buddy.c" \
    -o "$build_dir/allocator_test"

"$build_dir/allocator_test"
