#!/bin/bash
# Generate symbol table from nm output

sym_count=1
rm -f .symbls.txt .addrs.txt

while read -r addr _ name; do
    ((sym_count++))
    echo -n "\"$name\"," >> .symbls.txt
    echo -n "0x$addr," >> .addrs.txt
done

cat > kallsyms.c <<EOF
#include "stddef.h"
const size_t sym_count = $sym_count;
const char *const symbols[$sym_count] = {$(cat .symbls.txt)"(EOF)",};
const size_t addresses[$sym_count] = {$(cat .addrs.txt)0xffffffffffffffff,};
EOF

rm -f .symbls.txt .addrs.txt
