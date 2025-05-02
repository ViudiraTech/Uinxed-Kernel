#!/bin/bash
# Generate symbol table from nm output

sym_count=1
SYM_FILE=.symbls.txt
ADDR_FILE=.addrs.txt
OUT_FILE=kallsyms.c

echo -n "const char *const symbols[] = { " > $SYM_FILE
echo -n "const __SIZE_TYPE__ addresses[] = { " > $ADDR_FILE

while read -r addr _ name; do
    # Extract the symbol name and address from the line
    ((sym_count++))
    echo -n "\"$name\"," >> $SYM_FILE
    echo -n "0x$addr," >> $ADDR_FILE
done

echo "\"(EOF)\",};" >> $SYM_FILE
echo "0xffffffffffffffff,};" >> $ADDR_FILE
echo "const __SIZE_TYPE__ sym_count = $sym_count;" > $OUT_FILE
cat $SYM_FILE $ADDR_FILE >> $OUT_FILE

rm -f $SYM_FILE $ADDR_FILE
