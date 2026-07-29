#!/usr/bin/env bash
# Native USB protocol red/green regression tests.

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
test_dir=$(mktemp -d /tmp/uinxed-usb-tests.XXXXXX)

cleanup()
{
    case "$test_dir" in
        /tmp/uinxed-usb-tests.*) rm -rf -- "$test_dir" ;;
        *) printf 'Refusing to remove unexpected test path: %s\n' "$test_dir" >&2 ;;
    esac
}
trap cleanup EXIT

command -v gcc >/dev/null || {
    printf 'Missing USB test dependency: gcc\n' >&2
    exit 1
}

gcc -std=gnu11 -Wall -Wextra -Werror -I"$root_dir/include" \
    "$root_dir/tools/tests/usb_protocol_test.c" \
    "$root_dir/drivers/usb/class/storage_protocol.c" \
    "$root_dir/drivers/usb/class/hid_parser.c" \
    -o "$test_dir/usb_protocol_test"

"$test_dir/usb_protocol_test"
